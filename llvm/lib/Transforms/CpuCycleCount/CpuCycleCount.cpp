#include "llvm/Transforms/CpuCycleCount/CpuCycleCount.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/Value.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Casting.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include <string>
#include <vector>

using namespace llvm;

PreservedAnalyses CpuCycleCountPass::run(Module &M, ModuleAnalysisManager &AM) {
  LLVMContext &Ctx = M.getContext();
  Type *VoidTy = Type::getVoidTy(Ctx);
  Type *Int64Ty = Type::getInt64Ty(Ctx);

  FunctionType *StartMeasureFnTy = FunctionType::get(Int64Ty, false);
  FunctionCallee StartMeasureFn = M.getOrInsertFunction("cpu_cycle_start_measurement", StartMeasureFnTy);

  FunctionType *EndMeasureFnTy = FunctionType::get(VoidTy, {Int64Ty}, false);
  FunctionCallee EndMeasureFn = M.getOrInsertFunction("cpu_cycle_end_measurement", EndMeasureFnTy);

  FunctionType *StatsFnTy = FunctionType::get(VoidTy, false);
  FunctionCallee StatsFn = M.getOrInsertFunction("print_cpu_cycle_stats", StatsFnTy);

  FunctionType *ProgramStartFnTy = FunctionType::get(VoidTy, false);
  FunctionCallee ProgramStartFn = M.getOrInsertFunction("cpu_cycle_program_start", ProgramStartFnTy);

  FunctionType *ProgramEndFnTy = FunctionType::get(VoidTy, false);
  FunctionCallee ProgramEndFn = M.getOrInsertFunction("cpu_cycle_program_end", ProgramEndFnTy);

  for (auto *RuntimeFn : {
      dyn_cast<Function>(StartMeasureFn.getCallee()),
      dyn_cast<Function>(EndMeasureFn.getCallee()),
      dyn_cast<Function>(StatsFn.getCallee()),
      dyn_cast<Function>(ProgramStartFn.getCallee()),
      dyn_cast<Function>(ProgramEndFn.getCallee())}) {
    if (RuntimeFn) {
      RuntimeFn->removeFnAttr(Attribute::ReadNone);
      RuntimeFn->removeFnAttr(Attribute::ReadOnly);
      RuntimeFn->addFnAttr(Attribute::NoInline);
      RuntimeFn->setLinkage(GlobalValue::ExternalLinkage);
    }
  }

  bool Modified = false;

  if (Function *MainFn = M.getFunction("main")) {
    if (!MainFn->empty()) {
      BasicBlock &EntryBB = MainFn->getEntryBlock();
      if (!EntryBB.empty()) {
        IRBuilder<> EntryBuilder(&EntryBB.front());
        EntryBuilder.CreateCall(ProgramStartFn);
        Modified = true;
      }
    }
    
    for (BasicBlock &BB : *MainFn) {
      for (Instruction &I : BB) {
        if (auto *RetInst = dyn_cast<ReturnInst>(&I)) {
          IRBuilder<> Builder(RetInst);
          Builder.CreateCall(ProgramEndFn);
          Builder.CreateCall(StatsFn);
          Modified = true;
          break;
        }
      }
    }
  }

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
              if (ActiveMarkerBegin && ActiveMarkerBegin->getParent() == I.getParent()) {
                IRBuilder<> StartBuilder(ActiveMarkerBegin->getNextNode());
                Value *StartCycles = StartBuilder.CreateCall(StartMeasureFn, {}, "start_cycles");

                IRBuilder<> EndBuilder(&I);
                EndBuilder.CreateCall(EndMeasureFn, {StartCycles});
                
                Modified = true;

                ActiveMarkerBegin = nullptr;
              }
            }
          }
        }
      }
    }
  }
  
  return Modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}