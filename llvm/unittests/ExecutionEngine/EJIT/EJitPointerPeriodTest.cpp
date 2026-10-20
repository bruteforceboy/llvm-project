//===-- EJitPointerPeriodTest.cpp - Pointer period bounds tests -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/AsmParser/Parser.h"
#include "llvm/ExecutionEngine/EJIT/EJit.h"
#include "llvm/ExecutionEngine/EJIT/EJitOptimizer.h"
#include "llvm/ExecutionEngine/EJIT/EJitRegistrationStore.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/SourceMgr.h"
#include "gtest/gtest.h"
#include <limits>
#include <sstream>

using namespace llvm;
using namespace llvm::ejit;

namespace {
struct PointerFixture {
  unsigned Slots;
  uint64_t Stride;
  std::vector<uint8_t> Data;
  void *Pointer;

  explicit PointerFixture(unsigned Slots)
      : Slots(Slots), Stride(alignTo(8 + Slots * 4, 8)), Data(2 * Stride),
        Pointer(Data.data()) {
    for (unsigned I = 0; I < Slots; ++I)
      Data[Stride + 8 + I * 4 + 1] = 1;
  }

  std::unique_ptr<Module> module(LLVMContext &Ctx, bool Absolute = false,
                                 bool FreeDim = true, bool Layout = true,
                                 bool Lifecycle = true, bool BigEndian = false,
                                 unsigned Field = 1, StringRef Index = {}) {
    std::ostringstream IR;
    IR << "target datalayout = \"" << (BigEndian ? 'E' : 'e')
       << "-p:64:64-i64:64\"\n"
          "%Slot = type { i8, i8, i16 }\n"
          "%Cell = type { i64, ["
       << Slots
       << " x %Slot] }\n"
          "@cfg = external global ptr, !ejit.metadata !0\n"
          "declare void @yes(i16)\n"
          "declare void @no(i16)\n"
          "define i8 @probe(i8 %cell, i16 %slot) !ejit.metadata !3 {\n";
    if (!Absolute)
      IR << "  %root = load ptr, ptr @cfg\n";
    IR << "  %phase = "
       << (Index.empty() ? "urem i16 %slot, " + std::to_string(Slots)
                         : Index.str())
       << "\n  %idx = zext i16 %phase to i64\n"
          "  %selected = getelementptr inbounds nuw %Cell, ptr ";
    if (Absolute)
      IR << "inttoptr (i64 " << reinterpret_cast<uintptr_t>(Data.data())
         << " to ptr)";
    else
      IR << "%root";
    IR << ", i64 1, i32 1, i64 %idx, i32 " << Field
       << "\n"
          "  %v = load i8, ptr %selected, !ejit.may_const !7\n"
          "  %cond = icmp eq i8 %v, 1\n"
          "  br i1 %cond, label %hot, label %cold\n"
          "hot:\n  call void @yes(i16 %slot)\n"
          "  store i8 0, ptr %selected\n  ret i8 %v\n"
          "cold:\n  call void @no(i16 %slot)\n  ret i8 %v\n"
          "}\n!0 = !{!1";
    if (Layout)
      IR << ", !8";
    IR << "}\n!1 = !{!\"ejit_period_arr\", !\"cell\", i32 0}\n"
          "!3 = !{!4";
    if (Lifecycle)
      IR << ", !5";
    if (FreeDim)
      IR << ", !6";
    IR << "}\n!4 = !{!\"ejit_entry\"}\n"
          "!5 = !{!\"ejit_period_arr_ind\", !\"cell\", i32 0}\n"
          "!6 = !{!\"ejit_free_dim\", !\"\", i32 1}\n!7 = !{}\n"
          "!8 = !{!\"ejit_pointer_layout\", i64 "
       << Stride;
    for (unsigned I = 0; I < Slots; ++I)
      IR << ", !" << 10 + I;
    IR << "}\n";
    for (unsigned I = 0; I < Slots; ++I)
      IR << '!' << 10 + I << " = !{i64 " << 8 + I * 4 + 1 << ", i64 1}\n";
    SMDiagnostic Err;
    auto M = parseAssemblyString(IR.str(), Err, Ctx);
    if (!M)
      ADD_FAILURE() << Err.getMessage().str();
    return M;
  }

