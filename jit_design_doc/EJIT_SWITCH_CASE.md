# EJIT Switch-Case Mode — runtime-keyed arms inside a per-identity specialization

**Status**: design, not implemented
**Related**: `EJIT_FREE_DIM.md`, `EJIT_ICACHE_MULTIVERSION.md`,
`EJIT_ICACHE_SHARED_TABLE.md`, `EJIT_VALUE_PROFILE.md`, `EJIT_ONLINE_PGO.md`,
`PASS3_EJitWrapperGen.md`, `PASS6_EJitStructFieldPass.md`

---

## 1. Goal and scope

### 1.1 Problem

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

### 1.2 Objective

**Fold slot-dependent data inside each cell's specialization, without a full
specialization per slot.** Each cell keeps one specialization, exactly as today.
Inside it, a small number of **arms** are keyed on the slot, plus a **default
arm**, and only the code that needs the key is cloned (§4). Cell 3 with
`len = 4`, `coef = {1, -2, 3, 1}`, `clip = 1000`, and
`slot[0..2] = {2,3,1}, {0,1,0}, {4,5,1}` becomes:

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

The gain is speed, bought with bounded extra code. Whether arms cost or save
memory depends on the baseline, so all three are measured (§8):

| Baseline | Arms against it |
|---|---|
| **Today**: one specialization per cell, slot generic | **More code.** Must be faster to justify it |
| One full specialization per `(cell, slot)` pair | **Much less code.** Not available today for `slotNo`, but it is what arms replace where it would be |
| One specialization per cell, with the slot's data copied into a small constant table in the JIT code, indexed by the key | **Usually more code.** The table cannot fold branches such as `if (s->mode)`; arms must beat it on speed |

**Two kinds of slot churn behave differently:**
* **selection churn**: calls pick slot 0, then 2, then 1. This only selects a
  different arm; nothing is compiled. The design is cheap here;
* **configuration churn**: a period handler rewrites a slot's data. That
  invalidates the whole cell specialization, and every arm is recompiled with it
  (§8.1).

### 1.3 What does not change

The runtime dim never enters the specialization identity. So the following are
all unchanged:

* the cache identity `(funcIndex, dims[])`, `EJitCompileRequest`, dedup;
* versions, publication, the generation gate;
* **deactivation.** PASS4's deactivate → modify → reactivate cycle
  (`EJitPeriodHandler.cpp`) is today's. A specialization, arms included, is one
  function, so it is invalidated and rebuilt as a whole. The runtime dim has no
  lifecycle and is never deactivated;
* **the inline cache** (`-mllvm -ejit-inline-cache`), which the design assumes
  is on, as on the board. The `[D]^numDims` cell table stays keyed on the
  `ejit_dim`s. A hit tail-calls the specialization with the original arguments,
  so the runtime dim arrives as an ordinary argument, and `EJIT_ICACHE_DIM_SIZE`
  does not bound it. The only new interaction is on the lazy path: a promotion
  must invalidate one cell (§6.6).

The default arm is the specialization an entry gets today. Arms are an addition
on top of it, and **no state of this design runs code less specialized than
today's**.

### 1.4 Gating

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
| **switch point** | where the key is computed and dispatched on; code before it is shared by all arms (§4.3) |
| **arm** | a clone of the code after the switch point, with `P` replaced by one key |
| **default arm** | the original code after the switch point, with the runtime dim left generic; today's code |
| **eager path** | all `M` arms built in the first compile (§5.2) |
| **lazy path** | arms chosen from observed keys, then added by one promotion (§6) |
| **promotion** | the recompile of an identity that adds the lazy path's arms |
| **epoch** | the lifecycle stamp of a lazy-path collection (§6.3) |
| **identity** | `(funcIndex, dims[])`, unchanged from today |

---

## 3. The attribute and its limits

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

**Asserts nothing.** Unlike `ejit_free_dim`, soundness does not depend on a
claim the programmer makes about the data. Every arm is guarded by its key, and
any other value runs the default arm. The attribute is an opt-in to spending code
size on this parameter.

**CodeGen** emits the tag on the function's `!ejit.metadata`, the same way
`ejit_free_dim` emits `TAG_EJIT_FREE_DIM` (`CGEJIT.cpp:121`,
`EJitCommon.h:72`). No range, modulus or period name is declared; the JIT
derives them (§4.1).

