/**
 * Does cold-compile time depend on the SIZE OF THE TRANSLATION UNIT rather than
 * the size of the function being specialized?
 *
 * AOT bitcode is emitted per translation unit, not per ejit_entry, so every
 * entry in a file shares one blob and each cold compile parses and runs the
 * pipeline over the whole thing. If that is what dominates, then adding entries
 * to a file that are never even called should slow down the compiles of the one
 * that is.
 *
 * `probe` is the only function measured, and it is byte-identical regardless of
 * EJIT_TU_BALLAST. Ballast entries are never called.
 *
 *   -DEJIT_TU_BALLAST=0   probe alone
 *   -DEJIT_TU_BALLAST=N   probe + N unrelated ejit_entry functions
 *
 * Run: ./ejit_test/out/ejit_tusize_bench
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ejit_test_helpers.h"

extern void ejit_shutdown(void);

#ifndef EJIT_TU_BALLAST
#define EJIT_TU_BALLAST 0
#endif

#define NCELL 32

struct Cfg {
  ejit_may_const uint32_t kind;
  ejit_may_const uint32_t mul;
  ejit_may_const uint32_t add;
  ejit_may_const uint32_t shift;
  uint32_t sink;
};
ejit_period_arr(cell) struct Cfg g_cfg[NCELL];

//===-- The measured function (identical in every build) --------------------===//

ejit_entry
uint32_t probe(ejit_period_arr_ind(cell) uint8_t ci)
{
  return g_cfg[ci].kind + g_cfg[ci].mul;
}

//===-- Ballast: never called, only present in the module -------------------===//
//
// Each carries enough branching and arithmetic to look like real application
// code to the optimizer and the object emitter.

#define BALLAST_FN(N)                                                          \
  ejit_entry uint64_t ballast_##N(ejit_period_arr_ind(cell) uint8_t ci,        \
                                  uint32_t w) {                                \
    struct Cfg *p = &g_cfg[ci];                                                \
    uint64_t s = N;                                                            \
    for (uint32_t i = 0; i < w; i++) {                                         \
      if (p->kind == 0xFD)                                                     \
        s += (uint64_t)p->mul * (i + p->add) / (p->shift + 1);                 \
      else if (p->kind == 0xEC)                                                \
        s += (uint64_t)(p->mul + p->add) * i - p->shift;                       \
      else if (p->kind == 0xAB)                                                \
        s ^= ((uint64_t)p->mul << (p->shift & 15)) + i;                        \
      else                                                                     \
        s += (p->mul ^ p->add) + i;                                            \
    }                                                                          \
    return s;                                                                  \
  }

#if EJIT_TU_BALLAST >= 1
BALLAST_FN(0) BALLAST_FN(1) BALLAST_FN(2) BALLAST_FN(3)
#endif
#if EJIT_TU_BALLAST >= 2
BALLAST_FN(4) BALLAST_FN(5) BALLAST_FN(6) BALLAST_FN(7)
#endif
#if EJIT_TU_BALLAST >= 3
BALLAST_FN(8) BALLAST_FN(9) BALLAST_FN(10) BALLAST_FN(11)
#endif
#if EJIT_TU_BALLAST >= 4
BALLAST_FN(12) BALLAST_FN(13) BALLAST_FN(14) BALLAST_FN(15)
#endif

//===-- Driver --------------------------------------------------------------===//

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static volatile uint32_t sink;

static int cmp_dbl(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}

int main(int argc, char **argv) {
  int n = (argc >= 2) ? atoi(argv[1]) : 24;
  if (n > NCELL - 1) n = NCELL - 1;

  for (int i = 0; i < NCELL; i++) {
    g_cfg[i].kind = 0xFD; g_cfg[i].mul = 10 + i;
    g_cfg[i].add = 3; g_cfg[i].shift = 2;
  }

  ejit_config_t cfg;
  ejit_default_config(&cfg);
  ejit_init(&cfg);

  // ASYNC on purpose. The first call on a fresh cell enqueues, the single
  // worker compiles it, and pending_count returns to 0 when the result is
  // published — so this wall time is one cold compile plus the queue hop
  // (~0.05-0.1 ms). Distinct cells, one at a time, so the funcIndex dedup never
  // coalesces two requests.
  double *t = malloc(sizeof(double) * (size_t)n);
  for (int i = 0; i < n; i++) {
    uint8_t c = (uint8_t)(i + 1);
    ejit_activate("cell", c);
    double t0 = now_ms();
    sink += probe(c);
    ejit_drain_taskpool();
    t[i] = now_ms() - t0;
  }

  ejit_taskpool_stats_t s;
  memset(&s, 0, sizeof(s));
  ejit_taskpool_get_stats(&s);

  qsort(t, (size_t)n, sizeof(double), cmp_dbl);
  double sum = 0;
  for (int i = 0; i < n; i++) sum += t[i];

  printf("ballast=%d  entries_in_TU=%d  n=%d  "
         "min %.3f  p50 %.3f  max %.3f  avg %.3f ms   "
         "compiles=%llu failed=%llu\n",
         EJIT_TU_BALLAST, 1 + 4 * EJIT_TU_BALLAST, n,
         t[0], t[n / 2], t[n - 1], sum / n,
         (unsigned long long)s.asyncCompiles,
         (unsigned long long)s.compileFailed);

  if (s.asyncCompiles == 0) {
    printf("  FAIL: nothing compiled\n");
    free(t);
    ejit_shutdown();
    return 1;
  }
  free(t);
  ejit_shutdown();
  return 0;
}