  void registerRoot(PeriodArrayRegistry &Registry) {
    Registry.registerArray("cell", "cfg", &Pointer, 0);
  }
};

static unsigned markedLoads(Module &M) {
  unsigned Count = 0;
  for (Instruction &I : instructions(M.getFunction("probe")))
    if (auto *LI = dyn_cast<LoadInst>(&I))
      Count += LI->hasMetadata("ejit.may_const");
  return Count;
}

static void optimize(Module &M, PeriodArrayRegistry &Registry,
                     CompileTier Tier = CompileTier::Baseline) {
  SpecializationContext Context;
  Context.fnName = "probe";
  Context.dimensions.push_back({"cell", 1});
  Context.tier = Tier;
  EJitOptimizer Optimizer(Registry);
  Optimizer.runPipeline(M, Context);
  EXPECT_FALSE(verifyModule(M, &errs()));
}
} // namespace

TEST(EJitPointerPeriod, FoldsNestedN5N10AcrossPipelineTiers) {
  for (unsigned Slots : {5U, 10U})
    for (CompileTier Tier : {CompileTier::Baseline, CompileTier::Instrumented,
                             CompileTier::PGOUse}) {
      SCOPED_TRACE(static_cast<unsigned>(Tier));
      PointerFixture Fixture(Slots);
      LLVMContext Ctx;
      auto M = Fixture.module(Ctx);
      ASSERT_NE(M, nullptr);
      PeriodArrayRegistry Registry;
      Fixture.registerRoot(Registry);
      ASSERT_TRUE(Registry.registerPointerPeriodBounds("cfg", Fixture.Pointer,
                                                       Fixture.Data.size()));
      optimize(*M, Registry, Tier);
      EXPECT_EQ(markedLoads(*M), 0U);
      bool LiveStore = false, LiveCall = false;
      Function *F = M->getFunction("probe");
      EXPECT_FALSE(F->getArg(1)->use_empty());
      for (Instruction &I : instructions(F)) {
        if (auto *Store = dyn_cast<StoreInst>(&I))
          LiveStore |= !isa<Constant>(Store->getPointerOperand());
        if (auto *Call = dyn_cast<CallInst>(&I))
          if (Function *Callee = Call->getCalledFunction()) {
            EXPECT_NE(Callee->getName(), "no");
            LiveCall |= Callee->getName() == "yes" &&
                        Call->getArgOperand(0) == F->getArg(1);
          }
      }
      EXPECT_TRUE(LiveStore);
      EXPECT_TRUE(LiveCall);
    }
}

TEST(EJitPointerPeriod, AbsoluteI8UsesTargetLayoutInBothByteOrders) {
  for (bool BigEndian : {false, true}) {
    PointerFixture Fixture(10);
    LLVMContext Ctx;
    auto M = Fixture.module(Ctx, true, true, true, true, BigEndian);
    ASSERT_NE(M, nullptr);
    PeriodArrayRegistry Registry;
    Fixture.registerRoot(Registry);
    ASSERT_TRUE(Registry.registerPointerPeriodBounds("cfg", Fixture.Pointer,
                                                     Fixture.Data.size()));
    optimize(*M, Registry);
    EXPECT_EQ(markedLoads(*M), 0U);
  }
}

TEST(EJitPointerPeriod, FoldsStaticPointerPeriod) {
  PointerFixture Fixture(5);
  LLVMContext Ctx;
  auto M = Fixture.module(Ctx);
  ASSERT_NE(M, nullptr);
  MDNode *Old = M->getGlobalVariable("cfg")->getMetadata("ejit.metadata");
  auto *Static = MDNode::get(
      Ctx, {MDString::get(Ctx, "ejit_period"), MDString::get(Ctx, "static")});
  M->getGlobalVariable("cfg")->setMetadata(
      "ejit.metadata", MDNode::get(Ctx, {Static, Old->getOperand(1).get()}));
  PeriodArrayRegistry Registry;
  Registry.registerStaticVar("cfg", &Fixture.Pointer);
  ASSERT_TRUE(Registry.registerPointerPeriodBounds("cfg", Fixture.Pointer,
                                                   Fixture.Data.size()));
  optimize(*M, Registry);
  EXPECT_EQ(markedLoads(*M), 0U);
}

