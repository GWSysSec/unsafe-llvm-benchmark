//===-- UnsafeInstCounter.cpp - Count unsafe instructions -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/DynamicAnalysis/UnsafeInstCounter.h"
#include "llvm/Transforms/DynamicAnalysis/UnsafeAnalysisUtils.h"
#include "llvm/Transforms/DynamicAnalysis/UnsafeFunctionTracker.h"
#include "llvm/Transforms/InstMarker/InstMarker.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Module.h"

using namespace llvm;

namespace {

constexpr const char *RECORD_BLOCK_FN = "__unsafe_record_block";

/// \brief Check if function should be instrumented
static bool shouldInstrumentFunction(const Function &F) {
  if (F.isDeclaration() || F.isIntrinsic())
    return false;

  StringRef Name = F.getName();
  return !Name.starts_with("__unsafe_") &&
         !Name.starts_with("llvm.");
}

/// \brief Get or create the record block function
static FunctionCallee getOrCreateRecordBlockFn(Module &M) {
  LLVMContext &Ctx = M.getContext();
  Type *VoidTy = Type::getVoidTy(Ctx);
  Type *Int32Ty = Type::getInt32Ty(Ctx);
  Type *Int16Ty = Type::getInt16Ty(Ctx);
  
  // __unsafe_record_block(func_id, total, unsafe_total,
  //   load, store, call_direct, call_indirect, call_intrinsic,
  //   cast, gep, atomic, other)
  FunctionCallee RecordBlockFn = M.getOrInsertFunction(
    RECORD_BLOCK_FN,
    FunctionType::get(VoidTy, {Int32Ty, Int32Ty, Int32Ty,
                               Int16Ty, Int16Ty, Int16Ty,
                               Int16Ty, Int16Ty, Int16Ty,
                               Int16Ty, Int16Ty, Int16Ty}, false)
  );
  
  if (auto *F = dyn_cast<Function>(RecordBlockFn.getCallee())) {
    F->addFnAttr(Attribute::NoInline);
    F->setLinkage(GlobalValue::ExternalLinkage);
  }
  
  return RecordBlockFn;
}

} // anonymous namespace

