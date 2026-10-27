/*
 * SRE NEON JIT probe. Run test_ejit_period on the EJIT worker core and one
 * other core. The worker prints the JIT body after the other core sees it.
 *
 * Build the SRE runtime with EJIT_DIAG_ENABLE=ON and EJIT_DUMP_ASM=ON. Compile
 * this file with the matching EJIT clang and -O2 -fno-vectorize
 * -fno-slp-vectorize, so a vector loop in the dump came from JIT optimization.
 * Link with the rebuilt ejit.o and the usual SRE registry script. The NEON
 * vectorization change must also be present in that runtime build.
 *
 * In the printed neon_probe assembly, look for vector loads/stores and
 * "add vN.4s" (register numbers vary). EXECUTION PASS checks only the result.
 */

#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"
#include <stdint.h>

extern void SRE_printf(const char *format, ...);
extern uint32_t SRE_TaskDelay(uint32_t ticks);
extern void call_init_array_functions(void);
extern uint8_t g_ucLocalCoreID;

#define NEON_COUNT 256u
#define NEON_SHARED __attribute__((section(".mc_shared")))

struct NeonConfig {
  ejit_may_const uint32_t bias;
};
NEON_SHARED ejit_period_arr(neon_period)
struct NeonConfig g_neonConfig[16];
NEON_SHARED uint32_t g_neonRunner;
NEON_SHARED uint32_t g_neonDumpRequest;
NEON_SHARED uint32_t g_neonDumpDone;
/* EJIT's AOT pass must retain this shared symbol for the JIT body. */
NEON_SHARED __attribute__((used)) volatile uintptr_t g_neonPc;

static uint32_t g_neonSrc[NEON_COUNT];
static uint32_t g_neonDst[NEON_COUNT];

void __attribute__((noinline)) neon_mark_pc(void) {
  g_neonPc = (uintptr_t)__builtin_return_address(0);
}

ejit_entry __attribute__((noinline)) void
neon_probe(ejit_period_arr_ind(neon_period) uint8_t instance,
           uint32_t *restrict dst, const uint32_t *restrict src,
           uint32_t count) {
  neon_mark_pc();
  const uint32_t bias = g_neonConfig[instance].bias;
  for (uint32_t i = 0; i < count; ++i)
    dst[i] = src[i] + bias;
}

static void idle(void) {
  for (;;)
    SRE_TaskDelay(10);
}

int test_ejit_period(uint8_t cell, uint8_t trp, uint8_t slice,
                     uint8_t carrier) {
  (void)cell;
  (void)trp;
  (void)slice;
  (void)carrier;
  call_init_array_functions();

  ejit_config_t config = {
      .compileMode = EJIT_COMPILE_ASYNC,
      .optLevel = EJIT_OPT_L2,
      .enableLogger = false,
      .forceStaticRegistry = true,
  };
  ejit_set_log_level(EJIT_LOG_INFO);
  if (ejit_init(&config) != EJIT_OK) {
    SRE_printf("[NEON] FAIL init core=%u\n", g_ucLocalCoreID);
    idle();
  }

  if (g_ucLocalCoreID == ejit_taskpool_get_worker_core()) {
    while (!__atomic_load_n(&g_neonDumpRequest, __ATOMIC_ACQUIRE))
      SRE_TaskDelay(10);
    ejit_print_dumped("neon_probe");
    __atomic_store_n(&g_neonDumpDone, 1, __ATOMIC_RELEASE);
    idle();
  }

  uint32_t expected = 0;
  if (!__atomic_compare_exchange_n(&g_neonRunner, &expected,
                                   (uint32_t)g_ucLocalCoreID + 1, false,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    idle();

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wembedded-jit"
  g_neonConfig[0].bias = 7;
#pragma clang diagnostic pop
  for (uint32_t i = 0; i < NEON_COUNT; ++i) {
    g_neonSrc[i] = 3 * i + 1;
    g_neonDst[i] = 0;
  }

  neon_probe(0, g_neonDst, g_neonSrc, NEON_COUNT);
  const uintptr_t aotPc = g_neonPc;
  ejit_dump_func("neon_probe");
  if (ejit_activate("neon_period", 0) != EJIT_OK) {
    SRE_printf("[NEON] FAIL activate\n");
    idle();
  }

  for (uint32_t attempt = 0; attempt < 6000 && g_neonPc == aotPc; ++attempt) {
    neon_probe(0, g_neonDst, g_neonSrc, NEON_COUNT);
    SRE_TaskDelay(10);
  }

  int ok = g_neonPc != aotPc;
  for (uint32_t i = 0; i < NEON_COUNT; ++i)
    ok &= g_neonDst[i] == g_neonSrc[i] + 7;

  if (ok) {
    __atomic_store_n(&g_neonDumpRequest, 1, __ATOMIC_RELEASE);
    for (uint32_t attempt = 0;
         attempt < 6000 && !__atomic_load_n(&g_neonDumpDone, __ATOMIC_ACQUIRE);
         ++attempt)
      SRE_TaskDelay(10);
    ok = __atomic_load_n(&g_neonDumpDone, __ATOMIC_ACQUIRE) != 0;
  }

  SRE_printf("[NEON] EXECUTION %s; inspect worker assembly for add v?.4s\n",
             ok ? "PASS" : "FAIL");
  idle();
  return 0;
}
