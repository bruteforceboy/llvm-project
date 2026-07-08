//===-- EJitSrePlatform.cpp - SRE platform adapter for the code pool ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  Wires EJitCodePoolManager to the real SRE platform primitives. Compiled
//  only when EJIT_SRE_CODE_POOL is enabled. The platform symbols (enable_ex,
//  split_2m_to_4k, SRE_MemDbgAlloc) are ONLY declared here — never defined and
//  never given weak fallbacks. The real platform / business link environment
//  must supply their strong definitions; if a symbol is missing it must surface
//  as a link-time error rather than be silently satisfied by a no-op. Host unit
//  tests do not reference makeSreCodePoolManager (they inject mock callbacks
//  into EJitCodePoolManager directly), so this translation unit's external
//  references are never pulled into a host test link.
//
//===----------------------------------------------------------------------===//

#ifdef EJIT_SRE_CODE_POOL

#include "llvm/ExecutionEngine/EJIT/EJitSrePlatform.h"
#include "llvm/ExecutionEngine/EJIT/EJitDiag.h"

#ifndef EJIT_SRE_CODE_POOL_SIZE
#define EJIT_SRE_CODE_POOL_SIZE                                                \
  (static_cast<unsigned long long>(2) * 1024 * 1024)
#endif

#ifndef EJIT_SRE_CODE_POOL_PTNO
#define EJIT_SRE_CODE_POOL_PTNO 8
#endif

// Memory module id passed to SRE_MemDbgAlloc. Not architecturally significant
// for the pool; overridable if a deployment needs a specific id.
#ifndef EJIT_SRE_CODE_POOL_MID
#define EJIT_SRE_CODE_POOL_MID 0
#endif

namespace {
constexpr unsigned long long kSrePoolSize = EJIT_SRE_CODE_POOL_SIZE;
constexpr unsigned char kSrePtNo =
    static_cast<unsigned char>(EJIT_SRE_CODE_POOL_PTNO);
constexpr unsigned kSreMid = static_cast<unsigned>(EJIT_SRE_CODE_POOL_MID);
constexpr size_t k2MiB = static_cast<size_t>(2) * 1024 * 1024;
constexpr size_t k4KiB = static_cast<size_t>(4) * 1024;

#ifdef EJIT_CODE_POOL_NEAR_MAIN
// ── Near-main code arena ─────────────────────────────────────────────────────
// When EJIT_CODE_POOL_NEAR_MAIN is set, the code pool draws its backing memory
// from this arena instead of SRE_MemDbgAlloc. The arena is an ordinary static
// object linked into the main program image, so every pool — and therefore all
// JIT-emitted machine code — lands within the image's address span, i.e. within
// AArch64 direct-branch (±128 MiB) and ADRP (±4 GiB) reach of main's code and
// the period-array / static globals the JIT references. That lets codegen use
// cheap PC-relative addressing instead of the far absolute / literal-pool form
// the Large code model must emit when JITLink's own mmap lands terabytes away.
//
// NOTE: near placement alone does not change the emitted instructions — the ORC
// engine must ALSO drop CodeModel::Large (→ Small/Medium) and stop clearing
// dso_local for this to translate into cheaper addressing. See EJitOrcEngine.
//
// Sizing: the manager requests poolSize + poolAlign per pool (4K-seal slack) and
// 2 MiB-aligns the base, so we reserve EJIT_CODE_POOL_NEAR_MAIN_POOLS such
// windows. The arena is 2 MiB-aligned and its size is a whole multiple of 2 MiB,
// so each pool owns entire 2 MiB regions exclusively — split_2m_to_4k / enable_ex
// never touch unrelated image data. Exhaustion returns null → the manager
// surfaces a clean allocation failure and the JIT falls back to the AOT body.
#ifndef EJIT_CODE_POOL_NEAR_MAIN_POOLS
#define EJIT_CODE_POOL_NEAR_MAIN_POOLS 2
#endif
constexpr size_t kNearMainPools =
    static_cast<size_t>(EJIT_CODE_POOL_NEAR_MAIN_POOLS);
constexpr size_t kNearMainArenaBytes =
    kNearMainPools * (static_cast<size_t>(kSrePoolSize) + k2MiB);

alignas(k2MiB) unsigned char gEJitNearMainArena[kNearMainArenaBytes];
size_t gEJitNearMainOff = 0;

// Bump-allocate a 2 MiB-aligned window from the arena. Called only from
// newActivePoolLocked (under the manager lock), so no synchronization needed.
void *nearMainAlloc(size_t Bytes) {
  size_t base = (gEJitNearMainOff + (k2MiB - 1)) & ~(k2MiB - 1);
  if (base < gEJitNearMainOff || base + Bytes < base ||
      base + Bytes > kNearMainArenaBytes)
    return nullptr; // exhausted / overflow → clean allocation failure
  gEJitNearMainOff = base + Bytes;
  return gEJitNearMainArena + base;
}
#endif // EJIT_CODE_POOL_NEAR_MAIN
} // namespace

