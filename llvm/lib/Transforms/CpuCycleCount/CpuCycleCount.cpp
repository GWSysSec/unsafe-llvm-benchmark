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

PreservedAnalyses CpuCycleCountPass::run(Module &M, ModuleAnalysisManager &AM) {
    LLVMContext &Ctx = M.getContext();

    // Define the function signatures for the Rust runtime functions
    Type *VoidTy = Type::getVoidTy(Ctx);
    Type *Int64Ty = Type::getInt64Ty(Ctx);

    FunctionCallee StartMeasureFn = M.getOrInsertFunction(
        "cpu_cycle_start_measurement", FunctionType::get(Int64Ty, false));

    FunctionCallee EndMeasureFn = M.getOrInsertFunction(
        "cpu_cycle_end_measurement", FunctionType::get(VoidTy, {Int64Ty}, false));

    FunctionCallee StatsFn = M.getOrInsertFunction(
        "print_cpu_cycle_stats", FunctionType::get(VoidTy, false));
    
    for (auto *FnHandle : {&StartMeasureFn, &EndMeasureFn, &StatsFn}) {
        if (auto *F = dyn_cast<Function>(FnHandle->getCallee())) {
            F->addFnAttr(Attribute::NoInline);
            F->setLinkage(GlobalValue::ExternalLinkage);
        }
    }

    appendToGlobalDtors(M, cast<Function>(StatsFn.getCallee()), 0);

    bool Modified = false;

    for (Function &F : M) {
        if (F.isDeclaration())
            continue;

        // This guarantees it's available everywhere in the function, solving dominance issues.
        AllocaInst* StartCycleVar = nullptr;

        // We use a vector to store marker pairs to avoid modifying the list while iterating.
        std::vector<std::pair<Instruction*, Instruction*>> MarkerPairs;
        Instruction *ActiveMarkerBegin = nullptr;

        for (BasicBlock &BB : F) {
            for (Instruction &I : BB) {
                if (auto *CallInst = dyn_cast<CallBase>(&I)) {
                    if (auto *InlineAsmCall = dyn_cast<InlineAsm>(CallInst->getCalledOperand()->stripPointerCasts())) {
                        StringRef AsmStr = InlineAsmCall->getAsmString();

                        if (AsmStr == UNSAFE_MARKER_BEGIN) {
                            ActiveMarkerBegin = &I;
                        } else if (AsmStr == UNSAFE_MARKER_END) {
                            if (ActiveMarkerBegin) {
                                MarkerPairs.push_back({ActiveMarkerBegin, &I});
                                ActiveMarkerBegin = nullptr; // Reset for the next pair
                            }
                        }
                    }
                }
            }
        }

        // If we found any pairs, now we can instrument them.
        if (!MarkerPairs.empty()) {
            // Create the stack variable only if it's needed.
            IRBuilder<> EntryBuilder(&F.getEntryBlock().front());
            StartCycleVar = EntryBuilder.CreateAlloca(Int64Ty, nullptr, "start_cycle_var");

            for (auto &Pair : MarkerPairs) {
                Instruction* Begin = Pair.first;
                Instruction* End = Pair.second;

                // Store the start time in the stack variable.
                IRBuilder<> StartBuilder(Begin->getNextNode());
                Value *StartCycles = StartBuilder.CreateCall(StartMeasureFn, {});
                StartBuilder.CreateStore(StartCycles, StartCycleVar);

                // Load the start time from the stack variable and pass it to the end function.
                IRBuilder<> EndBuilder(End);
                Value* LoadedStartCycles = EndBuilder.CreateLoad(Int64Ty, StartCycleVar);
                EndBuilder.CreateCall(EndMeasureFn, {LoadedStartCycles});
            }
            Modified = true;
        }
    }

    return Modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
