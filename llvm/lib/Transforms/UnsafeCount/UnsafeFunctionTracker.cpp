//===-- UnsafeFunctionTracker.cpp - Track unsafe functions -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/UnsafeCount/UnsafeFunctionTracker.h"
#include "llvm/Transforms/InstMarker/InstMarker.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include <vector>
#include <cstdlib>
#include <cstring>

using namespace llvm;

namespace {

constexpr const char *INIT_METADATA_FN = "__unsafe_init_metadata";
constexpr const char *RECORD_FUNCTION_FN = "__unsafe_record_function";
constexpr const char *DUMP_STATS_FN = "__unsafe_dump_stats";

/// \brief Check if instruction is a marker
static bool isMarkerInstruction(const Instruction &I) {
  if (auto *CI = dyn_cast<CallBase>(&I)) {
    if (auto *IA = dyn_cast<InlineAsm>(CI->getCalledOperand()->stripPointerCasts())) {
      StringRef AsmStr = IA->getAsmString();
      return AsmStr == UNSAFE_MARKER_BEGIN || AsmStr == UNSAFE_MARKER_END;
    }
  }
  return false;
}

/// \brief Check if instruction has unsafe metadata
static bool hasUnsafeMetadata(const Instruction &I) {
  return I.getMetadata("unsafe_inst") != nullptr;
}

/// \brief Check if function should be instrumented
static bool shouldInstrumentFunction(const Function &F) {
  if (F.isDeclaration() || F.isIntrinsic())
    return false;
  
  StringRef Name = F.getName();
  return !Name.startswith("__unsafe_") && 
         !Name.startswith("llvm.");
}

/// \brief Analyze function for unsafe characteristics according to new criteria
static bool analyzeFunction(Function &F) {
  // Scan for regions and metadata inside regions
  bool inUnsafeRegion = false;
  bool foundUnsafeInstInRegion = false;

  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      // Look for region markers
      if (isMarkerInstruction(I)) {
        auto *CI = dyn_cast<CallBase>(&I);
        auto *IA = dyn_cast<InlineAsm>(CI->getCalledOperand()->stripPointerCasts());
        StringRef AsmStr = IA->getAsmString();

        if (AsmStr == UNSAFE_MARKER_BEGIN)
          inUnsafeRegion = true;
        else if (AsmStr == UNSAFE_MARKER_END)
          inUnsafeRegion = false;

        continue;
      }

      // Only check for unsafe_inst metadata if inside region
      if (inUnsafeRegion && hasUnsafeMetadata(I)) {
        foundUnsafeInstInRegion = true;
        // No need to continue, one is enough
        return true;
      }
    }
  }

  // Only true if at least one unsafe_inst is found inside a region
  return false;
}

} // anonymous namespace