**Limits.** Arm count alone is not a cost bound: eight tiny arms and eight
cloned loops differ by orders of magnitude. So v1 has four limits, as CMake
options passed to both PASS3 and the runtime:

| Option | Bounds | Default |
|---|---|---|
| `EJIT_SWITCH_CASE_MAX_ARMS` (`A_max`) | arms per specialization; overridable per entry with `ejit_runtime_dim(n)` | 8 |
| `EJIT_SWITCH_CASE_MAX_REGION` | IR instructions in one arm's region; above it the entry gets no arms | set from the stage 3 sweep (§8.2) |
| `EJIT_SWITCH_CASE_MAX_CLONED` | IR instructions added by cloning, summed over all arms of one compile; bounds compile work | set from the stage 3 sweep |
| `EJIT_SWITCH_CASE_MAX_PROMOTIONS` | promotions per identity per generation (§6.4) | 2 |

A fifth check, the **pool check**, is not an option. Before the worker links a
specialization with arms, it compares the object's size with the pool's free
space. If there is not enough, it compiles without arms instead (§6.4).

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
on every one of those walks, and its only non-constant leaf is the parameter.
v1 recognizes exactly these forms:

| Form | `P` | `M` | Path |
|---|---|---|---|
| `urem v, C`, with `C` constant after cell specialization | `v % C` | `C` | eager if within limits, else lazy |
| `and v, 2ⁿ − 1` | `v & mask` | `2ⁿ` | eager if within limits, else lazy |
| anything else | `v` (identity) | unbounded | lazy |

In these forms, `v` is the parameter or a `zext` of it, and the operation is
unsigned. `srem` and `sdiv`, which can produce negative keys, and any `trunc` or
`sext` on the path, fall to the identity row.

The identity row is always valid, because fixing the parameter fixes everything
derived from it, and an identity-keyed arm is only entered for that exact value.
Valid is not the same as useful, though. `table[slotNo + runtimeOffset]` keeps a
dynamic offset whatever `slotNo` is. So **an arm is kept only if it folds more
than the default arm.** The second PASS6 run reports a per-arm count of
resolved sites; an arm that resolves nothing extra is dropped, and a compile
with no arm left gets none.

**Detection runs after the cell has been specialized**, not at AOT time. That
covers a modulus that is itself configuration: in `v % c->numSlots`, `numSlots`
is a `may_const` field, which phase 1c turns into a constant before detection
looks.

### 4.2 Supported form

v1 transforms only a deliberately narrow form, and declines everything else,
logging the reason (§7):

1. **Entry-local sites.** The key-dependent sites are in the entry function
   itself. Sites in non-inlined helpers have no common dominator with the entry
   and stay generic. If every site is in a helper, the entry gets no arms.
2. **A recognized projection** from the §4.1 table.
3. **A rematerializable address chain.** Every instruction between `P` and the
   key-dependent loads (the `urem`, the GEPs, any casts) must be speculatable and
   side-effect free. It is **rematerialized inside each region**, so that an
   address computed before the switch point does not stay on the original,
   variable `P`, which would block folding in the arms.
4. **Supported control flow.** Loops are in LoopSimplify form, with a preheader.
   The region has no `invoke`, `callbr` or `indirectbr`, and no edges other than
   ordinary branches, switches and returns.
5. **Within limits.** Each region is at most `EJIT_SWITCH_CASE_MAX_REGION`, and
   the total cloned is at most `EJIT_SWITCH_CASE_MAX_CLONED` (§3).

### 4.3 Region-level arms

Arms clone only the code that needs the key, not the whole entry. Whole-entry
clones would copy the cell-only work (part A in §1.1, the fully unrolled loop)
into every arm.

**The switch point** is the latest program point that:
1. dominates every key-dependent site; and
2. is not inside a loop. A dominator inside a loop moves to the preheader of the
   outermost loop containing it, which is ordinary loop unswitching.

Hoisting is legal because §4.2 rule 3 makes `P` and the address chain
speculatable. It can run `P`, and the lazy path's observation, on paths that
never reach a key-dependent load, including zero-trip loops. That costs cycles
and skews the lazy path's counts, but it is never incorrect.

**Cloning.** The blocks dominated by the switch point are cloned once per key. In
each clone, `P` and the rematerialized address chain use the key; the originals
become the default arm. Blocks where control rejoins and which the switch point
does not dominate stay shared, with SSA repaired by `SSAUpdater`, as loop
unswitching and jump threading already do. When the switch point is the entry
block, the region is the whole body. Whole-entry cloning is the degenerate case,
not a separate mechanism.

