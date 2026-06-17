//===-- EJitOptimizer.cpp - JIT Optimization Pipeline ---------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitOptimizer.h"
#include "llvm/ExecutionEngine/EJIT/EJitStructFieldPass.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/ExecutionEngine/EJIT/EJitPassBuilder.h"
#include "llvm/Support/Debug.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
// JIT Inline disabled: AOT pre-optimization already inlines.
// #include "llvm/Transforms/IPO/AlwaysInliner.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar/ADCE.h"
#include "llvm/Transforms/Scalar/SCCP.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Scalar/LoopPassManager.h"
#include "llvm/Transforms/Scalar/LoopUnrollPass.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"
#include "llvm/Transforms/Utils/Mem2Reg.h"

using namespace llvm;
using namespace llvm::ejit;

#define DEBUG_TYPE "ejit-optimizer"

namespace {

static bool ejitTracePasses() {
  static int cached = -1;
  if (cached < 0)
    cached = (std::getenv("EJIT_TRACE_PASSES") != nullptr) ? 1 : 0;
  return cached != 0;
}

struct PassTimer {
  using Clock = std::chrono::steady_clock;
  Clock::time_point t0;
  const char *name;
  explicit PassTimer(const char *n) : t0(Clock::now()), name(n) {}
  ~PassTimer() {
    if (!ejitTracePasses()) return;
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                  Clock::now() - t0)
                  .count();
    std::fprintf(stderr, "[ejit-passes] %-30s %6lld us\n", name,
                 static_cast<long long>(us));
  }
};

} // namespace

EJitOptimizer::EJitOptimizer(PeriodArrayRegistry &reg)
    : registry_(reg) {
  EJitPassBuilder::registerFunctionAnalyses(FAM_);
  EJitPassBuilder::registerLoopAnalyses(LAM_);
  EJitPassBuilder::registerCGSCCAnalyses(CGAM_);
  EJitPassBuilder::registerModuleAnalyses(MAM_);
  EJitPassBuilder::crossRegisterProxies(LAM_, FAM_, CGAM_, MAM_);

  // Pre-build cached pass pipelines.
  // L1: SCCP + ADCE + SimplifyCFG (always runs).
  L1FPM_.addPass(SCCPPass());
  L1FPM_.addPass(ADCEPass());
  L1FPM_.addPass(SimplifyCFGPass());

  // L2: SimplifyCFG cleanup after inlining.
  L2FPM_.addPass(SimplifyCFGPass());

  // L3: LoopSimplify + FullUnroll + Promote (Mem2Reg) + SimplifyCFG.
  L3FPM_.addPass(LoopSimplifyPass());
  {
    LoopPassManager LPM;
    LPM.addPass(LoopFullUnrollPass());
    L3FPM_.addPass(createFunctionToLoopPassAdaptor(std::move(LPM)));
  }
  L3FPM_.addPass(PromotePass());
  L3FPM_.addPass(SimplifyCFGPass());
}

void EJitOptimizer::clearAnalyses() {
  FAM_.clear();
  LAM_.clear();
  CGAM_.clear();
  MAM_.clear();
}

void EJitOptimizer::runPipeline(Module &M,
                                const SpecializationContext &ctx) {
  // 1. Parameter substitution: replace ejit_period_arr_ind args with constants
  { PassTimer _t("preReplacePeriodIndices"); preReplacePeriodIndices(M, ctx); }

  // 2. InstCombine: fold constant GEP chains from substituted params
  //    so StructFieldPass can compute correct byte offsets.
  { PassTimer _t("InstCombine (pre-struct)"); runInstCombine(M); }

  // 3. Inline (L2+): currently disabled.
  // if (static_cast<int>(ctx.optLevel) >= 2) { ... }

  // 4. StructFieldPass: replace may_const loads with runtime constants.
  { PassTimer _t("StructFieldPass"); runStructFieldPass(M); }

  // 5. Core optimization at the configured level
  { PassTimer _t("OptimizationPipeline"); runOptimizationPipeline(M, ctx.optLevel); }
}

void EJitOptimizer::preReplacePeriodIndices(
    Module &M, const SpecializationContext &ctx) {
  LLVM_DEBUG(dbgs() << "ejit-optimizer: preReplacePeriodIndices, "
                    << ctx.dimensions.size() << " dim(s)\n");
  for (Function &F : M.functions()) {
    MDNode *MD = F.getMetadata(MD_EJIT_METADATA);
    if (!MD)
      continue;

    for (const MDOperand &Op : MD->operands()) {
      auto *Sub = dyn_cast<MDNode>(Op.get());
      if (!Sub || Sub->getNumOperands() < 3)
        continue;

      auto *Tag = dyn_cast<MDString>(Sub->getOperand(0));
      if (!Tag || Tag->getString() != TAG_EJIT_PERIOD_ARR_IND)
        continue;

      auto *PN = dyn_cast<MDString>(Sub->getOperand(1));
      auto *IdxC = mdconst::dyn_extract<ConstantInt>(Sub->getOperand(2));
      if (!PN || !IdxC)
        continue;

      unsigned argIdx = static_cast<unsigned>(IdxC->getZExtValue());
      if (argIdx >= F.arg_size())
        continue;

      for (auto &dim : ctx.dimensions) {
        if (dim.periodName == PN->getString()) {
          Argument *arg = F.getArg(argIdx);
          arg->replaceAllUsesWith(
              ConstantInt::get(arg->getType(), dim.cellIdx));
          break;
        }
      }
    }
  }
}

void EJitOptimizer::runInstCombine(Module &M) {
  FunctionPassManager FPM;
  FPM.addPass(InstCombinePass());

  for (Function &F : M.functions())
    if (!F.isDeclaration())
      FPM.run(F, FAM_);
}

void EJitOptimizer::runStructFieldPass(Module &M) {
  EJitStructFieldPass structField(registry_);
  structField.initFromModule(M);
  for (Function &F : M.functions())
    if (!F.isDeclaration())
      structField.run(F, FAM_);
}

void EJitOptimizer::runOptimizationPipeline(Module &M,
                                            OptimizationLevel level) {
  // L1: SCCP + ADCE + SimplifyCFG
  { PassTimer _t("L1: SCCP+ADCE+SimplifyCFG");
    for (Function &F : M.functions())
      if (!F.isDeclaration())
        L1FPM_.run(F, FAM_);
  }

  // L2: SimplifyCFG cleanup
  if (static_cast<int>(level) >= 2) {
    PassTimer _t("L2: SimplifyCFG");
    for (Function &F : M.functions())
      if (!F.isDeclaration())
        L2FPM_.run(F, FAM_);
  }

  // L3: LoopSimplify + FullUnroll + Promote + SimplifyCFG, then re-run
  // StructFieldPass because unrolling can expose new may_const GEPs.
  if (static_cast<int>(level) >= 3) {
    { PassTimer _t("L3: LoopSimplify+Unroll+Promote");
      for (Function &F : M.functions())
        if (!F.isDeclaration())
          L3FPM_.run(F, FAM_);
    }
    { PassTimer _t("L3: StructFieldPass (post-unroll)"); runStructFieldPass(M); }
    { PassTimer _t("L3: InstCombine (post-unroll)");     runInstCombine(M); }
  }
}