TEST(EJitPointerPeriod, FoldsFlattenedPeriodAndSlotOffsets) {
  for (unsigned Slots : {5U, 10U}) {
    PointerFixture Fixture(Slots);
    for (unsigned I = 0; I < Slots; ++I)
      Fixture.Data[Slots * 4 + I * 4 + 1] = 1;
    LLVMContext Ctx;
    std::ostringstream IR;
    IR << "target datalayout = \"e-p:64:64-i64:64\"\n"
          "%Slot = type { i8, i8, i16 }\n"
          "@cfg = external global ptr, !ejit.metadata !0\n"
          "define i8 @probe(i8 %trp, i16 %slot) !ejit.metadata !3 {\n"
          "  %root = load ptr, ptr @cfg\n"
          "  %block = getelementptr inbounds nuw i8, ptr %root, i64 "
       << Slots * 4
       << "\n"
          "  %phase = urem i16 %slot, "
       << Slots
       << "\n"
          "  %idx = zext i16 %phase to i64\n"
          "  %field = getelementptr inbounds nuw %Slot, ptr %block, "
          "i64 %idx, i32 1\n"
          "  %value = load i8, ptr %field, !ejit.may_const !7\n"
          "  store i8 0, ptr %field\n  ret i8 %value\n}\n"
          "!0 = !{!1, !8}\n"
          "!1 = !{!\"ejit_period_arr\", !\"cell\", i32 0}\n"
          "!3 = !{!4, !5, !6}\n!4 = !{!\"ejit_entry\"}\n"
          "!5 = !{!\"ejit_period_arr_ind\", !\"cell\", i32 0}\n"
          "!6 = !{!\"ejit_free_dim\", !\"\", i32 1}\n!7 = !{}\n"
          "!8 = !{!\"ejit_pointer_layout\", i64 4, !9}\n"
          "!9 = !{i64 1, i64 1}\n";
    SMDiagnostic Err;
    auto M = parseAssemblyString(IR.str(), Err, Ctx);
    ASSERT_NE(M, nullptr) << Err.getMessage().str();
    PeriodArrayRegistry Registry;
    Fixture.registerRoot(Registry);
    ASSERT_TRUE(Registry.registerPointerPeriodBounds("cfg", Fixture.Pointer,
                                                     Slots * 4 * 2));
    optimize(*M, Registry);
    EXPECT_EQ(markedLoads(*M), 0U);
    auto *Return =
        dyn_cast<ReturnInst>(M->getFunction("probe")->back().getTerminator());
    ASSERT_NE(Return, nullptr);
    auto *Value = dyn_cast<ConstantInt>(Return->getReturnValue());
    ASSERT_NE(Value, nullptr);
    EXPECT_EQ(Value->getZExtValue(), 1U);
  }
}

TEST(EJitPointerPeriod, RequiresBoundsFreeDimLayoutAndLifecycle) {
  for (unsigned Missing = 0; Missing < 5; ++Missing) {
    SCOPED_TRACE(Missing);
    PointerFixture Fixture(5);
    LLVMContext Ctx;
    auto M =
        Fixture.module(Ctx, false, Missing != 1, Missing != 2, Missing != 3);
    ASSERT_NE(M, nullptr);
    PeriodArrayRegistry Registry;
    if (Missing != 4)
      Fixture.registerRoot(Registry);
    if (Missing != 0)
      ASSERT_TRUE(Registry.registerPointerPeriodBounds("cfg", Fixture.Pointer,
                                                       Fixture.Data.size()));
    optimize(*M, Registry);
    EXPECT_EQ(markedLoads(*M), 1U);
  }
}