Key-independent code *after* the switch point is still cloned with its region:
work interleaved with key-dependent loads, or a whole loop unswitched on the key.
The region limits bound it, and nothing at the IR level recovers it (§10).

**Helpers.** The key substitution is local to each region clone, never a
module-wide RAUW:
* a helper called from two arms with different constant arguments is left
  generic by IPSCCP, which only folds arguments that agree at every call site;
* a helper reached from one arm only is specialized for that arm alone.

Neither case puts one arm's constants into another arm's code.

**Where it runs**: a new step in `EJitOptimizer::runPipeline`, between phases 1c
and 1d (`EJitOptimizer.cpp:187-194`):

1. Phases 1a–1c run as today. The cell is substituted and its constants folded;
   key-dependent loads are declined.
2. Detect `P` (§4.1) and check the supported form (§4.2).
3. Choose the path and the keys (§5), place the switch point and clone the
   regions. On the lazy path before promotion, there are no keys, and only the
   observation code (§6.2) is inserted.
4. Phases 1d–1f and the rest of the pipeline run unchanged over all arms. Then
   the per-arm fold counts decide which arms survive (§4.1). Dropping an arm
   means redirecting its `case` to the default region; SimplifyCFG deletes the
   dead clone.

### 4.4 Dispatch

At the switch point:

```llvm
  %k = <P(%slotNo)>
  switch i32 %k, label %default.region [ i32 0, label %arm0.region
                                         i32 1, label %arm1.region ... ]
```

No table lives outside the function, nothing is written at runtime, and the
dispatch has no concurrency surface. The key needs no bounds check: the switch
default *is* the out-of-set path.

**Lowering is chosen by measurement, not fixed.** Left to the backend, a switch
with around four or more cases may become a jump table: a dependent load and an
indirect branch. A compare chain avoids those, but its cost depends on key
distribution, branch prediction, arm size and layout, and a rapidly varying key
can favour either. The levers are limited. `"no-jump-tables"` forbids jump tables
but does not fix the shape of what replaces them, and it applies to every switch
in the function. So v1 uses the backend's choice, and stage 3 (§9) measures it
against the alternatives across `K` and key distributions before any lever is
set.

---

## 5. Choosing arms

### 5.1 Policy

At every compile of an identity, after §4.1–§4.2:

1. **Declined** (unsupported form, no key-dependent site): no arms.
2. **Eager** when `M` is finite, `M <= A_max`, and `M` arms fit the region and
   cloning limits.
3. **Lazy** otherwise: `M` unbounded, or too many arms for the limits. This
   applies only where promotion is possible: async compile mode, and the entry
   not excluded (§10).
4. Otherwise, no arms.

The choice is remade at every compile, so a configuration update that changes
`P` (a new `numSlots`) simply moves the identity to whichever path now applies.

**A cell specialization has one set of arms**, at most `A_max`. Keys without an
arm take the default arm; there is no second batch. A re-promotion replaces the
set rather than adding to it.

### 5.2 Eager path

The first compile builds all `M` arms. There is no observation, no record, no
promotion, and nothing new at the inline cache. The identity's lifecycle is
exactly today's, with arms in the code from the start. For `% 3`, this is the
whole feature.

---

## 6. Lazy path

### 6.1 Lifecycle

| State | What calls run |
|---|---|
| Not yet compiled | AOT body, as today |
| `COLLECTING` | The specialization without arms, with observation (§6.2) |
| `FROZEN` | Same as `COLLECTING`; keys chosen, promotion being requested |
| `QUEUED` | Same; promotion compile in flight |
| `PROMOTED` | Arms for the chosen keys, the default arm for others. Every later compile, including after a data update, builds the arms immediately |
| `KEEP_BASELINE` | The specialization without arms, and without observation from its next compile on. Terminal for the epoch |

**Nothing falls back to the AOT body because collection is incomplete.** Waiting
for the keys would give up today's cell specialization for the whole collection
window. The cost is one pre-promotion allocation per identity (§8.1). §10 lists
running AOT until promotion as a future alternative.

### 6.2 Observation

The lazy path collects **frequencies over a bounded window**, not first-seen
keys. The domain is large or unknown there, and the keys seen at startup need not
be the keys that dominate later.

Before promotion the JIT emits this at the switch point, with the record's
address and the current epoch baked in as constants:

