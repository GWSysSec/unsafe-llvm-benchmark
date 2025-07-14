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
static const char *TOUCH_TRACKER_FN     = "touch_thread_tracker";

/// @brief Instruments unsafe blocks marked by InstMarkerPass to measure CPU cycles.
/// @param F The target function.
/// @param startFn The function to call at the beginning of an unsafe block.
/// @param endFn The function to call at the end of an unsafe block.
/// @return True if the function was modified, false otherwise.
static bool instrumentUnsafeBlocks(Function &F, FunctionCallee startFn, FunctionCallee endFn) {
    bool Modified = false;
    LLVMContext &Ctx = F.getContext();

    for (BasicBlock &BB : F) {
        Instruction *ActiveMarkerBegin = nullptr;

        for (Instruction &I : BB) {
            if (auto *CallInst = dyn_cast<CallBase>(&I)) {
                if (auto *InlineAsmCall = dyn_cast<InlineAsm>(CallInst->getCalledOperand()->stripPointerCasts())) {
                    StringRef AsmStr = InlineAsmCall->getAsmString();

                    if (AsmStr.contains("marker_begin")) {
                        ActiveMarkerBegin = &I;
                    } else if (AsmStr.contains("marker_end")) {
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
    Type *VoidTy = Type::getVoidTy(Ctx);
    Type *Int64Ty = Type::getInt64Ty(Ctx);

    FunctionCallee TouchTrackerFn = M.getOrInsertFunction(
        TOUCH_TRACKER_FN, FunctionType::get(VoidTy, false));
    FunctionCallee StartMeasureFn = M.getOrInsertFunction(
        START_MEASUREMENT_FN, FunctionType::get(Int64Ty, false));
    FunctionCallee EndMeasureFn = M.getOrInsertFunction(
        END_MEASUREMENT_FN, FunctionType::get(VoidTy, {Int64Ty}, false));
    FunctionCallee StatsFn = M.getOrInsertFunction(
        PRINT_STATS_FN, FunctionType::get(VoidTy, false));

    // Ensure the runtime functions are not inlined and are externally linked
    for (auto *FnHandle : {&TouchTrackerFn, &StartMeasureFn, &EndMeasureFn, &StatsFn}) {
        if (auto *F = dyn_cast<Function>(FnHandle->getCallee())) {
            F->addFnAttr(Attribute::NoInline);
            F->setLinkage(GlobalValue::ExternalLinkage);
        }
    }

    // Register the stats printing function to be called at program exit
    appendToGlobalDtors(M, cast<Function>(StatsFn.getCallee()), 0);

    bool Modified = false;
    for (Function &F : M) {
        // Skip function declarations and our own runtime functions
        if (F.isDeclaration() || F.getName() == TOUCH_TRACKER_FN ||
            F.getName() == START_MEASUREMENT_FN || F.getName() == END_MEASUREMENT_FN ||
            F.getName() == PRINT_STATS_FN) {
            continue;
        }

        // Instrument the entry of every function to initialize the thread tracker
        IRBuilder<> Builder(&F.getEntryBlock().front());
        Builder.CreateCall(TouchTrackerFn, {});
        Modified = true;

        // Runs the logic to instrument specific unsafe blocks
        if (instrumentUnsafeBlocks(F, StartMeasureFn, EndMeasureFn)) {
            Modified = true;
        }
    }

    return Modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
