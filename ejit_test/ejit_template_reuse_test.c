/**
 * EJIT bitcode-template reuse test.
 *
 * The ORC engine parses a function's registered bitcode once into a pristine
 * template module and clones the template for every specialization
 * (EJitOrcEngine loadBitcodeModule). Specialization (may_const substitution,
 * entry-linkage promotion, optimization) must happen ONLY on the clone; if any
 * of it leaks into the template, the next clone inherits a previous
 * specialization's constants and silently computes stale values.
 *
 * Checks, all value-exact:
 *   1. Clone independence: many instances of the same functions, each JIT'd
 *      specialization returns its own instance's constants.
 *   2. Template purity across recompiles: three generations of
 *      value-change -> deactivate/activate (version bump) -> recompile. Each
 *      generation's post-drain (cache-hit, i.e. JIT-served) result must show
 *      the NEW constants. A poisoned template returns a previous generation's.
 *   3. Static entry: a `static` ejit_entry relies on the engine promoting the
 *      entry to external linkage on the CLONE so ORC can materialize it.
 *   4. Compile activity: asyncCompiles grows every generation (the values in
 *      (2) really came from fresh compiles, not a stale cache).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ejit_test_helpers.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"

#define NINST 8
#define NGEN 3

struct Cfg {
  ejit_may_const uint32_t v;
};
ejit_period_arr(tp) struct Cfg g_cfg[NINST];

/* Two ordinary entries plus one static entry, all on the same period. */
ejit_entry uint32_t tr_a(ejit_period_arr_ind(tp) uint8_t i) {
  return g_cfg[i].v * 3u + 1u;
}
ejit_entry uint32_t tr_b(ejit_period_arr_ind(tp) uint8_t i) {
  return g_cfg[i].v * 5u + 2u;
}
static ejit_entry uint32_t tr_c(ejit_period_arr_ind(tp) uint8_t i) {
  return g_cfg[i].v * 7u + 3u;
}

static int failures = 0;
#define CHECK(cond, ...)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      printf("FAIL: " __VA_ARGS__);                                           \
      printf("\n");                                                           \
      failures++;                                                              \
    }                                                                          \
  } while (0)

static uint32_t base_of_gen(int gen) { return 100u * (uint32_t)(gen + 1); }

/* Call every (func, instance) pair, draining after each call so the compile
   lands, then call again so the checked value is served by the JIT'd code
   (the first call may be the AOT fallback while the compile is in flight). */
static void run_generation(int gen) {
  uint32_t base = base_of_gen(gen);
  for (int i = 0; i < NINST; i++) {
    uint32_t v = base + (uint32_t)i;
    volatile uint32_t r;
    r = tr_a((uint8_t)i);
    ejit_drain_taskpool();
    r = tr_a((uint8_t)i);
    CHECK(r == v * 3u + 1u, "gen%d tr_a(%d)=%u want %u", gen, i, r,
          v * 3u + 1u);
    r = tr_b((uint8_t)i);
    ejit_drain_taskpool();
    r = tr_b((uint8_t)i);
    CHECK(r == v * 5u + 2u, "gen%d tr_b(%d)=%u want %u", gen, i, r,
          v * 5u + 2u);
    r = tr_c((uint8_t)i);
    ejit_drain_taskpool();
    r = tr_c((uint8_t)i);
    CHECK(r == v * 7u + 3u, "gen%d tr_c(%d)=%u want %u", gen, i, r,
          v * 7u + 3u);
  }
}

int main(void) {
  for (int i = 0; i < NINST; i++)
    g_cfg[i].v = base_of_gen(0) + (uint32_t)i;

  ejit_config_t cfg;
  ejit_default_config(&cfg);
  ejit_init(&cfg);
  for (int i = 0; i < NINST; i++)
    ejit_activate("tp", (uint8_t)i);

  uint64_t prevCompiles = 0;
  for (int gen = 0; gen < NGEN; gen++) {
    if (gen > 0) {
      /* New generation: new constants, then a deactivate/activate cycle per
         instance to bump the version so the stale specialization misses and
         every (func, instance) recompiles -- a fresh clone of the template. */
      for (int i = 0; i < NINST; i++)
        g_cfg[i].v = base_of_gen(gen) + (uint32_t)i;
      for (int i = 0; i < NINST; i++) {
        ejit_deactivate("tp", (uint8_t)i);
        ejit_activate("tp", (uint8_t)i);
      }
    }

    run_generation(gen);

    ejit_taskpool_stats_t st;
    memset(&st, 0, sizeof st);
    ejit_taskpool_get_stats(&st);
    CHECK(st.asyncCompiles > prevCompiles,
          "gen%d produced no new compiles (asyncCompiles %llu -> %llu)", gen,
          (unsigned long long)prevCompiles,
          (unsigned long long)st.asyncCompiles);
    printf("gen%d ok: compiles=%llu ready=%u\n", gen,
           (unsigned long long)st.asyncCompiles, st.readyEntries);
    prevCompiles = st.asyncCompiles;
  }

  printf(failures ? "TEMPLATE REUSE TEST FAILED (%d)\n"
                  : "TEMPLATE REUSE TEST PASSED\n",
         failures);
  ejit_shutdown();
  return failures ? 1 : 0;
}