```c
uint32_t n = load_relaxed(&rec->calls) + 1;       /* lost updates tolerated */
store_relaxed(&rec->calls, n);
if ((n & (S - 1)) == 0 && load_relaxed(&rec->state) <= FROZEN)
  ejit_rtdim_observe(rec, EPOCH, P(slotNo));      /* every S-th call */
```

JIT code only increments a sampling counter and reads `state`. Concurrent callers
may lose increments, which shifts the sampling phase but corrupts nothing. Every
other change to the record happens inside `ejit_rtdim_observe`. That is a runtime
function made visible to JIT modules the way `ejit_vp_record_scalar` already is
(`EJit.cpp:292`), and it takes the record lock (§6.3). So the design does
**not** rely on one core per identity. This covers 0-dim entries, which several
cores execute by design (`EJIT_ICACHE_SHARED_TABLE.md` §P1a), as well as
overlapping tasks and migration. The observation code uses no read-modify-write
atomic, so it cannot depend on outlined atomics (`EJitOptimizer.cpp:211-236`).

`ejit_rtdim_observe`:
1. `tryWrite` on the record lock. If it is busy, drop the sample. The call path
   never waits, so interrupt reentry on the same core cannot deadlock.
2. If the passed epoch differs from `rec->epoch`, drop the sample: it comes from
   code compiled for an older collection.
3. Count the key in a heavy-hitter table of `2·A_max` entries (space-saving).
4. After `W` samples, close the window. Take the top `A_max` keys. If their
   share of the window reaches the coverage threshold `C`, **freeze** them and
   request promotion (§6.4). Otherwise clear the table and start another window;
   after `R` windows below threshold, go to `KEEP_BASELINE`.

`S`, `W`, `C` and `R` are runtime-configurable, with defaults set from stage 4
data. Open question 1 asks what the real key distributions are.

### 6.3 The record and its epoch

PASS3 emits, per switch-case entry, `@__ejit_rtdim_<name>` in `.mc_shared`,
registered by name alongside the icache slot. It holds a byte index over the
identities (by `icacheLinearize`, `EJitSharedTaskPool.cpp:287`) into a small pool
of records. The worker assigns a record when it first compiles an identity on the
lazy path, and bakes its address into the code. Eager and declined identities
never take one. A full pool puts the identity in `KEEP_BASELINE`, logged.

```c
struct EJitRtDimRecord {
  EJitRwLock lock;                 // runtime only; tryWrite on the call path
  uint32_t epoch;                  // bumped on every reset
  uint32_t state;                  // §6.1
  uint32_t projTag;                // identifies P
  uint32_t calls;                  // sampling counter, written by JIT code
  uint32_t samples, windows, attempts, promotions;
  struct { uint32_t key, count; } table[2 * A_max];
  uint32_t frozenCount;
  uint32_t frozen[A_max];
};
```

It is **not** placed in `EJitSharedTaskPoolState`, whose layout and budget are
fixed for every build. At `A_max = 8` a record is about 200 bytes. The pool
size, which bounds how many identities of one entry can use the lazy path, is
open question 2.

**The epoch is the collection's lifecycle.** Under the record lock:
* **reset** bumps `epoch`, clears the table, `samples`, `windows` and `attempts`,
  and sets `projTag`. Only the worker resets, at the start of a compile, when the
  detected `P` differs from `projTag`, or on a generation change;
* **a compile snapshot** reads `epoch`, `state` and the frozen keys together.
  After a reset, code that is still running carries the old epoch, and step 2 of
  §6.2 drops its samples. So old-projection keys never enter a new collection;
* **a state transition** caused by a compile (published, rejected, failed)
  applies only if `rec->epoch` still equals the snapshot's epoch. Otherwise the
  result is published or discarded as usual, but the record is left to the newer
  collection. Stale keys in published code are harmless: dispatch always
  recomputes `P` with that code's own projection.

### 6.4 Promotion state machine

Every exit has an explicit transition:

| From | Event | To |
|---|---|---|
| `COLLECTING` | window closes at or above coverage `C` | `FROZEN` |
| `COLLECTING` | `R` windows below `C` | `KEEP_BASELINE` |
| `FROZEN` | enqueue succeeds | `QUEUED` |
| `FROZEN` | queue full, dedup declines, or not async | stay `FROZEN`; retry on the next sample; `attempts++` |
| `FROZEN` | `attempts` exceeds its limit | `KEEP_BASELINE` |
| `QUEUED` | published | `PROMOTED`; `promotions++`; clear the cell (§6.6) |
| `QUEUED` | `VersionMismatch` or generation mismatch | `FROZEN` (the recompile after the data update also builds the arms) |
| `QUEUED` | compile or seal failure, or the pool check fails (§3) | `KEEP_BASELINE` |
| any | `P` changes and `promotions < EJIT_SWITCH_CASE_MAX_PROMOTIONS` | reset (§6.3) → `COLLECTING` |
| any | `P` changes, promotion budget spent | `KEEP_BASELINE` |
| any | generation change | record cleared |

"Keys frozen" and "request queued" are separate states, so a failed enqueue
never strands an identity. The promotion budget counts every promotion in the
generation, including re-promotions after `P` changes. That also bounds the
identity's lifetime pool growth and its cell clears.

Promotion is **async-only**: `ejit_rtdim_observe` never compiles, because
compiling inline from inside JIT code would re-enter the compiler.

### 6.5 Publication

The promotion request is a baseline request for the identity, carrying a
**promotion marker**. The worker must not send it through batch staging.
`cacheStagePending` takes the slot matching the identity even when it is
`Ready`, stores the new pointer as `Pending`, and releases the old function
(`EJitSharedTaskPool.cpp:2932-3001`). Callers would lose today's specialization
until the batch is flushed, and nothing makes a flush prompt.

So a marked request takes the direct path. The worker compiles, finalizes the
code so it is executable, then replaces the `Ready` slot's pointer through
`cachePublish` (`EJitSharedTaskPool.cpp:3178-3185`). The old specialization keeps
serving until that store. The inline cache is on, so no code releaser is wired,
and the old code stays executable for callers still inside it.

How the marker is carried is open question 3.

### 6.6 Inline cache: scoped cell clear

A hit jumps straight into the specialization, so after promotion the identity's
cell keeps the pre-promotion pointer until it is cleared. The global
`icacheDrainAll` would do that, but it also evicts every unrelated hot entry on
every core. Staggered promotions during warm-up would repeat that eviction. So
v1 clears **one cell**, using the drain's existing protocol
(`EJitSharedTaskPool.cpp:415`):

1. `icacheDrainsInFlight.fetchAdd(1)`;
2. store the table's **empty value** into the cell at
   `icacheLinearize(dims)`: `&MissFn` for sentinel-form tables, whose probe
   branches through the cell unconditionally, and 0 for guarded tables;
3. bump `icacheDrainSeq` and retire the drain.

A fill that resolved concurrently sees the drain and retracts itself, exactly as
it does against a full drain, so the old pointer cannot be written back after the
clear. `icacheArmed` is left alone. Promotions published in one worker step share
one bracket. For 0-dim entries the fill stays gated by
`icacheCrossCoreExecutable()`, as today.

---

## 7. Diagnostics

Every step is logged, so that on SRE the logs alone show:
* which entries use the mode, and which path each identity takes;
* when an identity was promoted, and what each arm folded;
* what it cost.

All lines use the existing `EJIT_DIAG` macros (`EJitDiag.h`) with a common
`rtdim` prefix. None comes from JIT code or a hit path. They come only from
compiles, from `ejit_rtdim_observe` (every `S`-th call at most), and from
publication.

| Event | Level | Content |
|---|---|---|
| Entry registered | `EJIT_DIAG_VERBOSE` | function, limits, record pool size |
| Compile of an identity | `EJIT_DIAG` | function, dims, path (`eager` / `lazy` / `none`), `P` (`urem:C`, `and:mask`, `identity`), `M`, keys, arms kept, switch-point block, region and cloned sizes, whether the body has another switch |
| Per arm | `EJIT_DIAG_VERBOSE` | key, sites resolved against the default arm; `dropped` if none |
| Declined | `EJIT_DIAG`, once per function | reason: which §4.2 rule failed, a limit, PGO, bound pointer, sync mode, attribute ignored because the option is off |
| Window closed | `EJIT_DIAG` | function, identity, epoch, samples, top keys, coverage, outcome |
| State transition | `EJIT_DIAG` | function, identity, epoch, from → to, cause (every row of §6.4) |
| Promotion published | `EJIT_DIAG` | function, identity, arm count, code size before and after, pool bytes of this allocation |
| Reset | `EJIT_DIAG` | function, identity, old and new epoch, old and new `projTag` |
| Cell clear | `EJIT_DIAG_VERBOSE` | function, identity, whether the cell held a pointer |

