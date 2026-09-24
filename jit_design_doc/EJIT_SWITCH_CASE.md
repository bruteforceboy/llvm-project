# EJIT Switch-Case Mode — runtime-keyed arms inside a per-identity specialization

**Status**: design, not implemented
**Related**: `EJIT_FREE_DIM.md`, `EJIT_VALUE_PROFILE.md`, `EJIT_ONLINE_PGO.md`,
`EJIT_ICACHE_MULTIVERSION.md`, `PASS3_EJitWrapperGen.md`,
`PASS6_EJitStructFieldPass.md`

---

## 1. Problem

Some entries select their `may_const` data with a value that is only known at
runtime and that EJIT cannot fold today. The running example uses a cell index
and a slot number. The mechanism is generic: it applies to any parameter carrying
the attribute (§3), whatever it is called.

```c
typedef struct {
  ejit_may_const uint32_t shift;
  ejit_may_const uint32_t scale;
  ejit_may_const uint32_t mode;
} SlotCfg;

typedef struct {
  ejit_may_const uint32_t len;
  ejit_may_const int32_t  coef[4];
  ejit_may_const int32_t  clip;
  SlotCfg slot[3];
} CellCfg;

ejit_period_arr("cell") CellCfg g_cellCfg[8];

ejit_entry
int32_t process(uint8_t  ejit_dim("cell") cellIndex,
                uint32_t                  slotNo,
                const int16_t *in) {
  const CellCfg *c = &g_cellCfg[cellIndex];

  int32_t acc = 0;                               /* part A: cell only */
  for (uint32_t i = 0; i < c->len; ++i)
    acc += in[i] * c->coef[i];
  if (acc > c->clip)
    acc = c->clip;

  const SlotCfg *s = &c->slot[slotNo % 3];       /* part B: slot */
  int32_t r = acc >> s->shift;
  if (s->mode)
    r *= s->scale;
  return r + slotNo;
}
```

`cellIndex` is a specialization dimension and folds. `slotNo` does not, so every
load through `s` is declined with `non-const-offset`
(`EJitStructFieldPass.cpp:1919`). The specialization for cell 3 folds part A and
none of part B.

None of the existing parameter attributes fits `slotNo`:

| Attribute | Why not |
|---|---|
| `ejit_dim` | No period array describes the slot, so there is no declared range; values run up to about 1024, against `kEJitSharedInstances = 256` (`EJitSharedTaskPoolState.h:109`); and the slot has no activate/deactivate lifecycle of its own. The data it selects belongs to the cell's period. |
| `ejit_free_dim` | Its contract is that the data is the same for every value (`EJIT_FREE_DIM.md` §2). Here it is not. |

**Switch-case mode** keeps the per-identity model exactly as it is (one
specialization per `cellIndex`) and adds, *inside* each specialization, a small
number of **arms** keyed on the runtime value, plus a **default arm**. Only the
code that needs the key is cloned (§4.2).

Take cell 3 with `len = 4`, `coef = {1, -2, 3, 1}`, `clip = 1000`, and
`slot[0..2] = {2,3,1}, {0,1,0}, {4,5,1}`. Its promoted specialization is:

```c
int32_t process_cell3(uint8_t, uint32_t slotNo, const int16_t *in) {
  int32_t acc = in[0] - 2*in[1] + 3*in[2] + in[3];   /* part A, once */
  if (acc > 1000) acc = 1000;

  int32_t r;
  switch (slotNo % 3) {                              /* the key, §4.1 */
    case 0:  r = (acc >> 2) * 3; break;              /* arm k=0 */
    case 1:  r = acc;            break;              /* arm k=1 */
    case 2:  r = (acc >> 4) * 5; break;              /* arm k=2 */
    default: {                                       /* default arm */
      const SlotCfg *s = &g_cellCfg[3].slot[slotNo % 3];
      r = acc >> s->shift;
      if (s->mode) r *= s->scale;
    }
  }
  return r + slotNo;                                 /* slotNo stays live */
}
```

An identity has at most `min(M, A_max)` arms (§2), **never** the product of the
cell range and the slot range.

### 1.1 What does not change

The runtime dim never enters the specialization identity. So the following are
all unchanged, and switch-case adds nothing to them:

* the cache identity `(funcIndex, dims[])`, `EJitCompileRequest`, dedup;
* the inline cache: the `[D]^numDims` cell table stays keyed on the `ejit_dim`s;
* versions, publication, the generation gate;
* **deactivation.** PASS4's deactivate → modify → reactivate cycle
  (`EJitPeriodHandler.cpp`) is today's. A specialization, arms included, is one
  function, so it is invalidated and rebuilt as a whole. The runtime dim has no
  lifecycle and is never deactivated.

The default arm is the specialization an entry gets today. So every arm is an
addition on top of current behaviour, and **no state of this design runs code
less specialized than today's** (§5.1).

### 1.2 Gating

A CMake option `EJIT_SWITCH_CASE` builds the support.

* **OFF**: `ejit.o` is byte-identical to today's. Clang accepts the attribute,
  warns that it is ignored, and ignores it (safe, §3).
* **ON**: entries without the attribute get unchanged AOT output, an unchanged
  shared-state ABI and unchanged behaviour.

There is no separate AOT flag. The attribute is the per-entry opt-in.

---

## 2. Terminology

| Term | Meaning |
|---|---|
| **runtime dim** | the parameter carrying `ejit_runtime_dim`; `slotNo` in the examples |
| **projection `P`** | the function of the runtime dim through which the `may_const` addresses depend on it (§4.1) |
| **key** | `P(runtime dim)`; the value an arm is specialized for |
| **`M`** | the number of distinct keys `P` can produce; unbounded for the identity projection |
| **`A_max`** | the maximum number of arms in one specialization (§3) |
| **switch point** | where the key is computed and dispatched on; code before it is shared by all arms (§4.2) |
| **arm** | a clone of the code after the switch point, with `P` replaced by one key |
| **default arm** | the original code after the switch point, with the runtime dim left generic; today's code |
| **key set** | the keys observed for one identity, at most `A_max` (§5) |
| **promotion** | the single recompile of an identity that adds arms for its key set (§5.4) |
| **identity** | `(funcIndex, dims[])`, unchanged from today |

---

## 3. The attribute

The spelling is a placeholder:

```c
#define ejit_runtime_dim       __attribute__((ejit_runtime_dim))
#define ejit_runtime_dim_n(n)  __attribute__((ejit_runtime_dim(n)))

ejit_entry
int32_t process(uint8_t  ejit_dim("cell") cellIndex,
                uint32_t ejit_runtime_dim slotNo, const int16_t *in);
```

**Applies to** an integer parameter, at most 32 bits wide, of an `ejit_entry`
function. There may be at most one per function in v1. It cannot share a
parameter with `ejit_dim`, `ejit_free_dim` or `ejit_bound_ptr`. A 0-dim entry
may carry it too; its single identity then gets the arms.

**`A_max`** caps the arms per specialization:
* the default comes from a CMake option `EJIT_SWITCH_CASE_MAX_ARMS`, default 8.
  It is passed to both PASS3 and the runtime, because PASS3 sizes the
  per-identity record at AOT time (§5.2);
* `ejit_runtime_dim(n)` overrides it for one entry, and PASS3 sizes that entry's
  record from `n`.

**Asserts nothing.** Unlike `ejit_free_dim`, soundness does not depend on a
claim the programmer makes about the data. Every arm is guarded by its key, and
any other value runs the default arm. The attribute is an opt-in to spending code
size on this parameter.

**CodeGen** emits the tag on the function's `!ejit.metadata`, the same way
`ejit_free_dim` emits `TAG_EJIT_FREE_DIM` (`CGEJIT.cpp:121`,
`EJitCommon.h:72`). No range, modulus or period name is declared; the JIT
derives them (§4.1).

---

## 4. The specialization

### 4.1 Finding the key: projection, not the parameter

**The trap.** Under `slotNo % 3`, the arm for key 2 serves `slotNo` = 2, 5, 8, ….
So an arm must **not** substitute the parameter. Substituting it would turn
`return r + slotNo` into `return r + 2` for a call with `slotNo = 5`, which is a
miscompile. Only `P(slotNo)` is replaced, and every other use of the parameter
stays live. This is why `preReplacePeriodIndices`'s whole-parameter RAUW
(`EJitOptimizer.cpp:846`) cannot be reused.

**Detection.** Consider the `may_const` sites the first PASS6 run declined with
`non-const-offset`. For each one whose address depends on the runtime dim, walk
the variable part of the address back to the parameter. `P` is the common value
on every one of those walks, and its only non-constant leaf is the parameter:

| The runtime dim reaches the `may_const` addresses via | `P` | `M` | Arms |
|---|---|---|---|
| `v % C`, `C` constant after cell specialization | `v % C` | `C` | `min(C, A_max)` |
| `v & (2ⁿ − 1)` | `v & mask` | `2ⁿ` | `min(2ⁿ, A_max)` |
| anything else: raw `v`, a modulus that stays non-constant, different projections at different sites | `v` | unbounded | `A_max` |

The last row is always valid, because fixing the parameter fixes everything
derived from it. There, each arm substitutes the parameter itself, which is
correct because an identity-keyed arm is only entered for that exact value.

**Detection runs after the cell has been specialized**, not at AOT time. That
covers a modulus that is itself configuration: in `v % c->numSlots`, `numSlots`
is a `may_const` field, which phase 1c turns into a constant before detection
looks. A literal modulus is the easy case of the same rule.

### 4.2 Region-level arms

Arms clone only the code that needs the key, not the whole entry. Whole-entry
clones would copy the cell-only work (part A in §1, the fully unrolled loop) into
every arm. In the example that is about 80% of each body.

**The switch point** is the latest program point that:
1. dominates every key-dependent `may_const` site (§4.1); and
2. is not inside a loop. `P` depends only on a parameter, so it is invariant in
   every loop. A dominator inside a loop moves to the preheader of the outermost
   loop containing it, which is ordinary loop unswitching.

`P` is rematerialized at the switch point: its operands are the parameter and
constants.

**Cloning.** The blocks dominated by the switch point are cloned once per key. In
each clone `P` is replaced by the key; the originals become the default arm.
Blocks where control rejoins and which the switch point does not dominate stay
shared, with SSA repaired by `SSAUpdater`, as loop unswitching and jump
threading already do. Identical dominated tails, such as `return r + slotNo`,
are left for SimplifyCFG's tail merging. When the switch point is the entry
block, the region is the whole body: whole-entry cloning is the degenerate case,
not a separate mechanism.

Key-independent code *after* the switch point is still cloned with its region:
work interleaved with key-dependent loads, or a whole loop unswitched on the key.
Nothing at the IR level recovers it (§11, §12 question 4).

**Where it runs**: a new step in `EJitOptimizer::runPipeline`, between phases 1c
and 1d (`EJitOptimizer.cpp:187-194`):

1. Phases 1a–1c run as today. The cell is substituted and its constants folded;
   key-dependent loads are declined.
2. **Detect `P`** (§4.1). If the runtime dim reaches no declined site, the
   attribute has nothing to do: no arms, and the decline is logged (§7).
3. **Place the switch point and clone the region** for the key set. With an
   empty key set (before promotion), only the observation code (§5.3) is
   inserted, and only if promotion is possible for this identity.
4. Phases 1d–1f and the rest of the pipeline run **unchanged**. IPSCCP
   propagates each arm's constants into helpers, and the second PASS6 run folds
   the loads each key made addressable.

**Helpers.** The key substitution is local to each region clone, never a
module-wide RAUW:
* a helper called from two arms with different constant arguments is left
  generic by IPSCCP, which only folds arguments that agree at every call site;
* a helper reached from one arm only is specialized for that arm alone.

Neither case puts one arm's constants into another arm's code. The cost is that
a non-inlined helper shared by arms stays generic. The AOT inliner has already
expanded most callees (`EJitOptimizer.cpp:182-185`), which limits how often
this happens.

### 4.3 Dispatch

At the switch point:

```llvm
  %k = <P(%slotNo)>
  switch i32 %k, label %default.region [ i32 0, label %arm0.region
                                         i32 1, label %arm1.region ... ]
```

No table lives outside the function, nothing is written at runtime, and the
dispatch has no concurrency surface. The key needs no bounds check: the switch
default *is* the out-of-set path.

**Lowering is pinned to a compare chain.** Left to the backend, a switch with
around four or more cases may become a jump table: a bounds check, a dependent
load and an indirect branch. That is the serial load-then-branch the hit path
should not carry. v1 sets `"no-jump-tables"="true"` on the specialization, so the
switch lowers to at most `A_max` compare-and-branch pairs. The attribute is
function-wide, so it also applies to any other switch in the body; the compile
log reports when the body has one (§7). A jump table is allowed only if stage 6
measures it faster for a given `K`.

