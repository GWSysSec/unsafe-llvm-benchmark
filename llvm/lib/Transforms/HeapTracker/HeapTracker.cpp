#include "llvm/Transforms/HeapTracker/HeapTracker.h"
#include "llvm/Transforms/InstMarker/InstMarker.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"

static const char *DYN_MEM_ACCESS_FN = "dyn_mem_access";
static const char *DYN_UNSAFE_MEM_ACCESS_FN = "dyn_unsafe_mem_access";

using namespace llvm;

PreservedAnalyses HeapTrackerPass::run(Function &F, FunctionAnalysisManager &AM) {
    LLVMContext &C = F.getContext();
    Module *M = F.getParent();

    // Define function prototypes for the runtime library functions.
    Type *voidTy = Type::getVoidTy(C);
    Type *rawPtrTy = PointerType::getUnqual(Type::getInt8Ty(C)); // i8*
    Type *booleanTy = Type::getInt1Ty(C);

    // Signature for dyn_mem_access(i8*)
    FunctionType *dynMemAccessFnTy = FunctionType::get(voidTy, rawPtrTy, false);
    FunctionCallee dynMemAccessFn = M->getOrInsertFunction(DYN_MEM_ACCESS_FN, dynMemAccessFnTy);

    // Signature for dyn_unsafe_mem_access(i8*, i1)
    FunctionType *dynUnsafeMemAccessFnTy = FunctionType::get(voidTy, {rawPtrTy, booleanTy}, false);
    FunctionCallee dynUnsafeMemAccessFn = M->getOrInsertFunction(DYN_UNSAFE_MEM_ACCESS_FN, dynUnsafeMemAccessFnTy);

    // This flag's scope is now the entire function, so it persists across basic blocks.
    bool unsafeBlockStarted = false;
    bool modified = false;

    // Iterate over each basic block in the function.
    for (BasicBlock &BB : F) {
        // We must use an iterator because we are modifying the block's instruction list.
        for (auto I_it = BB.begin(), E = BB.end(); I_it != E; ++I_it) {
            Instruction &I = *I_it;

            // First, check for memory instructions (Load or Store).
            if (isa<LoadInst>(I) || isa<StoreInst>(I)) {
                IRBuilder<> Builder(&I);
                bool isLoad = isa<LoadInst>(I);

                // Get the pointer operand from the memory instruction.
                Value *memAddr = isLoad ? cast<LoadInst>(&I)->getPointerOperand()
                                        : cast<StoreInst>(&I)->getPointerOperand();

                // FIX: Explicitly cast the pointer to i8* to match the function signature.
                // This prevents LLVM type assertion errors.
                Value *castedAddr = Builder.CreateBitCast(memAddr, rawPtrTy);

                if (unsafeBlockStarted) {
                    // We are in an unsafe block, call the unsafe version.
                    Value *isLoadVal = ConstantInt::get(booleanTy, isLoad);
                    Builder.CreateCall(dynUnsafeMemAccessFn, {castedAddr, isLoadVal});
                } else {
                    // We are in a safe block, call the safe version.
                    Builder.CreateCall(dynMemAccessFn, castedAddr);
                }
                modified = true;
            }

            // After checking for memory instructions, update the state if we see a marker.
            if (CallInst *CI = dyn_cast<CallInst>(&I)) {
                if (InlineAsm *IA = dyn_cast<InlineAsm>(CI->getCalledOperand())) {
                    StringRef AsmStr = IA->getAsmString();
                    if (AsmStr == UNSAFE_MARKER_BEGIN) {
                        if (!unsafeBlockStarted) {
                            unsafeBlockStarted = true;
                        }
                    } else if (AsmStr == UNSAFE_MARKER_END) {
                        if (unsafeBlockStarted) {
                            unsafeBlockStarted = false;
                        }
                    }
                }
            }
        }
    }

    return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
