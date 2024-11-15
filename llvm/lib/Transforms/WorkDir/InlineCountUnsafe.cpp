//===--------------InlineCountUnsafe.cpp----------------===//
// This pass will count the unsafe instructions executed during runtime.

#include "llvm/Transforms/WorkDir/InlineCountUnsafe.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/Pass.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/IntrinsicInst.h"

using namespace llvm;

PreservedAnalyses InlineCountUnsafe::run(Function &F,
                                           FunctionAnalysisManager &AM) {
        bool modified = false;
        Module *M = F.getParent();
        LLVMContext &Context = M->getContext();

        // Prototype for updating and printing instruction counts
        FunctionCallee updateCounterFunc = M->getOrInsertFunction(
            "update_counter", FunctionType::get(Type::getVoidTy(Context), {Type::getInt64Ty(Context)}, false));
        FunctionCallee printCounterFunc = M->getOrInsertFunction(
            "print_inline_count", FunctionType::get(Type::getVoidTy(Context), false));

        bool insideMarkers = false;
        bool flagOn = false;
        long blockInstructionCount = 0;

        for (BasicBlock &BB : F) {
            blockInstructionCount = 0;
            for (Instruction &I : BB) {
                if (auto *asmInst = dyn_cast<CallInst>(&I)) {
                    if (asmInst->isInlineAsm()) {
                        InlineAsm *inlineAsm = dyn_cast<InlineAsm>(asmInst->getCalledOperand());
                        std::string asmString = inlineAsm->getAsmString();

                        // Checking for markers to target the unsafe instructions
                        if (asmString.find("nop # marker_begin") != std::string::npos) {
                            insideMarkers = true;
                        } else if (asmString.find("nop # marker_end") != std::string::npos) {
                            insideMarkers = false;
                            flagOn = false;
                        }
                    }
                }

                // This flag will help to count instructions only within the markers
                if (flagOn) {
                //This will exclude "life start" and "life end" instructions within the marker block
                if (auto *intrinsicInst = dyn_cast<IntrinsicInst>(&I)) {
                    if (intrinsicInst->getIntrinsicID() == Intrinsic::lifetime_start ||
                        intrinsicInst->getIntrinsicID() == Intrinsic::lifetime_end) {
                        continue;
                    }
                }

                blockInstructionCount++;

                }

                if(insideMarkers){
                    flagOn = true;
                }
                else{
                    flagOn =false;
                }
            }

            // update the counter, if we see the unsafe instruction
            if (blockInstructionCount > 0) {
                IRBuilder<> Builder(BB.getTerminator());
                Builder.CreateCall(updateCounterFunc, ConstantInt::get(Type::getInt64Ty(Context), blockInstructionCount));
                modified = true;
            }
        }

        // Inserted printing function call at the end of `main`
        if (F.getName() == "main") {
            IRBuilder<> Builder(F.back().getTerminator());
            Builder.CreateCall(printCounterFunc);
            modified = true;
        }

        return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}