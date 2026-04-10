//===-- StdlibApiTracker.cpp - Track stdlib API calls in unsafe code -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Scans for inline asm markers inserted by the MIR StdlibApiTracker pass:
//
//   # __unsafe_stdlib_call:std::vec::Vec::<T, A>::set_len
//
// Each marker is replaced with a call to the runtime handler:
//   __unsafe_record_stdlib_call(api_str_ptr, api_str_len)
//
// This two-layer design (MIR markers + LLVM replacement) ensures markers
// survive MIR inlining at O2, where the original stdlib call instructions
// are eliminated before LLVM IR is emitted.
//
//===----------------------------------------------------------------------===//

// UNSAFE-RUST BEGIN

#include "llvm/Transforms/DynamicAnalysis/StdlibApiTracker.h"
#include "llvm/Transforms/DynamicAnalysis/UnsafeAnalysisUtils.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "stdlib-api-tracker"

using namespace llvm;

namespace {

constexpr const char *RECORD_STDLIB_CALL_FN = "__unsafe_record_stdlib_call";

/// \brief Check if function should be instrumented
static bool shouldInstrumentFunction(const Function &F) {
  if (F.isDeclaration() || F.isIntrinsic())
    return false;

  StringRef Name = F.getName();
  return !Name.starts_with("__unsafe_") &&
         !Name.starts_with("llvm.");
}

/// \brief Get or create the runtime recording function.
///
/// Signature: void __unsafe_record_stdlib_call(const char *api_path, uint32_t len)
static FunctionCallee getOrCreateRecordFn(Module &M) {
  LLVMContext &Ctx = M.getContext();
  Type *VoidTy = Type::getVoidTy(Ctx);
  Type *Int8PtrTy = PointerType::getUnqual(Ctx);
  Type *Int32Ty = Type::getInt32Ty(Ctx);

  FunctionCallee Fn = M.getOrInsertFunction(
      RECORD_STDLIB_CALL_FN,
      FunctionType::get(VoidTy, {Int8PtrTy, Int32Ty}, false));

  if (auto *F = dyn_cast<Function>(Fn.getCallee())) {
    F->addFnAttr(Attribute::NoInline);
    F->setLinkage(GlobalValue::ExternalLinkage);
  }

  return Fn;
}

/// \brief Get or create a global constant string for an API path.
///
/// Reuses existing globals with the same name prefix to avoid duplicates
/// across functions within the same module.
static GlobalVariable *getOrCreateApiString(Module &M, StringRef ApiPath) {
  // Use a deterministic name so the same API path shares one global
  std::string GlobalName =
      ("__stdlib_api_str." + ApiPath).str();

  // Replace any characters that are invalid in LLVM global names
  for (char &C : GlobalName) {
    if (C == ':' || C == '<' || C == '>' || C == ',' || C == ' ')
      C = '_';
  }

  if (GlobalVariable *Existing = M.getGlobalVariable(GlobalName))
    return Existing;

  Constant *StrConst =
      ConstantDataArray::getString(M.getContext(), ApiPath, /*AddNull=*/false);
  auto *GV = new GlobalVariable(M, StrConst->getType(), /*isConstant=*/true,
                                GlobalValue::PrivateLinkage, StrConst,
                                GlobalName);
  GV->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
  return GV;
}

} // anonymous namespace

namespace llvm {

PreservedAnalyses StdlibApiTrackerPass::run(Function &F,
                                            FunctionAnalysisManager &AM) {
  if (!isPrimaryPackage())
    return PreservedAnalyses::all();

  if (!shouldInstrumentFunction(F))
    return PreservedAnalyses::all();

  Module *M = F.getParent();
  FunctionCallee RecordFn = getOrCreateRecordFn(*M);
  Type *Int32Ty = Type::getInt32Ty(F.getContext());

  bool Modified = false;
  unsigned InstrumentedCount = 0;

  for (BasicBlock &BB : F) {
    for (auto It = BB.begin(), End = BB.end(); It != End; ) {
      Instruction &I = *It++;

      // Check for MIR-inserted stdlib call markers.
      StringRef ApiPath;
      if (!isStdlibCallMarker(I, ApiPath))
        continue;

      if (ApiPath.empty())
        continue;

      // Get or create the global string constant for this API path
      GlobalVariable *ApiStrGV = getOrCreateApiString(*M, ApiPath);

      // Insert recording call BEFORE erasing the marker (use marker's
      // position so the call lands in the same block).
      IRBuilder<> Builder(&I);

      // GEP to get i8* from the array global
      Value *StrPtr = Builder.CreateInBoundsGEP(
          ApiStrGV->getValueType(), ApiStrGV,
          {Builder.getInt32(0), Builder.getInt32(0)});

      auto *RecordCall = Builder.CreateCall(
          RecordFn, {StrPtr, ConstantInt::get(Int32Ty, ApiPath.size())});
      RecordCall->setDebugLoc(getInstrumentationDebugLoc(&I));

      // Erase the inline asm marker — it has been replaced.
      I.eraseFromParent();

      Modified = true;
      InstrumentedCount++;

      LLVM_DEBUG(dbgs() << "stdlib-api-tracker: instrumented call to '"
                        << ApiPath << "' in " << F.getName() << "\n");
    }
  }

  LLVM_DEBUG(if (InstrumentedCount > 0) {
    dbgs() << "stdlib-api-tracker: " << F.getName() << " — "
           << InstrumentedCount << " stdlib call(s) instrumented\n";
  });

  return Modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace llvm

// UNSAFE-RUST END