**Coverage needs counters, not logs**, because it is a hot-path property. A
measurement-only build flag, `EJIT_SWITCH_CASE_STATS`, makes the switch point
count arm and default-arm executions per identity, in a side table beside the
record. The runtime prints them at shutdown or on demand. The flag is off in
production and **off when cycles are measured** (§8.2).

---

## 8. Costs and measurement

### 8.1 Costs

This mode spends code to buy speed. Two facts about the code pool decide the
memory side:
* **pool memory is never released in v1.** `EJitCodePoolMemoryManager::deallocate`
  runs the dealloc actions but does not return the memory, because sealed pages
  must not be recycled (`EJitCodePoolMemoryManager.cpp:384-401`);
* **with immediate 4K sealing, every allocation takes at least one page**
  (`EJitCodePool.cpp:243-246`). With batched page sealing, allocations share
  pages until the flush.

| Per identity | Today | Eager | Lazy |
|---|---|---|---|
| Key-dependent region | 1 generic copy | `M` specialized + 1 default | `K` specialized + 1 default, after promotion |
| Compiles | 1 | 1 | 2 (initial + promotion) |
| Dead allocations | — | — | 1: the pre-promotion code, never freed. One page under immediate 4K sealing |
| Each recompile after a data update | 1 allocation | 1, larger by the arm regions | 1, larger by the arm regions |
| Hit path | icache probe → body | probe → body to the switch point → `P` → dispatch → arm | same as eager, after promotion; before it, plus the sampling counter |

`1 + K·r` times today's size, with `r` the region's share of the body, is a
**rough structural estimate only**. Cloning changes what the rest of the pipeline
does: unrolling, inlining, register allocation and layout all respond to function
size. So neither compile time nor allocation size is predicted from it, and "the
default arm is today's code" does not mean it performs like today's compiled
specialization. Both are measured.

### 8.2 Measurement

All measurements are taken on SRE, with the inline cache on. The gate checks
that `-mllvm -ejit-inline-cache` is in the AOT compile command itself; a runtime
cache entry alone does not emit the probe. Cycles are measured with
`EJIT_SWITCH_CASE_STATS` off, and coverage separately with it on.

**Region-size sweep (stage 3).** Vary the switch point's position and the region
size in a synthetic entry: early, late, inside the main loop. Record emitted code
and cycles per path. The results set `EJIT_SWITCH_CASE_MAX_REGION` and
`EJIT_SWITCH_CASE_MAX_CLONED`, and decide the dispatch lowering (§4.4).

**Gates (stage 7).** For each entry, against the three baselines of §1.2:

| Metric | Gate |
|---|---|
| Cycles/call, keys that reach an arm | improvement of at least `G_arm` over today |
| Cycles/call, keys that reach the default arm | regression of at most `G_default` over today |
| Tail latency: worst-case call over the run, including compile, promotion and cell-clear windows | at most `G_tail` over today |
| Bytes and pool pages per identity | reported against each baseline |
| Coverage (lazy path) | reported: share of calls served by arms |

`G_arm`, `G_default` and `G_tail` are fixed from the stage 1 baseline **before**
stage 7 runs, so they are not tuned to the result. An entry that misses a gate
should not use the attribute.

---

## 9. Staging

| Stage | Deliverable | Gate |
|---|---|---|
| 0 | `EJIT_SWITCH_CASE` and limit options; attribute in `Attr.td`, Sema rules and CodeGen tag (§3) | OFF: byte-identical `ejit.o`, attribute warns and is ignored. ON: unchanged output for entries without it. Sema tests for every rule in §3 |
| 1 | The §1.1 integration test and a constant-table variant, run with today's code, inline cache on | Baselines for §8.2; gate values `G_*` fixed |
| 2 | Projection detection, supported-form checks, region cloning, **eager path**, compile-time logs (§4, §5, §7) | End to end for `% 3`, with no runtime change. `slotNo = 5` reaches arm 2 and returns `r + 5`; part A appears once; each §4.1 form, including `srem` falling to identity; config-field modulus; address chain above the switch point rematerialized; key-dependent load behind a condition in a loop (hoisted, still correct on zero-trip); helper-only sites declined; arm with no extra folds dropped; every §4.2 decline logged |
| 3 | Region-size sweep; dispatch lowering comparison (§4.4, §8.2) | Limits set; lowering chosen |
| 4 | Lazy path: record pool, sampling, windows, epoch, state machine (§6.2–§6.4) | gtest: concurrent observers on one record, including a 0-dim entry, lose no correctness; old-epoch samples after a reset are dropped; every §6.4 transition, including queue full, dedup decline, sync mode, seal failure and pool-check failure; promotion budget honoured |
| 5 | Promotion publication and scoped cell clear (§6.5, §6.6) | Marked requests never reach `cacheStagePending`; the old specialization serves until the replacement is executable; a fill racing the clear retracts; unrelated entries' cells are untouched |
| 6 | PGO and bound-pointer exclusions (§10) | Such entries get no arms, are not miscompiled, and log the decline |
| 7 | Measurement on SRE (§8.2) | The gates |