TEST(EJitPointerPeriod, RejectsCrossFieldAndAtomicLoads) {
  for (bool Atomic : {false, true}) {
    PointerFixture Fixture(5);
    LLVMContext Ctx;
    auto M = Fixture.module(Ctx);
    ASSERT_NE(M, nullptr);
    SmallVector<LoadInst *, 4> Loads;
    for (Instruction &I : instructions(M->getFunction("probe")))
      if (auto *Load = dyn_cast<LoadInst>(&I))
        if (Load->hasMetadata("ejit.may_const"))
          Loads.push_back(Load);
    for (LoadInst *Load : Loads) {
      if (Atomic) {
        Load->setAtomic(AtomicOrdering::Acquire);
      } else {
        IRBuilder<> B(Load);
        auto *Wide = B.CreateAlignedLoad(B.getInt16Ty(),
                                         Load->getPointerOperand(), Align(1));
        Wide->setMetadata("ejit.may_const",
                          Load->getMetadata("ejit.may_const"));
        Load->replaceAllUsesWith(B.CreateTrunc(Wide, B.getInt8Ty()));
        Load->eraseFromParent();
      }
    }
    PeriodArrayRegistry Registry;
    Fixture.registerRoot(Registry);
    ASSERT_TRUE(Registry.registerPointerPeriodBounds("cfg", Fixture.Pointer,
                                                     Fixture.Data.size()));
    optimize(*M, Registry);
    EXPECT_EQ(markedLoads(*M), 1U);
  }
}

TEST(EJitPointerPeriod, RejectsAmbiguousEligibleAllocations) {
  PointerFixture Fixture(5);
  LLVMContext Ctx;
  auto M = Fixture.module(Ctx, true);
  ASSERT_NE(M, nullptr);
  auto *Alias =
      new GlobalVariable(*M, PointerType::getUnqual(Ctx), false,
                         GlobalValue::ExternalLinkage, nullptr, "alias");
  Alias->setMetadata("ejit.metadata",
                     M->getGlobalVariable("cfg")->getMetadata("ejit.metadata"));
  PeriodArrayRegistry Registry;
  Fixture.registerRoot(Registry);
  Registry.registerArray("cell", "alias", &Fixture.Pointer, 0);
  ASSERT_TRUE(Registry.registerPointerPeriodBounds("cfg", Fixture.Pointer,
                                                   Fixture.Data.size()));
  ASSERT_TRUE(Registry.registerPointerPeriodBounds("alias", Fixture.Pointer,
                                                   Fixture.Data.size()));
  optimize(*M, Registry);
  EXPECT_EQ(markedLoads(*M), 1U);
}

TEST(EJitPointerPeriod, RejectsInvalidWitnessAndUnauthorizedField) {
  for (unsigned Rejection = 0; Rejection < 7; ++Rejection) {
    SCOPED_TRACE(Rejection);
    PointerFixture Fixture(5);
    LLVMContext Ctx;
    // A different field is deliberately marked in IR: the compiler-generated
    // layout must reject it even when an earlier transform copied the marker.
    auto M = Fixture.module(Ctx, false, true, true, true, false,
                            Rejection == 0 ? 0 : 1,
                            Rejection == 1 ? "sub nuw i16 %slot, 1" : "");
    ASSERT_NE(M, nullptr);
    PeriodArrayRegistry Registry;
    Fixture.registerRoot(Registry);
    uint64_t Size = Fixture.Data.size();
    if (Rejection == 2)
      Size = Fixture.Stride; // selected cell is outside the allocation
    if (Rejection == 3)
      --Size; // not a whole number of pointee elements
    ASSERT_TRUE(
        Registry.registerPointerPeriodBounds("cfg", Fixture.Pointer, Size));
    std::vector<uint8_t> Other(Fixture.Data.size());
    if (Rejection == 4)
      Fixture.Pointer = Other.data(); // stale bounds must not transfer
    if (Rejection == 5)
      for (Instruction &I : instructions(M->getFunction("probe")))
        if (auto *Load = dyn_cast<LoadInst>(&I))
          Load->setVolatile(true);
    if (Rejection == 6)
      M->getGlobalVariable("cfg")->setMetadata(
          "ejit.metadata",
          MDNode::get(
              Ctx, {MDNode::get(Ctx, {MDString::get(Ctx, "ejit_period_arr"),
                                      MDString::get(Ctx, "cell"),
                                      ConstantAsMetadata::get(ConstantInt::get(
                                          Type::getInt32Ty(Ctx), 0))}),
                    MDNode::get(Ctx, {MDString::get(Ctx, "ejit_pointer_layout"),
                                      ConstantAsMetadata::get(ConstantInt::get(
                                          Type::getInt64Ty(Ctx), 0))})}));
    optimize(*M, Registry);
    EXPECT_EQ(markedLoads(*M), 1U);
  }
}

