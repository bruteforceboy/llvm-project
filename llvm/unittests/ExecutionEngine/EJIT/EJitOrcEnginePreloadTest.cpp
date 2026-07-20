//===-- EJitOrcEnginePreloadTest.cpp - bitcode preload cache -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// EJitOrcEngine parses each bitcode blob once and clones the resulting template
// module per specialization. A cache hit and a re-parse produce identical JIT
// output, so these tests make the *input* observable instead: the bitcode is
// loaded from a mutable buffer whose bytes are rewritten between loads while
// its address (the cache key) stays fixed. A cached load keeps returning the
// original function; a re-parse would pick up the new bytes.
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitOrcEngine.h"
#include "llvm/ExecutionEngine/EJIT/EJitOptions.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntimeState.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"

#include <cstring>
#include <string>
#include <vector>

using namespace llvm;
using namespace llvm::ejit;

namespace {

/// Bitcode for a module defining `i32 @spec_entry()` returning \p RetVal.
std::string makeEntryBitcode(uint32_t RetVal) {
  LLVMContext Ctx;
  auto M = std::make_unique<Module>("ejit_preload_test", Ctx);
  auto *FT = FunctionType::get(Type::getInt32Ty(Ctx), /*isVarArg=*/false);
  auto *F = Function::Create(FT, GlobalValue::ExternalLinkage, "spec_entry",
                             M.get());
  IRBuilder<> B(BasicBlock::Create(Ctx, "entry", F));
  B.CreateRet(B.getInt32(RetVal));

  std::string Buf;
  raw_string_ostream OS(Buf);
  WriteBitcodeToFile(*M, OS);
  OS.flush();
  return Buf;
}

using EntryFn = uint32_t (*)();

/// These tests never set an active SpecializationContext, so the IR transform
/// layer skips the JIT optimization pipeline; codegen still runs.
class EJitPreloadTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
  }

  void SetUp() override {
    auto EngOrErr = EJitOrcEngine::Create(Cfg, Reg, State);
    ASSERT_TRUE(!!EngOrErr) << toString(EngOrErr.takeError());
    Eng = std::move(*EngOrErr);
  }

  uint32_t compileAndRun(StringRef Blob, uint64_t Key) {
    EXPECT_FALSE(errorToBool(Eng->loadBitcodeModule(Blob, Key, "spec_entry")))
        << "loadBitcodeModule failed for key " << Key;
    auto AddrOrErr = Eng->lookup(Key, "spec_entry");
    EXPECT_TRUE(!!AddrOrErr) << "lookup failed for key " << Key;
    if (!AddrOrErr) {
      consumeError(AddrOrErr.takeError());
      return 0;
    }
    return reinterpret_cast<EntryFn>(*AddrOrErr)();
  }

  Config Cfg;
  PeriodArrayRegistry Reg;
  EJitRuntimeState State;
  std::unique_ptr<EJitOrcEngine> Eng;
};

TEST_F(EJitPreloadTest, SameBlobAddressIsParsedOnlyOnce) {
  const std::string BC1 = makeEntryBitcode(111);
  const std::string BC2 = makeEntryBitcode(222);

  // One stable allocation, so both loads present the same address.
  std::vector<char> Blob(std::max(BC1.size(), BC2.size()));
  std::memcpy(Blob.data(), BC1.data(), BC1.size());

  EXPECT_EQ(compileAndRun(StringRef(Blob.data(), BC1.size()), 1), 111u);

  // Same address, different bytes. Only a re-parse would observe 222.
  std::memcpy(Blob.data(), BC2.data(), BC2.size());
  EXPECT_EQ(compileAndRun(StringRef(Blob.data(), BC2.size()), 2), 111u)
      << "second specialization re-parsed the bitcode instead of cloning the "
         "cached template";
}

// Control for the test above, which would also pass if the cache returned one
// stale module for everything.
TEST_F(EJitPreloadTest, DistinctBlobAddressIsParsedIndependently) {
  const std::string BC1 = makeEntryBitcode(111);
  const std::string BC2 = makeEntryBitcode(222);

  std::vector<char> BlobA(BC1.begin(), BC1.end());
  std::vector<char> BlobB(BC2.begin(), BC2.end());
  ASSERT_NE(BlobA.data(), BlobB.data());

  EXPECT_EQ(compileAndRun(StringRef(BlobA.data(), BlobA.size()), 1), 111u);
  EXPECT_EQ(compileAndRun(StringRef(BlobB.data(), BlobB.size()), 2), 222u);
}

TEST_F(EJitPreloadTest, PreLoadBitcodeUtilPopulatesTheCache) {
  const std::string BC1 = makeEntryBitcode(111);
  const std::string BC2 = makeEntryBitcode(222);

  std::vector<char> Blob(std::max(BC1.size(), BC2.size()));
  std::memcpy(Blob.data(), BC1.data(), BC1.size());

  ASSERT_FALSE(
      errorToBool(Eng->preLoadBitcodeUtil(StringRef(Blob.data(), BC1.size()))));

  // Rewritten before any loadBitcodeModule call: the preload already holds the
  // template.
  std::memcpy(Blob.data(), BC2.data(), BC2.size());
  EXPECT_EQ(compileAndRun(StringRef(Blob.data(), BC2.size()), 1), 111u)
      << "preLoadBitcodeUtil did not populate the cache";
}

// Preloading twice is idempotent and must not discard the existing template.
TEST_F(EJitPreloadTest, PreLoadBitcodeUtilIsIdempotent) {
  const std::string BC1 = makeEntryBitcode(111);
  const std::string BC2 = makeEntryBitcode(222);

  std::vector<char> Blob(std::max(BC1.size(), BC2.size()));
  std::memcpy(Blob.data(), BC1.data(), BC1.size());
  ASSERT_FALSE(
      errorToBool(Eng->preLoadBitcodeUtil(StringRef(Blob.data(), BC1.size()))));

  std::memcpy(Blob.data(), BC2.data(), BC2.size());
  ASSERT_FALSE(
      errorToBool(Eng->preLoadBitcodeUtil(StringRef(Blob.data(), BC2.size()))));

  EXPECT_EQ(compileAndRun(StringRef(Blob.data(), BC2.size()), 1), 111u)
      << "a second preLoadBitcodeUtil rebuilt the template from new bytes";
}

} // namespace