---

## 5. Collecting keys and promotion

Arms are built lazily from the keys actually observed, not from a declared
range. A range may not exist (identity projection), or may be much larger than
the set in use.

### 5.1 Lifecycle of one identity

| State | What calls run | Compared to today |
|---|---|---|
| **Not yet compiled** | AOT body: the miss path enqueues a compile and runs AOT meanwhile | Same |
| **`COLLECTING`** | The specialization with **no arms**: cell folded, key-dependent loads generic, observation code at the switch point (§5.3) | Today's code, plus the observation cost |
| **`PROMOTE_REQUESTED`** | Unchanged until the promoted code is published | Same as `COLLECTING` |
| **`PROMOTED`** | Each core's next call misses once (§5.5), then: arms for keys in the set, the default arm for others | Better for keys in the set |
| **Deactivated** (data update) | AOT during the window; afterwards a recompile **with arms immediately**, from the frozen key set | Same window; no re-collection |

**Nothing falls back to the AOT body because collection is incomplete.** The
cell's constants are available from the first compile, and waiting for the keys
would give up today's cell specialization for the whole collection window. The
cost is one pre-promotion allocation per identity (§6). §11 lists running AOT
until promotion as a future alternative.

### 5.2 The per-identity record

PASS3 emits one global per switch-case entry, `@__ejit_rtdim_<name>`, in
`.mc_shared`. It is indexed by the same `icacheLinearize` of the dims as the
icache table (`EJitSharedTaskPool.cpp:287`), and it is registered by name
alongside the icache slot. It holds one record per identity:

```c
struct EJitRtDimRecord {          // naturally aligned words only
  uint32_t state;                 // COLLECTING, PROMOTE_REQUESTED, PROMOTED
  uint32_t projTag;               // identifies P (§5.6); 0 = identity
  uint32_t count;                 // keys recorded, <= A_max
  uint32_t quiet;                 // switch-point executions since the last new key
  uint32_t keys[A_max];
};
```

It is **not** placed in `EJitSharedTaskPoolState`: that blob's layout and budget
are fixed, and adding a field would move them for every build. The storage
exists only in images that contain switch-case entries. A record is
`16 + 4·A_max` bytes. At `A_max = 8` that is 48 bytes: 768 bytes for a 1-dim
entry at `D = 16`, and 12 KiB for a 2-dim entry (§12 question 2).

Writers are single-core. Under the deployment contract the inline cache already
relies on, cores drive disjoint instance indices, so only one core executes a
given identity and writes its record. The worker only reads the record, apart
from resetting it (§5.6). So `keys[]` is written before `count`, `count` is
stored with release and read with acquire, and **no read-modify-write atomic is
needed**. That matters because JIT code must not depend on outlined atomics
(`EJitOptimizer.cpp:211-236`).

### 5.3 Observation at the switch point

Before promotion the JIT emits this at the switch point, with the record's
address baked in as a constant:

```c
uint32_t k = P(slotNo);
if (load_monotonic(&rec->state) == COLLECTING) {
  if (!contains(rec->keys, rec->count, k))       /* at most A_max compares */
    ejit_rtdim_observe(rec, k);                  /* append; may promote */
  else if (++rec->quiet == Q)                    /* plain load + store */
    ejit_rtdim_observe(rec, k);                  /* quiet trigger */
}
```

Only paths that reach a key-dependent load record a key. `ejit_rtdim_observe` is
a runtime function made visible to JIT modules the way `ejit_vp_record_scalar`
already is (`EJit.cpp:292`).

**The block is emitted only where it can lead somewhere.** Promoted code omits
it. So does any compile where promotion is impossible: sync mode (§5.4), and
entries declined for PGO or bound pointers (§9). Those identities run exactly
today's code, with no observation cost. The worker reads the compile mode when
it compiles. An identity compiled in sync mode starts collecting at its next
compile after the mode changes to async.

### 5.4 Triggers and promotion

Promotion is requested when either condition holds:

* **Full**: `count == min(M, A_max)`. Every key the arms can hold has been seen.
* **Quiet**: `quiet == Q` and `count > 0`. No new key has appeared in `Q`
  executions of the switch point. Without this, an entry whose keys in use are a
  strict subset of `M`, or whose projection is the identity, would collect
  forever.

