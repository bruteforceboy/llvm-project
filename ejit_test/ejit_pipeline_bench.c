/**
 * EJIT pipeline benchmark — where does the time actually go?
 *
 * Three separate costs get conflated when people say "EJIT is slow". This
 * bench separates them and reports each in its own unit:
 *
 *   A. DISPATCH   ns per call spent getting INTO the specialization
 *                 (funcIndex load -> compile_or_get_Nd -> indirect call ->
 *                 release_read). Paid on EVERY call, forever. Measured with a
 *                 near-empty body so the body cannot hide it.
 *   B. DISTANCE   AOT vs JIT steady-state speed of the body itself, swept over
 *                 body size, so the break-even point is visible rather than
 *                 assumed. Two shapes: leaf (AOT can hoist the may_const loads
 *                 out of the loop) and call-crossing (it cannot).
 *   C. COMPILE    ms of one cold compile — the one-time cost that dispatch and
 *                 distance are amortising against.
 *
 * Run: ./ejit_test/out/ejit_pipeline_bench [calls] [cold_compiles]
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ejit_test_helpers.h"

extern void ejit_shutdown(void);

//===-- Config --------------------------------------------------------------===//

#define NCELL 32

struct Cfg {
  ejit_may_const uint32_t kind;   // branch selector
  ejit_may_const uint32_t mul;    // multiplier
  ejit_may_const uint32_t add;    // addend
  ejit_may_const uint32_t shift;  // divisor-ish
  uint32_t sink;                  // NOT may_const
};

ejit_period_arr(cell) struct Cfg g_cfg[NCELL];

// 0-dim entry lives on a static-period struct: exercises compile_or_get_0d,
// the cheapest lookup shape there is.
struct Cfg0 {
  ejit_may_const uint32_t kind;
  ejit_may_const uint32_t mul;
};
ejit_period(static) struct Cfg0 g_cfg0;

//===-- Shared body ---------------------------------------------------------===//
//
// One macro so the JIT and no-attribute variants cannot drift apart. `w` is a
// plain runtime argument, NOT may_const — the JIT specializes the fields, never
// the trip count, which is what an embedded caller actually looks like.

#define BODY(P, W)                                                             \
  do {                                                                         \
    for (uint32_t i = 0; i < (W); i++) {                                       \
      if ((P)->kind == 0xFD)                                                   \
        s += (uint64_t)(P)->mul * (i + (P)->add) / ((P)->shift + 1);           \
      else if ((P)->kind == 0xEC)                                              \
        s += (uint64_t)((P)->mul + (P)->add) * i;                              \
      else                                                                     \
        s += ((P)->mul ^ (P)->add) + i;                                        \
    }                                                                          \
  } while (0)

//===-- A/B: leaf entry (1 dim) ---------------------------------------------===//

ejit_entry
uint64_t leaf_jit(ejit_period_arr_ind(cell) uint8_t ci, uint32_t w)
{
  struct Cfg *p = &g_cfg[ci];
  uint64_t s = 0;
  BODY(p, w);
  return s;
}

uint64_t leaf_aot(uint8_t ci, uint32_t w)
{
  struct Cfg *p = &g_cfg[ci];
  uint64_t s = 0;
  BODY(p, w);
  return s;
}

//===-- A: 0-dim entry ------------------------------------------------------===//

ejit_entry
uint64_t zerodim_jit(uint32_t w)
{
  uint64_t s = 0;
  for (uint32_t i = 0; i < w; i++)
    s += (uint64_t)g_cfg0.mul * i + g_cfg0.kind;
  return s;
}

uint64_t zerodim_aot(uint32_t w)
{
  uint64_t s = 0;
  for (uint32_t i = 0; i < w; i++)
    s += (uint64_t)g_cfg0.mul * i + g_cfg0.kind;
  return s;
}

//===-- B: call-crossing shape ----------------------------------------------===//
//
// noinline defeats the AOT hoist. Without interprocedural propagation the
// callee re-loads all four may_const fields and re-tests both guards per
// iteration; with it the callee folds. Do NOT "simplify" by dropping noinline —
// that silently turns this section into a copy of the leaf section.

__attribute__((noinline))
static uint64_t step(uint8_t ci, uint64_t s, uint32_t i)
{
  struct Cfg *p = &g_cfg[ci];
  if (p->kind == 0xFD)
    return s + (uint64_t)p->mul * (i + p->add) / (p->shift + 1);
  if (p->kind == 0xEC)
    return s + (uint64_t)(p->mul + p->add) * i;
  return s + ((p->mul ^ p->add) + i);
}

ejit_entry
uint64_t call_jit(ejit_period_arr_ind(cell) uint8_t ci, uint32_t w)
{
  uint64_t s = 0;
  for (uint32_t i = 0; i < w; i++)
    s = step(ci, s, i);
  return s;
}

uint64_t call_aot(uint8_t ci, uint32_t w)
{
  uint64_t s = 0;
  for (uint32_t i = 0; i < w; i++)
    s = step(ci, s, i);
  return s;
}

//===-- C: cold-compile driver ----------------------------------------------===//
//
// Distinct cell index == distinct cacheKey == distinct cold compile of the same
// bitcode, so N cells give N independent compiles.

ejit_entry
uint32_t cold_jit(ejit_period_arr_ind(cell) uint8_t ci)
{
  return g_cfg[ci].kind + g_cfg[ci].mul;
}

//===-- Timing --------------------------------------------------------------===//

static double now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e9 + ts.tv_nsec;
}

static volatile uint64_t g_sink;

static int cmp_dbl(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}

//===-- Stats checkpoints ---------------------------------------------------===//
//
// A JIT benchmark that does not check the counters is measuring AOT and calling
// it JIT. Every phase reports what the taskpool actually did.

#ifdef EJIT_SRE_SHARED_TASKPOOL
static ejit_taskpool_stats_t g_prev;
static void phase(const char *what) {
  ejit_taskpool_stats_t s;
  memset(&s, 0, sizeof(s));
  ejit_taskpool_get_stats(&s);
  printf("  [%s] compiles+%llu enq+%llu pending+%llu failed+%llu "
         "disabled+%llu hits+%llu ready=%u\n",
         what,
         (unsigned long long)(s.asyncCompiles - g_prev.asyncCompiles),
         (unsigned long long)(s.asyncEnqueues - g_prev.asyncEnqueues),
         (unsigned long long)(s.alreadyPending - g_prev.alreadyPending),
         (unsigned long long)(s.compileFailed - g_prev.compileFailed),
         (unsigned long long)(s.instanceDisabled - g_prev.instanceDisabled),
         (unsigned long long)(s.cacheHits - g_prev.cacheHits),
         s.readyEntries);
  g_prev = s;
}
#else
static void phase(const char *what) { (void)what; }
#endif

//===-- Helpers -------------------------------------------------------------===//

static void init_cells(void) {
  for (int i = 0; i < NCELL; i++) {
    g_cfg[i].kind  = 0xFD;
    g_cfg[i].mul   = 100 + i;
    g_cfg[i].add   = 7;
    g_cfg[i].shift = 3;
    g_cfg[i].sink  = 0;
  }
  g_cfg0.kind = 0xFD;
  g_cfg0.mul  = 11;
}

// Drive an entry until its specialization is published. Async publishes off a
// worker, and one funcIndex compiles at a time, so this is call-then-drain in a
// loop rather than a single call.
static int warm(uint64_t (*fn)(uint8_t, uint32_t), uint8_t ci) {
  for (int r = 0; r < 200; r++) {
    for (int i = 0; i < 8; i++)
      g_sink += fn(ci, 1);
    ejit_drain_taskpool();
  }
  return 1;
}

// Median of `reps` timings of `calls` invocations, reported as ns/call.
#define TIME_NS_PER_CALL(REPS, CALLS, EXPR)                                    \
  ({                                                                           \
    double _s[16];                                                             \
    int _reps = (REPS);                                                        \
    for (int _r = 0; _r < _reps; _r++) {                                       \
      double _t0 = now_ns();                                                   \
      for (uint64_t _c = 0; _c < (uint64_t)(CALLS); _c++)                      \
        g_sink += (EXPR);                                                      \
      _s[_r] = (now_ns() - _t0) / (double)(CALLS);                             \
    }                                                                          \
    qsort(_s, _reps, sizeof(double), cmp_dbl);                                 \
    _s[_reps / 2];                                                             \
  })

//===-- main ----------------------------------------------------------------===//

int main(int argc, char **argv) {
  uint64_t calls = (argc >= 2) ? strtoull(argv[1], NULL, 0) : 2000000ULL;
  int ncold      = (argc >= 3) ? atoi(argv[2]) : 24;
  const uint8_t ci = 1;
  const int reps = 5;

  if (ncold > NCELL - 4)
    ncold = NCELL - 4;

  init_cells();

  ejit_config_t cfg;
  ejit_default_config(&cfg);
  ejit_init(&cfg);

  // EJIT_BENCH_LOG=1|2|3 turns on runtime diagnostics. Compiles that fail are
  // otherwise invisible here — they just look like slow ones.
  const char *lv = getenv("EJIT_BENCH_LOG");
  if (lv)
    ejit_set_log_level((ejit_log_level_t)atoi(lv));

  printf("=== EJIT pipeline benchmark ===\n");
  printf("calls/measurement=%llu  reps=%d (median)  cold compiles=%d\n\n",
         (unsigned long long)calls, reps, ncold);

  //===-- A. DISPATCH ------------------------------------------------------===//
  //
  // w=0 -> the loop body never runs, so what is left is call + dispatch.

  printf("--- A. Dispatch cost (empty body, w=0) ---\n");

  double a_plain = TIME_NS_PER_CALL(reps, calls, leaf_aot(ci, 0));
  double a_fall  = TIME_NS_PER_CALL(reps, calls, leaf_jit(ci, 0));

  phase("before activate");
  ejit_activate("cell", ci);
  warm(leaf_jit, ci);
  phase("leaf warm");
  double a_hit = TIME_NS_PER_CALL(reps, calls, leaf_jit(ci, 0));
  phase("leaf hit measured");

  double z_plain = TIME_NS_PER_CALL(reps, calls, zerodim_aot(0));
  double z_fall  = TIME_NS_PER_CALL(reps, calls, zerodim_jit(0));
  ejit_activate("static", 0);
  for (int r = 0; r < 200; r++) {
    for (int i = 0; i < 8; i++) g_sink += zerodim_jit(1);
    ejit_drain_taskpool();
  }
  phase("0-dim warm");
  double z_hit = TIME_NS_PER_CALL(reps, calls, zerodim_jit(0));
  phase("0-dim hit measured");

  printf("  1-dim  plain AOT call      : %7.2f ns/call\n", a_plain);
  printf("  1-dim  wrapper, not active : %7.2f ns/call  (+%.2f vs plain)\n",
         a_fall, a_fall - a_plain);
  printf("  1-dim  wrapper, JIT hit    : %7.2f ns/call  (+%.2f vs plain)\n",
         a_hit, a_hit - a_plain);
  printf("  0-dim  plain AOT call      : %7.2f ns/call\n", z_plain);
  printf("  0-dim  wrapper, not active : %7.2f ns/call  (+%.2f vs plain)\n",
         z_fall, z_fall - z_plain);
  printf("  0-dim  wrapper, JIT hit    : %7.2f ns/call  (+%.2f vs plain)\n",
         z_hit, z_hit - z_plain);
  printf("  => DISPATCH TAX (1-dim) = %.2f ns/call\n\n", a_hit - a_plain);

  //===-- B. DISTANCE ------------------------------------------------------===//

  printf("--- B. AOT vs JIT body, swept over body size ---\n");
  printf("  leaf shape (AOT can hoist the may_const loads):\n");
  printf("  %6s %12s %12s %10s %12s\n", "w", "AOT ns/call", "JIT ns/call",
         "speedup", "net (incl.disp)");

  static const uint32_t ws[] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 1024};
  uint32_t breakeven_leaf = 0, breakeven_call = 0;

  for (unsigned k = 0; k < sizeof(ws) / sizeof(ws[0]); k++) {
    uint32_t w = ws[k];
    uint64_t n = calls / (1 + w / 8);
    if (n < 20000) n = 20000;
    double aot = TIME_NS_PER_CALL(reps, n, leaf_aot(ci, w));
    double jit = TIME_NS_PER_CALL(reps, n, leaf_jit(ci, w));
    printf("  %6u %12.2f %12.2f %9.2fx %12s\n", w, aot, jit, aot / jit,
           jit < aot ? "WIN" : "loss");
    if (!breakeven_leaf && jit < aot) breakeven_leaf = w;
  }

  printf("\n  call-crossing shape (noinline callee, AOT cannot hoist):\n");
  printf("  %6s %12s %12s %10s %12s\n", "w", "AOT ns/call", "JIT ns/call",
         "speedup", "net (incl.disp)");

  warm(call_jit, ci);
  phase("call-crossing warm");
  for (unsigned k = 0; k < sizeof(ws) / sizeof(ws[0]); k++) {
    uint32_t w = ws[k];
    uint64_t n = calls / (1 + w / 8);
    if (n < 20000) n = 20000;
    double aot = TIME_NS_PER_CALL(reps, n, call_aot(ci, w));
    double jit = TIME_NS_PER_CALL(reps, n, call_jit(ci, w));
    printf("  %6u %12.2f %12.2f %9.2fx %12s\n", w, aot, jit, aot / jit,
           jit < aot ? "WIN" : "loss");
    if (!breakeven_call && jit < aot) breakeven_call = w;
  }
  printf("  => break-even body size: leaf w>=%u, call-crossing w>=%u\n\n",
         breakeven_leaf, breakeven_call);

  //===-- C. COLD COMPILE --------------------------------------------------===//
  //
  // SYNC so the first call blocks on the compile and the wall time IS the
  // compile time. Distinct cells so nothing dedups.

  printf("--- C. Cold compile latency (ASYNC, distinct cells) ---\n");
  ejit_drain_taskpool();
  phase("before cold");

  // The first call on a fresh cell enqueues; the single worker compiles it and
  // pending_count falls back to 0 once the result is published. So this wall
  // time is one cold compile plus the queue hop (~0.05-0.1 ms). One cell at a
  // time, so the funcIndex dedup never coalesces two requests.
  double *cold = malloc(sizeof(double) * (size_t)ncold);
  int got = 0;
  for (int i = 0; i < ncold; i++) {
    uint8_t c = (uint8_t)(i + 2);
    ejit_activate("cell", c);
    double t0 = now_ns();
    g_sink += cold_jit(c);
    ejit_drain_taskpool();
    double t1 = now_ns();
    cold[got++] = (t1 - t0) / 1e6;
    if (i < 2) {
      const ejit_error_t *e = ejit_get_last_error();
      if (e && e->code)
        printf("  cell %u last error: code=%d fn=%s msg=%s\n", c, e->code,
               e->funcName, e->message);
    }
  }
  phase("cold compiles");
  {
    ejit_code_pool_stats_t cp;
    memset(&cp, 0, sizeof(cp));
    if (ejit_get_code_pool_stats(&cp) == EJIT_OK)
      printf("  code pool: used=%llu reserved=%llu pools=%llu\n",
             (unsigned long long)cp.usedBytes,
             (unsigned long long)cp.reservedBytes,
             (unsigned long long)cp.poolCount);
  }
  qsort(cold, (size_t)got, sizeof(double), cmp_dbl);
  double sum = 0;
  for (int i = 0; i < got; i++) sum += cold[i];
  printf("  n=%d  min %.3f  p50 %.3f  p90 %.3f  max %.3f  avg %.3f ms\n",
         got, cold[0], cold[got / 2], cold[(got * 9) / 10], cold[got - 1],
         sum / got);

  //===-- Payback ----------------------------------------------------------===//

  printf("\n--- Payback ---\n");
  double compile_ms = cold[got / 2];
  double disp = a_hit - a_plain;
  printf("  one cold compile      : %.3f ms\n", compile_ms);
  printf("  dispatch tax per call : %.2f ns\n", disp);
  if (disp > 0)
    printf("  calls just to pay the dispatch tax back at 1 ns saved/call:"
           " %.0f\n", compile_ms * 1e6 / 1.0);
  printf("  (a specialization must save more than %.2f ns/call to be worth\n"
         "   calling at all, then %.0f k calls to repay the compile at\n"
         "   10 ns saved/call)\n", disp, compile_ms * 1e6 / 10.0 / 1000.0);

  //===-- Validation -------------------------------------------------------===//

  ejit_drain_taskpool();
#ifdef EJIT_SRE_SHARED_TASKPOOL
  ejit_taskpool_stats_t ts;
  memset(&ts, 0, sizeof(ts));
  ejit_taskpool_get_stats(&ts);
  printf("\n  taskpool: ready=%u hits=%llu compiles=%llu enq=%llu "
         "pending=%llu failed=%llu\n",
         ts.readyEntries, (unsigned long long)ts.cacheHits,
         (unsigned long long)ts.asyncCompiles,
         (unsigned long long)ts.asyncEnqueues,
         (unsigned long long)ts.alreadyPending,
         (unsigned long long)ts.compileFailed);
  if (ts.asyncCompiles == 0) {
    printf("  FAIL: no compiles happened — every number above is AOT\n");
    free(cold);
    ejit_shutdown();
    return 1;
  }
#endif

  // Correctness: JIT and AOT must agree.
  int bad = 0;
  for (uint32_t w = 0; w <= 64; w += 16) {
    if (leaf_jit(ci, w) != leaf_aot(ci, w)) { printf("  FAIL: leaf w=%u\n", w); bad = 1; }
    if (call_jit(ci, w) != call_aot(ci, w)) { printf("  FAIL: call w=%u\n", w); bad = 1; }
  }
  if (!bad)
    printf("  results MATCH (JIT == AOT)\n");

  free(cold);
  ejit_shutdown();
  printf("\n=== done ===\n");
  return bad;
}