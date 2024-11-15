//===--------------InlineMarker.cpp ---------------------===//

// This pass will add an inline marker before and after unsafe instruction block.


#include "llvm/Transforms/WorkDir/InlineMarker.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/IR/PassManager.h"
#include <vector>


using namespace llvm;

PreservedAnalyses InlineMarker::run(Function &F,
                                           FunctionAnalysisManager &AM) {
bool modified = false;

        // Prototype for Inline Assembly markers
        auto *VoidTy = Type::getVoidTy(F.getContext());
        InlineAsm *asmMarkerBegin = InlineAsm::get(FunctionType::get(VoidTy, false),
                                                   "nop # marker_begin", "", true);
        InlineAsm *asmMarkerEnd = InlineAsm::get(FunctionType::get(VoidTy, false),
                                                 "nop # marker_end", "", true);

        // Process each basic block to add markers around unsafe instructions
        for (BasicBlock &BB : F) {
            Instruction *firstUnsafeInst = nullptr;
            Instruction *lastUnsafeInst = nullptr;

            // This loop will identify the first and last unsafe instructions in the block
            for (Instruction &I : BB) {
                if (I.getMetadata("unsafe_inst")) {
                    if (!firstUnsafeInst) {
                        firstUnsafeInst = &I;
                    }
                    lastUnsafeInst = &I;
                }
            }

            // insert markers, if unsafe instructions were found
            if (firstUnsafeInst && lastUnsafeInst) {
                // Inserts `marker_begin` before the first unsafe instruction
                IRBuilder<> Builder(firstUnsafeInst);
                Builder.CreateCall(asmMarkerBegin);
                modified = true;

                // Inserts `marker_end` after the last unsafe instruction, or at the end of the block
                if (Instruction *nextInst = lastUnsafeInst->getNextNode()) {
                    IRBuilder<> EndBuilder(nextInst);
                    EndBuilder.CreateCall(asmMarkerEnd);
                } else {
                    IRBuilder<> EndBuilder(&BB);
                    EndBuilder.SetInsertPoint(BB.getTerminator());
                    EndBuilder.CreateCall(asmMarkerEnd);
                }
            }
        }

        return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
    }