//===----------------------------------------------------------------------===//
// Platform primitives (declaration only — defined by the platform/business)
//
// enable_ex / split_2m_to_4k are renamed via asm labels so the generic
// identifiers (ejit_sre_enable_ex / ejit_sre_split_2m_to_4k) are used in C++
// while the linker sees the real platform symbol names. These are intentionally
// NOT given weak fallbacks: in static-pack / partial-link / platform-SDK
// scenarios a weak local definition could shadow or collide with the real
// symbol or bind incorrectly. EmbeddedJIT only declares and calls them; the
// platform must provide the strong definitions.
//===----------------------------------------------------------------------===//
extern "C" unsigned
ejit_sre_enable_ex(unsigned startLevel,
                   unsigned long long va) __asm__("enable_ex");

// Split a 2MiB-aligned [va, va + size) window into 4KiB mappings. Must be
// called before any per-page enable_ex on that window. Returns 0 on success.
extern "C" unsigned
ejit_sre_split_2m_to_4k(unsigned long long va,
                        unsigned long long size) __asm__("split_2m_to_4k");

extern "C" void *SRE_MemDbgAlloc(unsigned int mid, unsigned char ptNo,
                                 unsigned long size, const char *func,
                                 unsigned int line);

std::unique_ptr<llvm::ejit::EJitCodePoolManager>
llvm::ejit::makeSreCodePoolManager() {
  EJitCodePoolManager::Options Opts;
  Opts.poolSize = static_cast<size_t>(kSrePoolSize);
  Opts.poolAlign = k2MiB; // large-page / split granularity
  Opts.minCodeAlign = 64;
  EJIT_DIAG_VERBOSE("makeSreCodePoolManager: poolSize=%llu poolAlign=%zu",
                    kSrePoolSize, k2MiB);
#ifdef EJIT_CODE_POOL_4K_SEAL
  // Adapt to the platform's 4K execute-permission interface: the 2MiB pool is
  // split into 4K mappings at creation and sealed one 4KiB page at a time.
  Opts.fourKSeal = true;
  Opts.sealPageSize = k4KiB;
#endif

  auto RawAlloc = [](size_t Bytes) -> void * {
#ifdef EJIT_CODE_POOL_NEAR_MAIN
    // Serve from the image-resident arena so JIT code lands within AArch64
    // PC-relative reach of the main program's code and globals (see above).
    void *P = nearMainAlloc(Bytes);
    EJIT_DIAG_VERBOSE("nearMainAlloc(%zu) = %p (arena=%p..%p)", Bytes, P,
                      static_cast<void *>(gEJitNearMainArena),
                      static_cast<void *>(gEJitNearMainArena +
                                          kNearMainArenaBytes));
    return P;
#else
    return SRE_MemDbgAlloc(kSreMid, kSrePtNo, static_cast<unsigned long>(Bytes),
                           __func__, __LINE__);
#endif
  };

  auto Seal = [](void *Va) -> unsigned {
#ifdef EJIT_SRE_ENABLE_EX
    // In 4K seal mode Va is a single 4KiB page; in legacy mode it is the 2MiB
    // pool base. enable_ex flips the page containing Va to RX either way.
    return ejit_sre_enable_ex(1, reinterpret_cast<unsigned long long>(Va));
#else
    // Code-pool routing without permission flips (bring-up / measurement).
    (void)Va;
    return 0;
#endif
  };

  auto Split = [](void *Base, size_t Size) -> unsigned {
#ifdef EJIT_CODE_POOL_4K_SEAL
    return ejit_sre_split_2m_to_4k(reinterpret_cast<unsigned long long>(Base),
                                   static_cast<unsigned long long>(Size));
#else
    (void)Base;
    (void)Size;
    return 0;
#endif
  };

  return std::make_unique<EJitCodePoolManager>(Opts, RawAlloc, Seal, Split);
}

