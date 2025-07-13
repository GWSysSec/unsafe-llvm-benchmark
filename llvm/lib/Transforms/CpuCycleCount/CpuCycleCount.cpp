#include "llvm/Transforms/CpuCycleCount/CpuCycleCount.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

using namespace llvm;

PreservedAnalyses CpuCycleCountPass::run(Module &M, ModuleAnalysisManager &AM) {
  LLVMContext &Ctx = M.getContext();

  Type *VoidTy = Type::getVoidTy(Ctx);
  Type *Int64Ty = Type::getInt64Ty(Ctx);

  FunctionCallee StartMeasureFn = M.getOrInsertFunction(
    "cpu_cycle_start_measurement", FunctionType::get(Int64Ty, false));

  FunctionCallee EndMeasureFn = M.getOrInsertFunction(
    "cpu_cycle_end_measurement", FunctionType::get(VoidTy, {Int64Ty}, false));

  FunctionCallee StatsFn = M.getOrInsertFunction(
    "print_cpu_cycle_stats", FunctionType::get(VoidTy, false));

  FunctionCallee ProgramStartFn = M.getOrInsertFunction(
    "cpu_cycle_program_start", FunctionType::get(VoidTy, false));

  FunctionCallee ProgramEndFn = M.getOrInsertFunction(
    "cpu_cycle_program_end", FunctionType::get(VoidTy, false));

  for (auto *FnHandle : {&StartMeasureFn, &EndMeasureFn, &StatsFn, &ProgramStartFn, &ProgramEndFn}) {
    if (auto *F = dyn_cast<Function>(FnHandle->getCallee())) {
      F->removeFnAttr(Attribute::ReadNone);
      F->removeFnAttr(Attribute::ReadOnly);
      F->addFnAttr(Attribute::NoInline);
      F->setLinkage(GlobalValue::ExternalLinkage);
    }
  }

  appendToGlobalCtors(M, cast<Function>(ProgramStartFn.getCallee()), 0);
  appendToGlobalDtors(M, cast<Function>(ProgramEndFn.getCallee()), 0);
  appendToGlobalDtors(M, cast<Function>(StatsFn.getCallee()), 0);

  bool Modified = false;

  for (Function &F : M) {
    if (F.isDeclaration())
      continue;

    Instruction *ActiveMarkerBegin = nullptr;

    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        if (auto *CallInst = dyn_cast<CallBase>(&I)) {
          if (auto *InlineAsmCall = dyn_cast<InlineAsm>(CallInst->getCalledOperand()->stripPointerCasts())) {
            StringRef AsmStr = InlineAsmCall->getAsmString();

            if (AsmStr.contains("marker_begin")) {
              ActiveMarkerBegin = &I;
            } else if (AsmStr.contains("marker_end")) {
              if (ActiveMarkerBegin && ActiveMarkerBegin->getParent() == &BB) {
                IRBuilder<> StartBuilder(ActiveMarkerBegin->getNextNode());
                Value *StartCycles = StartBuilder.CreateCall(StartMeasureFn, {}, "start_cycles");

                IRBuilder<> EndBuilder(&I);
                EndBuilder.CreateCall(EndMeasureFn, {StartCycles});

                Modified = true;
                ActiveMarkerBegin = nullptr; // Reset for the next pair
              }
            }
          }
        }
      }
    }
  }

  return Modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