When more than `A_max` distinct keys occur, v1 keeps the first `A_max` it sees.

`ejit_rtdim_observe` sets `PROMOTE_REQUESTED` and enqueues a request for the
identity with its current versions. It follows the shape of
`enqueueTier2ForIdentity` (`EJitSharedTaskPool.cpp:1557`), including returning
early outside async mode: compiling inline from inside JIT code would re-enter the
compiler, so **promotion is async-only**.

The worker compiles the identity as in §4.2 with the recorded key set, and
publishes. A same-identity publish already replaces the slot's pointer
(`EJitSharedTaskPool.cpp:3178-3185`). If the versions have moved on, publication
fails with `VersionMismatch` as it does today. The record stays
`PROMOTE_REQUESTED`, and the recompile after reactivation builds the arms.

### 5.5 The inline cache

`cachePublish` does not touch icache cells, so the cell for the identity keeps
the pre-promotion pointer. After a successful promotion the worker calls
`icacheDrainAll("rtdim-promote")`, the existing mechanism used for period toggles
(`EJitSharedTaskPool.cpp:948`).

**The drain is global.** It empties every entry's cells on every core, not just
this identity's. Every entry, including ones without the attribute, pays one
slow-path miss per core per promotion. The count is bounded (at most one
promotion per identity per generation), but it lands during warm-up, when many
identities promote close together. Stage 6 measures it. A per-identity cell
clear is future work (§11).

The alternative is not filling the cell until promotion, as PGO does for Tier-1
(`EJitSharedTaskPool.cpp:834-838`). That keeps the whole collection window on
the slow path and needs a new check in `icacheFill`, so v1 drains instead.

### 5.6 After promotion

* **The key set is frozen.** Every later compile of the identity, including the
  recompile after a data update, builds the arms immediately from the stored
  keys.
* **A change to `P` resets collection.** When `P` depends on configuration
  (`v % numSlots`), an update can change it. The worker compares the detected `P`
  with `projTag`. On a mismatch it resets the record to `COLLECTING` and compiles
  with no arms. Stale keys are harmless either way: dispatch always recomputes
  `P` with the current code, so an arm for a key the new `P` never produces is
  simply never selected.
* **Owner re-initialization** (a generation change) clears all records.

---

## 6. Costs

This mode **spends code to buy specialization**. It does not save code.

**Code pool.** Collection allocates nothing; the record is an AOT global. Two
facts about the pool decide what the rest costs:
* **pool memory is never released in v1.** `EJitCodePoolMemoryManager::deallocate`
  runs the dealloc actions but does not return the memory, because sealed pages
  must not be recycled (`EJitCodePoolMemoryManager.cpp:384-401`). This holds in
  every build;
* **with immediate 4K sealing, every allocation takes at least one page**
  (`EJitCodePool.cpp:243-246`). With batched page sealing, allocations share
  pages until the flush.

| Per identity | Today | Switch-case |
|---|---|---|
| Shared code (before the switch point) | 1 copy | 1 copy |
| Key-dependent region | 1 copy, generic | `K` specialized + 1 default |
| First compile | 1 allocation | 1 allocation, slightly larger for the observation block |
| Promotion | — | **+1 compile, +1 allocation.** The pre-promotion code stays resident as dead code |
| Each recompile after a data update | 1 allocation | 1 allocation, larger by the `K` arm regions |
| A promotion rejected at publish | — | Its allocation is consumed anyway, like any rejected publish today |
| Hit path | icache probe → body | icache probe → body up to the switch point → `P` → switch → arm |

So the fixed extra cost is **one dead allocation per promoted identity**: one
4 KiB page under immediate 4K sealing, as long as the pre-promotion code fits in a
page.

The recurring cost is paid on **every data update**. With `r` the size of the
key-dependent region relative to the whole body, each recompile costs about
`1 + K·r` times today's compile time and pool bytes, and none of it is ever
freed. `A_max` bounds `K`, and region cloning keeps `r` small when the switch
point is late (§12 question 4).

**Cycles.** `P` costs one `and` for a mask, and about four instructions (multiply
by reciprocal, shift, multiply-subtract) for a non-power-of-two modulus. The
compare chain costs up to `K` compare-and-branch pairs (§4.3). The arm then
skips the declined loads and the branches on them. Promotion also costs other
entries a miss through the global drain (§5.5).

