//===--------------FunctionCount.cpp----------------===//
// This pass will log the function behavior during runtime.

#include "llvm/Transforms/DynamicUnsafeCount/FunctionCount.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/Pass.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/IntrinsicInst.h"

//Function to check if the crate is a primary crate or whether it is an dependency
static bool isPrimaryPackage() {
    const char *p = std::getenv("CARGO_PRIMARY_PACKAGE");
    return p && std::strcmp(p, "1") == 0;
}


using namespace llvm;

PreservedAnalyses FunctionCount::run(Function &F,
                                           FunctionAnalysisManager &AM) {


                                            if (!isPrimaryPackage())
                                            return PreservedAnalyses::all();  
    Module *M = F.getParent();
    LLVMContext &Context = M->getContext();

    // Demangles function name for better readability - For debugging
    std::string DemangledName = llvm::demangle(F.getName().str());

    // Skips functions that are declarations (external) or intrinsic functions
    if (F.isDeclaration() || F.isIntrinsic()) {
        return PreservedAnalyses::all();
    }



    // Excludes functions from Rust standard library/runtime
    if (DemangledName.find("std::") == 0 || DemangledName.find("core::") == 0 || 
        DemangledName.find("alloc::") == 0 || DemangledName.find("rustc_") == 0) {
        return PreservedAnalyses::all();
    }


    // Prototype for runtime functions
    Type *VoidTy = Type::getVoidTy(Context);
    Type *Int8PtrTy = PointerType::get(Type::getInt8Ty(Context), 0);
    Type *Int1Ty = Type::getInt1Ty(Context);

    std::vector<Type *> Params = {Int8PtrTy, Int1Ty};
    FunctionType *RecordFuncType = FunctionType::get(VoidTy, Params, false);
    FunctionCallee RecordFuncExec = M->getOrInsertFunction("record_function_execution", RecordFuncType);

    FunctionCallee PrintStats = M->getOrInsertFunction(
        "print_execution_statistics",
        FunctionType::get(VoidTy, {}, false)
    );

    // Checks if any instruction in the function is marked as "unsafe"
    bool IsUnsafeFunction = false;
    bool insideMarkers = false;

    for (auto &BB : F) {
        for (auto &I : BB) {

            if (auto *asmInst = dyn_cast<CallInst>(&I)) {
                if (asmInst->isInlineAsm()) {
                    InlineAsm *inlineAsm = dyn_cast<InlineAsm>(asmInst->getCalledOperand());
                    std::string asmString = inlineAsm->getAsmString();

                    // Checks for markers to target the unsafe instructions
                    if (asmString.find("nop # marker_begin") != std::string::npos) {
                        insideMarkers = true;
                    } 
                }
            }


            if ((insideMarkers == true) &&  (I.getMetadata("unsafe_inst")) && (I.getOpcode() != Instruction::Ret)) {
                if (auto *intrinsicInst = dyn_cast<IntrinsicInst>(&I)) {
                    if (intrinsicInst->getIntrinsicID() == Intrinsic::lifetime_start ||
                        intrinsicInst->getIntrinsicID() == Intrinsic::lifetime_end) {
                        continue;
                    }
                }
                IsUnsafeFunction = true;
                break; // No need to check further; marks the function as unsafe
            }
        }
        if (IsUnsafeFunction) break; // Exits the loop once unsafe instruction is found
    }


    if (!F.isDeclaration()) {
        // Find the last basic block in the function
        BasicBlock &LastBB = F.back();
        Instruction *Terminator = LastBB.getTerminator(); // Get the terminator (likely a return)
    
        IRBuilder<> Builder(Terminator); // Inserts before the return instruction
        Value *FuncName = Builder.CreateGlobalStringPtr(F.getName());
        Value *IsUnsafe = Builder.getInt1(IsUnsafeFunction);
        Builder.CreateCall(RecordFuncExec, {FuncName, IsUnsafe});
    }

    // Inserted printing function call at the end of `main`
    if (F.getName() == "main") {
        for (auto &BB : F) {
            if (isa<ReturnInst>(BB.getTerminator())) {
                IRBuilder<> Builder(BB.getTerminator());
                Builder.CreateCall(PrintStats, {});
            }
        }
    }

    return PreservedAnalyses::all();
}
