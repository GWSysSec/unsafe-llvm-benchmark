//===--------------InlineCountUnsafe.cpp----------------===//
// This pass will count the unsafe instructions executed during runtime.

#include "llvm/Transforms/DynamicUnsafeCount/InlineCountUnsafe.h"
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
            "update_counter", FunctionType::get(Type::getVoidTy(Context), {Type::getInt64Ty(Context), Type::getInt64Ty(Context), Type::getInt64Ty(Context), Type::getInt64Ty(Context), 
                Type::getInt64Ty(Context), Type::getInt64Ty(Context), Type::getInt64Ty(Context), Type::getInt64Ty(Context), Type::getInt64Ty(Context), Type::getInt64Ty(Context), Type::getInt64Ty(Context), Type::getInt64Ty(Context)}, false));
        FunctionCallee printCounterFunc = M->getOrInsertFunction(
                "print_inline_count", FunctionType::get(Type::getVoidTy(Context), false));


        long blockInstructionCount = 0;

        for (BasicBlock &BB : F) {
            long total_inst_count = 0;
            bool insideMarkers = false;
            bool flagOn = false;
            blockInstructionCount = 0;

            //Types of instruction
            long unsafe_load_count = 0;
            long unsafe_store_count = 0;
            long unsafe_add_count = 0;
            long unsafe_getelementptr_count = 0;
            long unsafe_sub_count = 0;
            long unsafe_alloca_count = 0;
            long unsafe_ptrtoint_instructions = 0;
            long unsafe_inttoptr_instructions = 0;
            long unsafe_bitcast_instructions = 0;
            long other = 0;

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

                total_inst_count++;

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

                    // Categorize the instruction type
                    switch (I.getOpcode()) {
                        case Instruction::Load:
                            ++unsafe_load_count;
                            break;
                        case Instruction::Store:
                            ++unsafe_store_count;
                            break;
                        case Instruction::Add:
                            ++unsafe_add_count;
                            break;
                        case Instruction::GetElementPtr:
                            ++unsafe_getelementptr_count;
                            break;
                        case Instruction::Sub:
                            ++unsafe_sub_count;
                            break;
                        case Instruction::Alloca:
                            ++unsafe_alloca_count;
                            break;
                        case Instruction::PtrToInt:
                            ++unsafe_ptrtoint_instructions;
                            break;
                        case Instruction::IntToPtr:
                            ++unsafe_inttoptr_instructions;
                            break;
                        case Instruction::BitCast:
                            ++unsafe_bitcast_instructions;
                            break;
                        default:
                            ++other;
                            break;
                    }

                }

                if(insideMarkers){
                    flagOn = true;
                }
                else{
                    flagOn =false;
                }
            }

            // update the counter, if we see the unsafe instruction
            if (total_inst_count > 0) {
                IRBuilder<> Builder(BB.getTerminator());
                Builder.CreateCall(updateCounterFunc, {
                    ConstantInt::get(Type::getInt64Ty(Context), total_inst_count),
                    ConstantInt::get(Type::getInt64Ty(Context), blockInstructionCount),
                    ConstantInt::get(Type::getInt64Ty(Context), unsafe_load_count),
                    ConstantInt::get(Type::getInt64Ty(Context), unsafe_store_count),
                    ConstantInt::get(Type::getInt64Ty(Context), unsafe_add_count),
                    ConstantInt::get(Type::getInt64Ty(Context), unsafe_getelementptr_count),
                    ConstantInt::get(Type::getInt64Ty(Context), unsafe_sub_count),
                    ConstantInt::get(Type::getInt64Ty(Context), unsafe_alloca_count),
                    ConstantInt::get(Type::getInt64Ty(Context), unsafe_ptrtoint_instructions),
                    ConstantInt::get(Type::getInt64Ty(Context), unsafe_inttoptr_instructions),
                    ConstantInt::get(Type::getInt64Ty(Context), unsafe_bitcast_instructions),
                    ConstantInt::get(Type::getInt64Ty(Context), other)
                });
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