Stage 2 delivers the eager path end to end with no runtime change, and it is
where correctness is decided.

---

## 10. Exclusions and future work

**Excluded in v1:**
* **Online PGO.** Tier-1 and Tier-2 must number sites on the same CFG. Arms
  change the CFG, so a switch-case entry stays at baseline.
* **`ejit_bound_ptr` entries.** The promotion request is raised from JIT code,
  which has no bound-pointer descriptor to attach.

`ejit_free_dim` is independent: it is a different parameter, and PASS6 keeps
treating it as today, inside every arm.

**Future work:**
* **Run AOT until promotion.** Skip the lazy path's pre-promotion compile and
  collect on the AOT path instead. This saves the dead allocation and a compile.
  It costs performance during collection, since the entry runs below today's
  code until promotion. It also has to collect raw values, since a
  configuration-dependent `P` is only known after the cell is specialized.
  Worth considering if pool pages prove scarcer than collection-window cycles.
* **`MachineOutliner`**, for identical key-independent code left inside arms
  (§4.3). It is compiled out under `EJIT_TRIM_LLVM_BACKEND`, so restoring it costs
  runtime-library size. It also turns shared hot-path sequences into calls, and it
  only guarantees a size win. Consider it only if stage 7 shows significant
  duplication, measuring cycles as well as bytes.
* **More than one batch of arms per cell.** Today, keys that do not fit in the
  `A_max` arms fall to the default arm. A later version could compile further
  batches of arms for those keys, so more of them get specialized code. Each
  batch costs a recompile and pool space that is never freed, so this should be
  driven by the measured coverage (§8.2).
* **Adapting frozen keys** when the dominant keys shift after promotion. Like
  extra batches, any re-selection needs its own lifetime allocation budget,
  because each one leaves unreclaimable code behind.
* Online PGO and `ejit_bound_ptr` support; more than one runtime dim per entry;
  sites in non-inlined helpers.

---

## 11. Soundness rules

The checklist for review and tests; each rule is stated where it arises.

1. **Only `P(runtime dim)` and its address chain are substituted in an arm,
   never the parameter**, unless `P` is the identity (§4.1).
2. **An arm is entered only through its `case`** (§4.4).
3. **The key substitution is local to its region clone**, never a module-wide
   RAUW (§4.3).
4. **The runtime dim never enters the identity**: not the cache key, the request
   or the icache index (§1.3).
5. **Record state changes only under the record lock, and only for a matching
   epoch** (§6.3).
6. **Promotion never compiles from JIT code, and never goes through batch
   staging** (§6.4, §6.5).

---

## 12. Open questions

1. **Real key behaviour.** Is `P` usually a modulus or mask, by a constant or a
   configuration field? How are the keys distributed per cell per TTI? This
   decides how often the eager path applies, and sets `S`, `W`, `C` and `R`.
2. **Record pool size per entry.** It bounds how many identities of one entry can
   use the lazy path at about 200 bytes each.
3. **The promotion marker.** It must reach the worker without an ABI change.
   Two candidates:
   * a high bit of `numDims`, mirroring the tier in `funcIndex`'s top bits
     (`EJitSreQueue.h:100-107`). Every `numDims` consumer must then mask it;
   * the free tier value. But `isPublishedTier2` tests `>= kEJitTierPgoUse`, so
     that value would read as Tier-2 unless those comparisons are audited.
4. **How often is the switch point late enough?** If real entries load
   key-dependent data early, or inside their main loop, regions approach the
   whole body and the limits decline them. The compile log's switch-point block
   and region size (§7) answer this on real entries.