TEST(EJitPointerPeriod, BoundsRegistrationValidatesAndKeepsFirst) {
  PeriodArrayRegistry Registry;
  uint8_t Data[16] = {};
  EXPECT_FALSE(Registry.registerPointerPeriodBounds("", Data, sizeof(Data)));
  EXPECT_FALSE(Registry.registerPointerPeriodBounds("cfg", nullptr, 16));
  EXPECT_FALSE(Registry.registerPointerPeriodBounds("cfg", Data, 0));
  EXPECT_FALSE(Registry.registerPointerPeriodBounds(
      "cfg", Data, std::numeric_limits<uint64_t>::max()));
  EXPECT_TRUE(Registry.registerPointerPeriodBounds("cfg", Data, sizeof(Data)));
  EXPECT_TRUE(Registry.registerPointerPeriodBounds("cfg", Data, sizeof(Data)));
  EXPECT_FALSE(Registry.registerPointerPeriodBounds("cfg", Data + 1, 8));
  ASSERT_NE(Registry.getPointerPeriodBounds("cfg"), nullptr);
  EXPECT_EQ(Registry.getPointerPeriodBounds("cfg")->base, Data);
  EXPECT_EQ(Registry.getPointerPeriodBounds("cfg")->size, sizeof(Data));
}

TEST(EJitPointerPeriod, CApiStagesBoundsAndRejectsConflicts) {
  auto &Store = EJitRegistrationStore::instance();
  Store.consume();
  uint8_t Data[16] = {};
  EXPECT_EQ(ejit_register_pointer_period_bounds(nullptr, Data, 16),
            EJIT_ERR_INVALID_PARAM);
  EXPECT_EQ(ejit_register_pointer_period_bounds("cfg", Data, 16), EJIT_OK);
  EXPECT_EQ(ejit_register_pointer_period_bounds("cfg", Data, 16), EJIT_OK);
  EXPECT_EQ(ejit_register_pointer_period_bounds("cfg", Data, 8),
            EJIT_ERR_INVALID_PARAM);
  auto Stored = Store.consume();
  // Bounds alone must not suppress automatic static registry discovery.
  EXPECT_TRUE(Stored.empty());
  ASSERT_EQ(Stored.pointerPeriodBounds.size(), 1U);
  EXPECT_EQ(Stored.pointerPeriodBounds[0].base, Data);
  EXPECT_EQ(Stored.pointerPeriodBounds[0].size, 16U);
  EXPECT_TRUE(Store.consume().pointerPeriodBounds.empty());
}

TEST(EJitPointerPeriod, InitTransfersBoundsIncludingForcedStaticRegistry) {
  for (bool ForceStatic : {false, true}) {
    auto &Store = EJitRegistrationStore::instance();
    Store.consume();
    uint8_t Data[16] = {};
    Store.registerStaticVar("bounds_marker", Data);
    ASSERT_TRUE(Store.registerPointerPeriodBounds("cfg", Data, sizeof(Data)));
    Config Options;
    Options.forceStaticRegistry = ForceStatic;
    EJit Engine(Options);
    auto *Bounds = Engine.getRegistry().getPointerPeriodBounds("cfg");
    ASSERT_NE(Bounds, nullptr);
    EXPECT_EQ(Bounds->base, Data);
    EXPECT_EQ(Bounds->size, sizeof(Data));
  }
}
