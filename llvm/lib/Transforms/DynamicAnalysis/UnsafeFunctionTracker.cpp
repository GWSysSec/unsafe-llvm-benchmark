//===-- UnsafeFunctionTracker.cpp - Track unsafe functions -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/DynamicAnalysis/UnsafeFunctionTracker.h"
#include "llvm/Transforms/DynamicAnalysis/UnsafeAnalysisUtils.h"
#include "llvm/Transforms/InstMarker/InstMarker.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Type.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include <vector>

using namespace llvm;

namespace {

constexpr const char *INIT_METADATA_FN = "__unsafe_init_metadata";
constexpr const char *RECORD_FUNCTION_FN = "__unsafe_record_function";
constexpr const char *DUMP_STATS_FN = "__unsafe_dump_stats";

/// \brief Check if instruction has unsafe metadata
static bool hasUnsafeMetadata(const Instruction &I) {
  return I.getMetadata("unsafe_inst") != nullptr;
}

/// \brief Check if function should be instrumented
static bool shouldInstrumentFunction(const Function &F) {
  if (F.isDeclaration() || F.isIntrinsic())
    return false;
  
  StringRef Name = F.getName();
  return !Name.starts_with("__unsafe_") &&
         !Name.starts_with("llvm.");
}

/// \brief Analysis result for a function's unsafe characteristics
struct FunctionUnsafeInfo {
  bool hasUnsafeRegions;  // Function contains marker pairs
  bool hasUnsafeInst;     // Function has !unsafe_inst metadata inside a region
};

/// \brief Analyze function for unsafe characteristics
///
/// Two flags are tracked independently:
///
/// - hasUnsafeRegions: marker pairs exist in the function.  This is the
///   primary signal — InstMarker inserts markers around unsafe blocks
///   before optimization, and they survive as durable inline asm.
///
/// - hasUnsafeInst: at least one instruction inside a marker region carries
///   !unsafe_inst metadata.  At O0 this refines the picture (a region can
///   contain only safe operations).  At O2 the metadata *may* survive on
///   instructions that were not eliminated, but is not guaranteed — so
///   hasUnsafeRegions is the authoritative flag for "function has unsafe code."
static FunctionUnsafeInfo analyzeFunction(Function &F) {
  FunctionUnsafeInfo Info = {false, false};
  bool inRegion = false;

  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      bool isBegin = false, isEnd = false;
      if (isMarkerInstruction(I, isBegin, isEnd)) {
        if (isBegin) {
          inRegion = true;
          Info.hasUnsafeRegions = true;
        } else if (isEnd) {
          inRegion = false;
        }
        continue;
      }

      if (inRegion && hasUnsafeMetadata(I))
        Info.hasUnsafeInst = true;
    }
  }

  return Info;
}

} // anonymous namespace

namespace llvm {

constexpr const char *UnsafeFunctionTrackerPass::FUNCTION_ID_METADATA;

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

    FunctionUnsafeInfo Info = analyzeFunction(F);

    metadata.push_back({
      nextId++,
      static_cast<uint8_t>(Info.hasUnsafeInst ? 1 : 0),
      static_cast<uint8_t>(Info.hasUnsafeRegions ? 1 : 0),
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

  // init_metadata returns u32 base offset for global ID remapping
  FunctionCallee InitMetadataFn = M.getOrInsertFunction(
    INIT_METADATA_FN,
    FunctionType::get(Int32Ty, {Int8PtrTy, Int32Ty}, false)
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
  
  // Phase 4: Create per-module base offset global and initialization function.
  // Each CGU gets its own base offset from init_metadata(); record_function
  // uses (base + local_id) so IDs are globally unique across CGUs.
  GlobalVariable *BaseGV = new GlobalVariable(
    M, Int32Ty, false, GlobalValue::InternalLinkage,
    ConstantInt::get(Int32Ty, 0), "__unsafe_func_id_base"
  );

  Function *InitFunc = Function::Create(
    FunctionType::get(VoidTy, false),
    GlobalValue::InternalLinkage,
    "__unsafe_module_init", &M
  );

  BasicBlock *InitBB = BasicBlock::Create(Ctx, "entry", InitFunc);
  IRBuilder<> Builder(InitBB);

  Value *TablePtr = Builder.CreateBitCast(GV, Int8PtrTy);
  Value *Count = ConstantInt::get(Int32Ty, metadata.size());
  Value *Base = Builder.CreateCall(InitMetadataFn, {TablePtr, Count});
  Builder.CreateStore(Base, BaseGV);
  Builder.CreateRetVoid();

  appendToGlobalCtors(M, InitFunc, 0);

  // Register destructor
  if (auto *F = dyn_cast<Function>(DumpStatsFn.getCallee())) {
    appendToGlobalDtors(M, F, 0);
  }

  // Phase 5: Instrument function entries with (base + local_id)
  for (Function *F : functionsToInstrument) {
    BasicBlock &EntryBB = F->getEntryBlock();
    Instruction *InsertPt = &EntryBB.front();
    IRBuilder<> EntryBuilder(InsertPt);

    // Get local function ID from metadata
    MDNode *MD = F->getMetadata(FUNCTION_ID_METADATA);
    ConstantAsMetadata *CMD = cast<ConstantAsMetadata>(MD->getOperand(0));
    ConstantInt *IdConst = cast<ConstantInt>(CMD->getValue());

    // Global ID = base + local_id
    Value *Base = EntryBuilder.CreateLoad(Int32Ty, BaseGV);
    Value *GlobalId = EntryBuilder.CreateAdd(Base, IdConst);

    auto *Call = EntryBuilder.CreateCall(RecordFunctionFn, {GlobalId});
    Call->setDebugLoc(getInstrumentationDebugLoc(InsertPt));
  }
  
  return PreservedAnalyses::none();
}

} // namespace llvm
