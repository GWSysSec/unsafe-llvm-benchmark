//===-- DynamicLineCount.cpp - Source Line Counter Implementation --------===//
#include "llvm/Transforms/DynamicLineCount/DynamicLineCount.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/SmallSet.h"

using namespace llvm;

bool DynamicLineCountPass::isCompilerGenerated(const Instruction &I) const {
    if (isa<DbgInfoIntrinsic>(I)) return true;
    if (I.isDebugOrPseudoInst()) return true;
    if (!I.getDebugLoc()) return true;
    return false;
}

bool DynamicLineCountPass::skipLibraryFile(StringRef File) const {
    if (File.contains("/rustc/")) return true;
    if (File.contains("/library/")) return true;
    return false;
}

bool DynamicLineCountPass::isUnsafeInstruction(const Instruction &I) const {
    return (I.getMetadata("unsafe_inst") != nullptr);
}

void DynamicLineCountPass::processSourceLocation(const DILocation *Loc, bool IsUnsafe, Function &F) {
    if (!Loc) return;

    const DILocation *UserLoc = Loc;
    while (UserLoc) {
        StringRef Filename = UserLoc->getFilename();
        if (!skipLibraryFile(Filename)) {
            break;
        }
        UserLoc = UserLoc->getInlinedAt();
    }
    if (!UserLoc) return;

    unsigned Line = UserLoc->getLine();
    StringRef File = UserLoc->getFilename();
    SourceLocation SrcLoc{File.str(), Line, IsUnsafe};

    if (SeenLines.insert(SrcLoc).second) {
        IRBuilder<> Builder(&F.getEntryBlock().front());
        insertLineCounter(Builder, SrcLoc);
    }
}

void DynamicLineCountPass::insertLineCounter(IRBuilder<> &Builder, const SourceLocation &Loc) {
    Module *M = Builder.GetInsertBlock()->getModule();
    LLVMContext &Ctx = M->getContext();

    FunctionType *FTy = FunctionType::get(
        Type::getVoidTy(Ctx),
        {Type::getInt64Ty(Ctx), PointerType::get(Type::getInt8Ty(Ctx), 0)},
        false
    );

    StringRef FuncName = Loc.IsUnsafe
        ? "update_unsafe_line_counter"
        : "update_line_counter";

    auto Callee = M->getOrInsertFunction(FuncName, FTy).getCallee();
    auto *Fn = cast<Function>(Callee);

    Value *LineNo = ConstantInt::get(Type::getInt64Ty(Ctx), Loc.Line);
    Value *Filename = Builder.CreateGlobalStringPtr(Loc.File);
    Builder.CreateCall(Fn, {LineNo, Filename});
}

void DynamicLineCountPass::insertExecutionCounter(IRBuilder<> &Builder, const SourceLocation &Loc) {
    Module *M = Builder.GetInsertBlock()->getModule();
    LLVMContext &Ctx = M->getContext();

    FunctionType *FTy = FunctionType::get(
        Type::getVoidTy(Ctx),
        {Type::getInt64Ty(Ctx), PointerType::get(Type::getInt8Ty(Ctx), 0)},
        false
    );

    StringRef FuncName = Loc.IsUnsafe
        ? "mark_unsafe_line_executed"
        : "mark_line_executed";

    auto Callee = M->getOrInsertFunction(FuncName, FTy).getCallee();
    auto *Fn = cast<Function>(Callee);

    Value *LineNo = ConstantInt::get(Type::getInt64Ty(Ctx), Loc.Line);
    Value *Filename = Builder.CreateGlobalStringPtr(Loc.File);
    Builder.CreateCall(Fn, {LineNo, Filename});
}

void DynamicLineCountPass::instrumentBasicBlock(BasicBlock &BB, Function &F) {
    if (BB.isEHPad() || BB.empty())
        return;

    StringRef FName = F.getName();
    if (FName.startswith("_ZN3std") ||
        FName.startswith("_ZN4core") ||
        FName.startswith("_ZN5alloc") ||
        FName.startswith("_ZN9hashbrown") ||
        FName.startswith("_ZN7__test") ||
        FName.contains("::rt::") ||
        FName.contains(".rt.") ||
        FName.contains("::panic") ||
        FName.contains(".panic")) {
        return;
    }

    IRBuilder<> Builder(&BB.front());
    
    SmallSet<LineKey, 8> InstrumentedLines;
    SmallSet<LineKey, 8> UnsafeLines;

    for (Instruction &I : BB) {
        if (!isCompilerGenerated(I)) {
            if (const DILocation *Loc = I.getDebugLoc()) {
                std::string Filename = Loc->getFilename().str();
                unsigned Line = Loc->getLine();
                LineKey Key = std::make_pair(Filename, Line);
                
                if (isUnsafeInstruction(I)) {
                    UnsafeLines.insert(Key);
                }
            }
        }
    }

    for (Instruction &I : BB) {
        if (!isCompilerGenerated(I)) {
            if (const DILocation *Loc = I.getDebugLoc()) {
                std::string Filename = Loc->getFilename().str();
                unsigned Line = Loc->getLine();
                LineKey Key = std::make_pair(Filename, Line);
                
                if (!InstrumentedLines.contains(Key)) {
                    InstrumentedLines.insert(Key);
                    
                    bool IsUnsafe = UnsafeLines.contains(Key);
                    
                    processSourceLocation(Loc, IsUnsafe, F);
                    
                    SourceLocation ExLoc{
                        std::move(Filename),
                        Line,
                        IsUnsafe
                    };
                    insertExecutionCounter(Builder, ExLoc);
                }
            }
        }
    }
}

PreservedAnalyses DynamicLineCountPass::run(Function &F, FunctionAnalysisManager &AM) {
    if (F.isDeclaration()) {
        return PreservedAnalyses::all();
    }

    SeenLines.clear();

    for (BasicBlock &BB : F) {
        instrumentBasicBlock(BB, F);
    }

    return PreservedAnalyses::none();
}