**Gate.** Stage 6 (§10) measures, on SRE, cycles/call for keys that reach an arm
*and* for keys that reach the default arm, plus bytes and pool pages per
identity. An entry may use the mode only if:
* **no key is slower than today**, including keys that reach the default arm,
  which pay `P` and the full compare chain for nothing;
* the cycles saved justify the bytes added. The exchange rate is reported per
  entry, so this decision is made with the numbers in hand.

The **default-arm ratio** (§7) shows how often the arms actually serve calls. A
high ratio means `A_max` or first-seen selection is wrong for that entry.

---

## 7. Diagnostics

The implementation logs every step of the mode, so that on SRE one can see from
the logs alone:
* which entries use it;
* when each identity was promoted;
* what each arm folded;
* what it cost.

All lines use the existing `EJIT_DIAG` macros (`EJitDiag.h`) with a common
`rtdim` prefix for grepping. None is emitted from the inline observation block
or from any hit path. They come only from compiles, from `ejit_rtdim_observe`
(which runs only on a new key or a trigger), and from publication.

| Event | Level | Content |
|---|---|---|
| Entry registered | `EJIT_DIAG_VERBOSE` | function, `A_max`, record count, record bytes |
| Compile of a switch-case identity | `EJIT_DIAG` | function, dims, record state, `P` (`urem:C`, `and:mask`, `identity`), `M`, keys, arm count, switch-point block, region size, whether the body has another switch (§4.3) |
| Per arm, at compile | `EJIT_DIAG_VERBOSE` | key, `may_const` sites folded in that arm against the default arm, from PASS6's counts |
| Declined | `EJIT_DIAG`, once per function | reason: no key-dependent site, PGO, bound pointer, sync mode, attribute ignored because the option is off |
| New key recorded | `EJIT_DIAG_VERBOSE` | function, identity, key, `count`/`min(M, A_max)` |
| Promotion requested | `EJIT_DIAG` | function, identity, trigger (`full` / `quiet`), keys |
| Promotion published | `EJIT_DIAG` | function, identity, arm count, code size before and after, pool bytes consumed by this allocation |
| Promotion rejected | `EJIT_DIAG` | function, identity, reason (`VersionMismatch`, generation) |
| `P` changed, collection reset | `EJIT_DIAG` | function, identity, old and new `projTag` |

The icache drain is already logged with its reason, `rtdim-promote`.

**The default-arm ratio needs a counter, not a log**, because it is a hot-path
property. A measurement-only build flag, `EJIT_SWITCH_CASE_STATS`, adds two
counters to each record in `@__ejit_rtdim_<name>`: calls that reached an arm and
calls that reached the default arm. It also makes promoted code increment them
at the switch point, with the same single-writer plain load and store as §5.2.
The runtime prints them per identity at shutdown, or on demand. The flag is off
in every production build. No counters are added to
`EJitSharedTaskPoolState.counters`, for the ABI reason in §5.2.

---

## 8. Soundness rules

Each rule is stated where it arises; this is the checklist for review and tests.

1. **Only `P(runtime dim)` is substituted in an arm, never the parameter**,
   unless `P` is the identity (§4.1). Test with a body that uses the raw value
   (`r + slotNo`) and a call whose value differs from its key.
2. **An arm is entered only through its `case`.** Region clones have no other
   entry edge (§4.3).
3. **The key substitution is local to its region clone**, never a module-wide
   RAUW (§4.2). Test a non-inlined helper whose result differs between keys.
4. **The runtime dim never enters the identity**: not the cache key, the request
   or the icache index. This is what leaves §1.1 unchanged.
5. **Promotion never compiles from JIT code** (§5.4).

---

## 9. Exclusions

| Item | v1 |
|---|---|
| **Online PGO** | **Excluded.** Tier-1 and Tier-2 must number sites on the same CFG, and promotion changes the CFG, so a Tier-1 profile would not match a Tier-2 compile with arms. A switch-case entry stays at baseline. |
| **`ejit_bound_ptr`** | **Excluded.** The promotion request is raised from JIT code, which has no bound-pointer descriptor to attach. |
| `ejit_free_dim` | Independent. They are different parameters, and PASS6 keeps treating the free dim as today, inside every arm. |

---

## 10. Staging

