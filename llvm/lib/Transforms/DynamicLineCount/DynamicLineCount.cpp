#include "llvm/Transforms/DynamicLineCount/DynamicLineCount.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_ostream.h"

#include <set>
#include <string>
#include <algorithm>

using namespace llvm;

/// utility to sanitize filenames for global variable names
static std::string sanitizeFileName(const std::string &File) {
    std::string Name = File;
    std::replace(Name.begin(), Name.end(), '/', '_');
    std::replace(Name.begin(), Name.end(), '\\', '_');
    std::replace(Name.begin(), Name.end(), '.', '_');
    return Name;
}

/// decide if a file is "in-project" or library/extern
static bool isProjectFile(StringRef File) {
    // 1) skip obviously external or toolchain-related files
    if (File.contains("/rustc/"))
        return false;
    if (File.contains("/.cargo/"))
        return false;

    // 2) skip locally cloned standard library code (e.g., library/core/, library/std/, etc.)
    if (File.contains("/library/"))
        return false;

    // If there is a canonical project root path, further filter:
    //   StringRef ProjectRoot = "/home/oscar/Projects/unsafebench/"; 
    //   if (!File.startswith(ProjectRoot))
    //       return false;
    
    // otherwise, if none of the skip criteria matched, consider it a project file
    return true;
}

PreservedAnalyses DynamicLineCountPass::run(Function &F, FunctionAnalysisManager &AM) {
    if (F.isDeclaration())
        return PreservedAnalyses::all();

    Module *M = F.getParent();
    LLVMContext &Ctx = M->getContext();

    // prepare the function prototypes for our runtime hooks: (int64, i8*) -> void
    Type *VoidTy = Type::getVoidTy(Ctx);
    Type *Int64Ty = Type::getInt64Ty(Ctx);
    Type *Int8Ty  = Type::getInt8Ty(Ctx);
    Type *Int8PtrTy = PointerType::getUnqual(Int8Ty);

    FunctionType *FnTy = FunctionType::get(VoidTy, {Int64Ty, Int8PtrTy}, false);
    FunctionCallee UpdateUnsafe =
        M->getOrInsertFunction("update_unsafe_line_counter", FnTy);
    FunctionCallee MarkUnsafe =
        M->getOrInsertFunction("mark_unsafe_line_executed", FnTy);

    // we only instrument each (File,Line) pair once per function
    std::set<std::pair<std::string, unsigned>> InstrumentedLines;

    bool Modified = false;

    // loop over all instructions that have our "unsafe_inst" metadata
    for (Instruction &I : instructions(F)) {
        if (!I.getMetadata("unsafe_inst"))
            continue;

        DebugLoc DL = I.getDebugLoc();
        if (!DL)
            continue; // has no debug info

        const DILocation *Loc = DL.get();
        StringRef File = Loc->getFilename();
        if (File.empty())
            continue;

        // now apply our filter: skip library / rustc / cargo code
        if (!isProjectFile(File))
            continue;  // skip instrumentation for non-project files

        // for project code, record the line
        unsigned Line = Loc->getLine();
        auto Key = std::make_pair(File.str(), Line);

        // only instrument once per (File,Line) in this Function
        if (!InstrumentedLines.insert(Key).second) {
            // already did it in this function
            continue;
        }

        // pick a safe insertion point
        IRBuilder<> Builder(Ctx);
        if (isa<PHINode>(I) || isa<LandingPadInst>(I)) {
            BasicBlock *BB = I.getParent();
            Instruction *InsertPoint = nullptr;
            for (Instruction &Inst : *BB) {
                if (!isa<PHINode>(Inst) && !isa<LandingPadInst>(Inst)) {
                    InsertPoint = &Inst;
                    break;
                }
            }
            if (!InsertPoint)
                continue; // can't find suitable insertion point
            Builder.SetInsertPoint(InsertPoint);
        } else {
            // insert right before 'I'
            Builder.SetInsertPoint(&I);
        }

        // create or reuse a global string for 'File'
        std::string GlobalName = "unsafe_str_" + sanitizeFileName(File.str());
        GlobalVariable *GV = M->getNamedGlobal(GlobalName);
        if (!GV) {
            auto *FileConstant = ConstantDataArray::getString(Ctx, File.str(), true);
            GV = new GlobalVariable(
                *M, FileConstant->getType(), /*isConstant=*/true,
                GlobalValue::InternalLinkage, FileConstant, GlobalName
            );
            GV->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
        }
        Value *FileArg = Builder.CreateBitCast(GV, Int8PtrTy);

        // insert calls to update_unsafe_line_counter and mark_unsafe_line_executed
        Builder.CreateCall(UpdateUnsafe, {
            ConstantInt::get(Int64Ty, Line),
            FileArg
        });
        Builder.CreateCall(MarkUnsafe, {
            ConstantInt::get(Int64Ty, Line),
            FileArg
        });

        Modified = true;
    }

    return Modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
