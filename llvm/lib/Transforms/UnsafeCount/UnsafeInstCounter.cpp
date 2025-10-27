//===-- UnsafeInstCounter.cpp - Count unsafe instructions -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/UnsafeCount/UnsafeInstCounter.h"
#include "llvm/Transforms/UnsafeCount/UnsafeFunctionTracker.h"
#include "llvm/Transforms/InstMarker/InstMarker.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include <cstdlib>
#include <cstring>

using namespace llvm;

namespace {

constexpr const char *RECORD_BLOCK_FN = "__unsafe_record_block";

/// \brief Check if instruction is a marker
static bool isMarkerInstruction(const Instruction &I, bool &isBegin, bool &isEnd) {
  isBegin = false;
  isEnd = false;
  
  if (auto *CI = dyn_cast<CallBase>(&I)) {
    if (auto *IA = dyn_cast<InlineAsm>(CI->getCalledOperand()->stripPointerCasts())) {
      StringRef AsmStr = IA->getAsmString();
      if (AsmStr == UNSAFE_MARKER_BEGIN) {
        isBegin = true;
        return true;
      }
      if (AsmStr == UNSAFE_MARKER_END) {
        isEnd = true;
        return true;
      }
    }
  }
  return false;
}

/// \brief Check if function should be instrumented
static bool shouldInstrumentFunction(const Function &F) {
  if (F.isDeclaration() || F.isIntrinsic())
    return false;
  
  StringRef Name = F.getName();
  return !Name.startswith("__unsafe_") && 
         !Name.startswith("llvm.");
}

/// \brief Get or create the record block function
static FunctionCallee getOrCreateRecordBlockFn(Module &M) {
  LLVMContext &Ctx = M.getContext();
  Type *VoidTy = Type::getVoidTy(Ctx);
  Type *Int32Ty = Type::getInt32Ty(Ctx);
  Type *Int16Ty = Type::getInt16Ty(Ctx);
  
  // __unsafe_record_block(func_id, total, unsafe_total, load, store, call, cast, gep, other)
  FunctionCallee RecordBlockFn = M.getOrInsertFunction(
    RECORD_BLOCK_FN,
    FunctionType::get(VoidTy, {Int32Ty, Int32Ty, Int32Ty,
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

bool UnsafeInstCounterPass::isPrimaryPackage() {
  const char *P = std::getenv("CARGO_PRIMARY_PACKAGE");
  return P && std::strcmp(P, "1") == 0;
}

bool UnsafeInstCounterPass::getUnsafeCategory(const Instruction &I, 
                                               UnsafeCategory &category) {
  switch (I.getOpcode()) {
    case Instruction::Load:
      category = UNSAFE_LOAD;
      return true;
    
    case Instruction::Store:
    case Instruction::AtomicCmpXchg:
    case Instruction::AtomicRMW:
      category = UNSAFE_STORE;
      return true;
    
    case Instruction::Call:
    case Instruction::Invoke:
    case Instruction::CallBr:
      category = UNSAFE_CALL;
      return true;
    
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
UnsafeInstCounterPass::analyzeBasicBlock(BasicBlock &BB) {
  BlockCounts counts;
  bool inUnsafeRegion = false;
  
  for (Instruction &I : BB) {
    // Skip debug intrinsics
    if (isa<DbgInfoIntrinsic>(&I))
      continue;
    
    // Check for markers
    bool isBegin = false, isEnd = false;
    if (isMarkerInstruction(I, isBegin, isEnd)) {
      if (isBegin) {
        inUnsafeRegion = true;
      } else if (isEnd) {
        inUnsafeRegion = false;
      }
      continue; // Don't count markers
    }
    
    // Count all instructions
    counts.totalInsts++;
    
    // Count unsafe instructions if in unsafe region
    if (inUnsafeRegion) {
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
  
  // Get function ID from metadata
  uint32_t funcId = getFunctionId(F);
  if (funcId == UINT32_MAX)
    return PreservedAnalyses::all();
  
  // Get or create runtime function
  Module *M = F.getParent();
  FunctionCallee RecordBlockFn = getOrCreateRecordBlockFn(*M);
  
  // Analyze and instrument basic blocks
  bool modified = false;
  for (BasicBlock &BB : F) {
    BlockCounts counts = analyzeBasicBlock(BB);
    
    // Only instrument blocks with instructions
    if (!counts.hasInstructions())
      continue;
    
    // Only create runtime call for blocks with unsafe instructions
    if (!counts.hasUnsafeInstructions()) {
      // For safe blocks, we still need total instruction count
      // Create a simplified call with all unsafe counts as zero
      IRBuilder<> Builder(BB.getTerminator());
      Builder.CreateCall(RecordBlockFn, {
        ConstantInt::get(Type::getInt32Ty(F.getContext()), funcId),
        ConstantInt::get(Type::getInt32Ty(F.getContext()), counts.totalInsts),
        ConstantInt::get(Type::getInt32Ty(F.getContext()), 0),
        ConstantInt::get(Type::getInt16Ty(F.getContext()), 0),
        ConstantInt::get(Type::getInt16Ty(F.getContext()), 0),
        ConstantInt::get(Type::getInt16Ty(F.getContext()), 0),
        ConstantInt::get(Type::getInt16Ty(F.getContext()), 0),
        ConstantInt::get(Type::getInt16Ty(F.getContext()), 0),
        ConstantInt::get(Type::getInt16Ty(F.getContext()), 0)
      });
    } else {
      // Instrument block with unsafe counts
      IRBuilder<> Builder(BB.getTerminator());
      Builder.CreateCall(RecordBlockFn, {
        ConstantInt::get(Type::getInt32Ty(F.getContext()), funcId),
        ConstantInt::get(Type::getInt32Ty(F.getContext()), counts.totalInsts),
        ConstantInt::get(Type::getInt32Ty(F.getContext()), counts.totalUnsafeInsts),
        ConstantInt::get(Type::getInt16Ty(F.getContext()), counts.unsafeCounts[UNSAFE_LOAD]),
        ConstantInt::get(Type::getInt16Ty(F.getContext()), counts.unsafeCounts[UNSAFE_STORE]),
        ConstantInt::get(Type::getInt16Ty(F.getContext()), counts.unsafeCounts[UNSAFE_CALL]),
        ConstantInt::get(Type::getInt16Ty(F.getContext()), counts.unsafeCounts[UNSAFE_CAST]),
        ConstantInt::get(Type::getInt16Ty(F.getContext()), counts.unsafeCounts[UNSAFE_GEP]),
        ConstantInt::get(Type::getInt16Ty(F.getContext()), counts.unsafeCounts[UNSAFE_OTHER])
      });
    }
    
    modified = true;
  }
  
  return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace llvm
