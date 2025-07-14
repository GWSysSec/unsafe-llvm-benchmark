//===--- CpuCycleCount.cpp - Counting CPU cycles for unsafe blocks ---*- C++ -*-===//
//
// This pass instruments unsafe code blocks to measure the CPU cycles they consume.
// It relies on markers previously inserted by the InstMarkerPass.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/CpuCycleCount/CpuCycleCount.h"
#include "llvm/Transforms/InstMarker/InstMarker.h"
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

static const char *START_MEASUREMENT_FN = "cpu_cycle_start_measurement";
static const char *END_MEASUREMENT_FN   = "cpu_cycle_end_measurement";
static const char *PRINT_STATS_FN       = "print_cpu_cycle_stats";

/// @brief Instruments unsafe blocks marked by InstMarkerPass to measure CPU cycles.
/// @param F The target function.
/// @param startFn The function to call at the beginning of an unsafe block.
/// @param endFn The function to call at the end of an unsafe block.
/// @return True if the function was modified, false otherwise.
static bool instrumentUnsafeBlocks(Function &F, FunctionCallee startFn, FunctionCallee endFn) {
    bool Modified = false;

    for (BasicBlock &BB : F) {
        Instruction *ActiveMarkerBegin = nullptr;

        for (Instruction &I : BB) {
            if (auto *CallInst = dyn_cast<CallBase>(&I)) {
                if (auto *InlineAsmCall = dyn_cast<InlineAsm>(CallInst->getCalledOperand()->stripPointerCasts())) {
                    StringRef AsmStr = InlineAsmCall->getAsmString();

                    if (AsmStr == UNSAFE_MARKER_BEGIN) {
                        ActiveMarkerBegin = &I;
                    } else if (AsmStr == UNSAFE_MARKER_END) {
                        if (ActiveMarkerBegin) {
                            IRBuilder<> StartBuilder(ActiveMarkerBegin->getNextNode());
                            Value *StartCycles = StartBuilder.CreateCall(startFn, {});

                            IRBuilder<> EndBuilder(&I);
                            EndBuilder.CreateCall(endFn, {StartCycles});

                            ActiveMarkerBegin = nullptr;
                            Modified = true;
                        }
                    }
                }
            }
        }
    }
    return Modified;
}

PreservedAnalyses CpuCycleCountPass::run(Module &M, ModuleAnalysisManager &AM) {
    LLVMContext &Ctx = M.getContext();

    // Define the function prototypes for the Rust runtime functions.
    Type *VoidTy = Type::getVoidTy(Ctx);
    Type *Int64Ty = Type::getInt64Ty(Ctx);

    FunctionCallee StartMeasureFn = M.getOrInsertFunction(
        START_MEASUREMENT_FN, FunctionType::get(Int64Ty, false));

    FunctionCallee EndMeasureFn = M.getOrInsertFunction(
        END_MEASUREMENT_FN, FunctionType::get(VoidTy, {Int64Ty}, false));

    FunctionCallee StatsFn = M.getOrInsertFunction(
        PRINT_STATS_FN, FunctionType::get(VoidTy, false));
    
    // Ensure the runtime functions are not inlined and are externally linked.
    for (auto *FnHandle : {&StartMeasureFn, &EndMeasureFn, &StatsFn}) {
        if (auto *F = dyn_cast<Function>(FnHandle->getCallee())) {
            F->addFnAttr(Attribute::NoInline);
            F->setLinkage(GlobalValue::ExternalLinkage);
        }
    }

    // Register the stats printing function to be called at program exit.
    appendToGlobalDtors(M, cast<Function>(StatsFn.getCallee()), 0);

    bool Modified = false;
    for (Function &F : M) {
        if (F.isDeclaration())
            continue;
        
        if (instrumentUnsafeBlocks(F, StartMeasureFn, EndMeasureFn)) {
            Modified = true;
        }
    }

    return Modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