namespace llvm {

constexpr const char *UnsafeFunctionTrackerPass::FUNCTION_ID_METADATA;

bool UnsafeFunctionTrackerPass::isPrimaryPackage() {
  const char *P = std::getenv("CARGO_PRIMARY_PACKAGE");
  return P && std::strcmp(P, "1") == 0;
}

PreservedAnalyses UnsafeFunctionTrackerPass::run(Module &M, ModuleAnalysisManager &AM) {
  if (!isPrimaryPackage())
    return PreservedAnalyses::all();
  
  LLVMContext &Ctx = M.getContext();
  std::vector<FunctionMetadata> metadata;
  std::vector<Function*> functionsToInstrument;
  
  // Phase 1: Analyze all functions and assign IDs
  uint32_t nextId = 0;
  for (Function &F : M) {
    if (!shouldInstrumentFunction(F))
      continue;

    F.setMetadata(FUNCTION_ID_METADATA, 
                  MDNode::get(Ctx, ConstantAsMetadata::get(
                    ConstantInt::get(Type::getInt32Ty(Ctx), nextId))));

    bool isUnsafe = analyzeFunction(F);

    metadata.push_back({
      nextId++,
      static_cast<uint8_t>(isUnsafe ? 1 : 0), // Now only track real unsafe functions
      0, // Optionally drop hasUnsafeRegions, or keep for extra info
      0
    });

    functionsToInstrument.push_back(&F);
  }
  
  if (metadata.empty())
    return PreservedAnalyses::all();
  
  // Phase 2: Setup runtime functions
  Type *VoidTy = Type::getVoidTy(Ctx);
  Type *Int32Ty = Type::getInt32Ty(Ctx);
  Type *Int8PtrTy = PointerType::get(Type::getInt8Ty(Ctx), 0);
  
  FunctionCallee InitMetadataFn = M.getOrInsertFunction(
    INIT_METADATA_FN,
    FunctionType::get(VoidTy, {Int8PtrTy, Int32Ty}, false)
  );
  
  FunctionCallee RecordFunctionFn = M.getOrInsertFunction(
    RECORD_FUNCTION_FN,
    FunctionType::get(VoidTy, {Int32Ty}, false)
  );
  
  FunctionCallee DumpStatsFn = M.getOrInsertFunction(
    DUMP_STATS_FN,
    FunctionType::get(VoidTy, {}, false)
  );
  
  // Set attributes
  for (auto *FnCallee : {&InitMetadataFn, &RecordFunctionFn, &DumpStatsFn}) {
    if (auto *F = dyn_cast<Function>(FnCallee->getCallee())) {
      F->addFnAttr(Attribute::NoInline);
      F->setLinkage(GlobalValue::ExternalLinkage);
    }
  }
  
  // Phase 3: Create global metadata table
  StructType *MetadataType = StructType::get(
    Int32Ty,                    // id
    Type::getInt8Ty(Ctx),      // hasUnsafeInst
    Type::getInt8Ty(Ctx),      // hasUnsafeRegions
    Type::getInt16Ty(Ctx)      // padding
  );
  
  std::vector<Constant*> MetadataElems;
  for (const auto &meta : metadata) {
    MetadataElems.push_back(ConstantStruct::get(
      MetadataType,
      ConstantInt::get(Int32Ty, meta.id),
      ConstantInt::get(Type::getInt8Ty(Ctx), meta.hasUnsafeInst),
      ConstantInt::get(Type::getInt8Ty(Ctx), meta.hasUnsafeRegions),
      ConstantInt::get(Type::getInt16Ty(Ctx), 0)
    ));
  }
  
  ArrayType *ArrayTy = ArrayType::get(MetadataType, MetadataElems.size());
  Constant *MetadataArray = ConstantArray::get(ArrayTy, MetadataElems);
  
  GlobalVariable *GV = new GlobalVariable(
    M, ArrayTy, true, GlobalValue::InternalLinkage,
    MetadataArray, "__unsafe_metadata_table"
  );
  GV->setAlignment(Align(8));
  
  // Phase 4: Create initialization function
  Function *InitFunc = Function::Create(
    FunctionType::get(VoidTy, false),
    GlobalValue::InternalLinkage,
    "__unsafe_module_init", &M
  );
  
  BasicBlock *InitBB = BasicBlock::Create(Ctx, "entry", InitFunc);
  IRBuilder<> Builder(InitBB);
  
  Value *TablePtr = Builder.CreateBitCast(GV, Int8PtrTy);
  Value *Count = ConstantInt::get(Int32Ty, metadata.size());
  Builder.CreateCall(InitMetadataFn, {TablePtr, Count});
  Builder.CreateRetVoid();
  
  appendToGlobalCtors(M, InitFunc, 0);
  
  // Register destructor
  if (auto *F = dyn_cast<Function>(DumpStatsFn.getCallee())) {
    appendToGlobalDtors(M, F, 0);
  }
  
  // Phase 5: Instrument function entries
  for (Function *F : functionsToInstrument) {
    BasicBlock &EntryBB = F->getEntryBlock();
    IRBuilder<> EntryBuilder(&EntryBB.front());
    
    // Get function ID from metadata
    MDNode *MD = F->getMetadata(FUNCTION_ID_METADATA);
    ConstantAsMetadata *CMD = cast<ConstantAsMetadata>(MD->getOperand(0));
    ConstantInt *IdConst = cast<ConstantInt>(CMD->getValue());
    
    EntryBuilder.CreateCall(RecordFunctionFn, {IdConst});
  }
  
  return PreservedAnalyses::none();
}

} // namespace llvm