bool llvm::ejit::prepareSreCodeForCurrentCore(const void *FnPtr) {
#if !defined(EJIT_SRE_ENABLE_EX) || defined(EJIT_CODE_POOL_4K_SEAL)
  EJIT_DIAG("prepareSreCode: unsupported config (FnPtr=%p), clean fallback",
            FnPtr);
  (void)FnPtr;
  return false;
#else
  if (!FnPtr) {
    EJIT_DIAG("prepareSreCode: null FnPtr, reject");
    return false;
  }
  const auto Address = reinterpret_cast<uintptr_t>(FnPtr);
  const auto PoolBase = Address & ~(static_cast<uintptr_t>(k2MiB) - 1);
  unsigned Rc = ejit_sre_enable_ex(1, static_cast<unsigned long long>(PoolBase));
  if (Rc != 0) {
    EJIT_DIAG("prepareSreCode FAIL: enable_ex poolBase=0x%llx rc=%u",
              static_cast<unsigned long long>(PoolBase), Rc);
    return false;
  }
  return true;
#endif
}

bool llvm::ejit::ejitSreSplitPoolForCurrentCore(uintptr_t PoolBase,
                                                uint64_t PoolSize) {
#if defined(EJIT_SRE_ENABLE_EX) && defined(EJIT_CODE_POOL_4K_SEAL)
  if (PoolBase == 0 || PoolSize == 0) {
    EJIT_DIAG("splitPoolForCurrentCore reject: poolBase=0x%llx size=%llu",
              static_cast<unsigned long long>(PoolBase),
              static_cast<unsigned long long>(PoolSize));
    return false;
  }
  // Per-core: this splits the 2MiB large page into 4K mappings in the calling
  // core's stage-1 translation only. enable_ex per page follows.
  unsigned Rc = ejit_sre_split_2m_to_4k(
      static_cast<unsigned long long>(PoolBase),
      static_cast<unsigned long long>(PoolSize));
  if (Rc != 0) {
    EJIT_DIAG("splitPoolForCurrentCore FAIL: split_2m_to_4k poolBase=0x%llx "
              "size=%llu rc=%u",
              static_cast<unsigned long long>(PoolBase),
              static_cast<unsigned long long>(PoolSize), Rc);
    return false;
  }
  return true;
#else
  EJIT_DIAG("splitPoolForCurrentCore unsupported config: poolBase=0x%llx",
            static_cast<unsigned long long>(PoolBase));
  (void)PoolBase;
  (void)PoolSize;
  return false;
#endif
}

bool llvm::ejit::ejitSreSealPageForCurrentCore(uintptr_t PageVA) {
#ifdef EJIT_SRE_ENABLE_EX
  if (PageVA == 0) {
    EJIT_DIAG("sealPageForCurrentCore reject: null PageVA");
    return false;
  }
  // Per-core: flips the 4KiB page containing PageVA to RX in the calling core's
  // translation context. enable_ex performs its own permission/cache sync, so
  // no __builtin___clear_cache here.
  unsigned Rc = ejit_sre_enable_ex(1, static_cast<unsigned long long>(PageVA));
  if (Rc != 0) {
    EJIT_DIAG("sealPageForCurrentCore FAIL: enable_ex pageVA=0x%llx rc=%u",
              static_cast<unsigned long long>(PageVA), Rc);
    return false;
  }
  return true;
#else
  EJIT_DIAG("sealPageForCurrentCore unsupported config: pageVA=0x%llx",
            static_cast<unsigned long long>(PageVA));
  (void)PageVA;
  return false;
#endif
}

#endif // EJIT_SRE_CODE_POOL