| Stage | Deliverable | Gate |
|---|---|---|
| 0 | `EJIT_SWITCH_CASE` and `EJIT_SWITCH_CASE_MAX_ARMS` options; attribute in `Attr.td`, Sema rules and CodeGen tag (§3) | OFF: byte-identical `ejit.o`, attribute warns and is ignored. ON: unchanged output for entries without it. Sema tests for every rule in §3 |
| 1 | The §1 integration test, run with today's code | Baseline cycles/call, bytes/identity and pool usage to compare against |
| 2 | Projection detection, switch-point placement and region cloning with an **injected** key set (a test hook on `SpecializationContext`); compile-time log lines (§4, §7) | `slotNo = 5` reaches the `k = 2` arm and returns `r + 5`; part A appears once in the output; each row of the §4.1 table; config-field modulus; key-dependent load inside a loop (switch hoisted to the preheader); differing non-inlined helper; entry with no key-dependent site gets no arms and logs the decline |
| 3 | Record, observation, triggers, promotion, icache drain; runtime log lines (§5, §7) | Full and quiet triggers both fire; promoted code, sync-mode code and declined entries have no observation block; the log shows the promotion with before/after code size |
| 4 | Post-promotion behaviour (§5.6) | Deactivate → modify → reactivate rebuilds the arms without re-collecting; a `projTag` change resets; generation reset |
| 5 | PGO and bound-pointer exclusions (§9) | Such entries stay without arms, are not miscompiled, and log the decline |
| 6 | Measurement on the stage 1 test, on SRE, with `EJIT_SWITCH_CASE_STATS` (§7) | The §6 gate: no key slower than today, including default-arm keys; the exchange rate; the default-arm ratio; the drain cost to entries without the attribute; compare chain against jump table per `K` |

Stages 0–2 need no runtime change. Stage 2 is where correctness is decided, and
it can be tested completely with injected keys.

---

## 11. Future work

* **Run AOT until promotion.** Skip the pre-promotion compile. Collect keys on
  the AOT path, and compile the identity only once, with its arms. This saves the
  dead pre-promotion allocation (one page per identity under immediate 4K
  sealing, §6) and one compile. It costs performance during collection: the
  entry runs AOT instead of today's cell-specialized code until promotion, which
  is below today's behaviour for that window. It also has to collect without `P`,
  since a configuration-dependent modulus is only known after the cell is
  specialized. The AOT path would record raw values, and the compile would apply
  `P` to them. Worth considering if pool pages prove scarcer than collection-window
  cycles on SRE.
* **`MachineOutliner`**, for identical key-independent code left inside arms
  (§4.2). It is compiled out under `EJIT_TRIM_LLVM_BACKEND`, so restoring it costs
  runtime-library size. It also turns shared sequences in the hot path into
  calls, and its cost model guarantees a size win, not a cycle win. Consider it
  only if stage 6 shows significant duplication inside arms, and measure cycles
  as well as bytes.
* **Online PGO** for switch-case entries. This needs the key set frozen before
  Tier-1, so both tiers compile the same CFG.
* **`ejit_bound_ptr` entries.** This needs promotion routed through the next
  slow-path call, which carries the descriptor.
* **A per-identity icache clear** in place of the global drain at promotion
  (§5.5).
* More than one runtime dim per entry, and frequency-ranked key selection.

---

## 12. Open questions

1. **Real key behaviour**: is `P` always a modulus or mask by a constant or a
   configuration field? How many distinct keys does one cell see per TTI? These
   set `A_max` and `Q`.
2. **Record storage for 2-dim entries**: 12 KiB of `.mc_shared` at `A_max = 8`
   and `D = 16`. Is that acceptable, or should records be allocated only for
   identities that are actually compiled?
3. **Does a request for an already-published identity recompile?** Tier-2 does,
   but baseline requests go through batch staging
   (`EJitSharedTaskPool.cpp:5266-5281`), and it needs checking whether staging
   short-circuits a `Ready` identity. If it does, promotion needs its own request
   marker. The two tier bits have a free value (`EJitSreQueue.h:100-107`), but
   `isPublishedTier2` tests `>= kEJitTierPgoUse`, so using that value naively
   would read as Tier-2.
4. **How often is the switch point late enough?** If real entries load
   key-dependent data early, or inside their main loop, region cloning degrades
   towards whole-body cloning. The compile log reports the switch-point block and
   the region size (§7), which answers this.