namespace llvm {

UnsafeInstCounterPass::BlockCounts::BlockCounts() 
  : totalInsts(0), totalUnsafeInsts(0) {
  std::memset(unsafeCounts, 0, sizeof(unsafeCounts));
}

bool UnsafeInstCounterPass::getUnsafeCategory(const Instruction &I,
                                               UnsafeCategory &category) {
  switch (I.getOpcode()) {
    case Instruction::Load:
      category = UNSAFE_LOAD;
      return true;

    case Instruction::Store:
      category = UNSAFE_STORE;
      return true;

    case Instruction::AtomicCmpXchg:
    case Instruction::AtomicRMW:
      category = UNSAFE_ATOMIC;
      return true;

    case Instruction::Call:
    case Instruction::Invoke:
    case Instruction::CallBr: {
      const auto *CB = cast<CallBase>(&I);
      if (CB->isInlineAsm()) {
        category = UNSAFE_OTHER;
      } else if (const Function *Callee = CB->getCalledFunction()) {
        category = Callee->isIntrinsic() ? UNSAFE_CALL_INTRINSIC
                                         : UNSAFE_CALL_DIRECT;
      } else {
        category = UNSAFE_CALL_INDIRECT;
      }
      return true;
    }

    case Instruction::BitCast:
    case Instruction::IntToPtr:
    case Instruction::PtrToInt:
    case Instruction::AddrSpaceCast:
      category = UNSAFE_CAST;
      return true;

    case Instruction::GetElementPtr:
      category = UNSAFE_GEP;
      return true;

    default:
      category = UNSAFE_OTHER;
      return true;
  }
}

UnsafeInstCounterPass::BlockCounts
UnsafeInstCounterPass::analyzeBasicBlock(BasicBlock &BB,
                                         const std::vector<SESERegion> &Regions,
                                         DominatorTree &DT,
                                         PostDominatorTree &PDT) {
  BlockCounts counts;

  for (Instruction &I : BB) {
    // Skip debug intrinsics and markers
    if (isa<DbgInfoIntrinsic>(&I))
      continue;

    bool isBegin = false, isEnd = false;
    if (isMarkerInstruction(I, isBegin, isEnd))
      continue;

    // Count all instructions
    counts.totalInsts++;

    // Count unsafe instructions via SESE region membership
    if (!Regions.empty() && isInSESERegion(I, Regions, DT, PDT)) {
      counts.totalUnsafeInsts++;

      UnsafeCategory category;
      if (getUnsafeCategory(I, category)) {
        counts.unsafeCounts[category]++;
      }
    }
  }

  return counts;
}

uint32_t UnsafeInstCounterPass::getFunctionId(Function &F) {
  MDNode *MD = F.getMetadata(UnsafeFunctionTrackerPass::FUNCTION_ID_METADATA);
  if (!MD) {
    // Function wasn't processed by tracker pass - shouldn't happen
    return UINT32_MAX;
  }
  
  ConstantAsMetadata *CMD = cast<ConstantAsMetadata>(MD->getOperand(0));
  ConstantInt *IdConst = cast<ConstantInt>(CMD->getValue());
  return IdConst->getZExtValue();
}

PreservedAnalyses UnsafeInstCounterPass::run(Function &F,
                                             FunctionAnalysisManager &AM) {
  if (!isPrimaryPackage())
    return PreservedAnalyses::all();

  if (!shouldInstrumentFunction(F))
    return PreservedAnalyses::all();

  // Get function ID from metadata (local per-CGU ID)
  uint32_t funcId = getFunctionId(F);
  if (funcId == UINT32_MAX)
    return PreservedAnalyses::all();

  // Get or create runtime function
  Module *M = F.getParent();
  FunctionCallee RecordBlockFn = getOrCreateRecordBlockFn(*M);

  // Build SESE regions for cross-BB unsafe region detection
  auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
  auto &PDT = AM.getResult<PostDominatorTreeAnalysis>(F);

  std::vector<CallInst *> BeginMarkers, EndMarkers;
  collectMarkers(F, BeginMarkers, EndMarkers);

  std::vector<SESERegion> ValidRegions;
  if (!BeginMarkers.empty())
    validateSESERegions(BeginMarkers, EndMarkers, DT, PDT, ValidRegions);

  // Load the per-module base offset for multi-CGU global ID remapping
  GlobalVariable *BaseGV = M->getGlobalVariable("__unsafe_func_id_base");

  // Analyze and instrument basic blocks
  bool modified = false;
  Type *Int32Ty = Type::getInt32Ty(F.getContext());
  Type *Int16Ty = Type::getInt16Ty(F.getContext());

  for (BasicBlock &BB : F) {
    BlockCounts counts = analyzeBasicBlock(BB, ValidRegions, DT, PDT);

    if (!counts.hasInstructions())
      continue;

    Instruction *Term = BB.getTerminator();
    IRBuilder<> Builder(Term);

    // Compute global func_id = base + local_id (CGU-safe)
    Value *GlobalFuncId;
    if (BaseGV) {
      Value *Base = Builder.CreateLoad(Int32Ty, BaseGV);
      GlobalFuncId = Builder.CreateAdd(Base,
                                       ConstantInt::get(Int32Ty, funcId));
    } else {
      GlobalFuncId = ConstantInt::get(Int32Ty, funcId);
    }

    auto *Call = Builder.CreateCall(RecordBlockFn, {
      GlobalFuncId,
      ConstantInt::get(Int32Ty, counts.totalInsts),
      ConstantInt::get(Int32Ty, counts.totalUnsafeInsts),
      ConstantInt::get(Int16Ty, counts.unsafeCounts[UNSAFE_LOAD]),
      ConstantInt::get(Int16Ty, counts.unsafeCounts[UNSAFE_STORE]),
      ConstantInt::get(Int16Ty, counts.unsafeCounts[UNSAFE_CALL_DIRECT]),
      ConstantInt::get(Int16Ty, counts.unsafeCounts[UNSAFE_CALL_INDIRECT]),
      ConstantInt::get(Int16Ty, counts.unsafeCounts[UNSAFE_CALL_INTRINSIC]),
      ConstantInt::get(Int16Ty, counts.unsafeCounts[UNSAFE_CAST]),
      ConstantInt::get(Int16Ty, counts.unsafeCounts[UNSAFE_GEP]),
      ConstantInt::get(Int16Ty, counts.unsafeCounts[UNSAFE_ATOMIC]),
      ConstantInt::get(Int16Ty, counts.unsafeCounts[UNSAFE_OTHER])
    });
    Call->setDebugLoc(getInstrumentationDebugLoc(Term));

    modified = true;
  }

  return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace llvm
