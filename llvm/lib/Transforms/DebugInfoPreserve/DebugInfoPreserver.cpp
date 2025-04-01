//===-- DebugInfoPreserver.cpp - Preserve Debug Metadata ------------------===//
#include "llvm/Transforms/DebugInfoPreserve/DebugInfoPreserver.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "llvm/Support/Debug.h"

using namespace llvm;

namespace {
bool verifyPHINodes(BasicBlock &BB) {
    bool Modified = false;
    BasicBlock::iterator I = BB.begin();
    BasicBlock::iterator InsertPt = BB.begin();
    
    while (I != BB.end() && isa<PHINode>(I)) {
        PHINode *PN = cast<PHINode>(I++);
        if (PN->getIterator() != InsertPt) {
            PN->moveBefore(&*InsertPt);
            Modified = true;
        }
        ++InsertPt;
    }
    return Modified;
}

bool isValidDebugLocation(const DILocation *Loc) {
    if (!Loc) return false;
    return (Loc->getScope() && Loc->getFile() && 
            Loc->getLine() > 0 && Loc->getColumn() > 0);
}

} // namespace

PreservedAnalyses DebugInfoPreserverPass::run(Module &M, ModuleAnalysisManager &AM) {
    LLVMContext &Ctx = M.getContext();
    bool Modified = false;

    // step a: reorder PHIs
    for (Function &F : M) {
        if (F.isDeclaration()) continue;
        for (BasicBlock &BB : F) Modified |= verifyPHINodes(BB);
    }

    // step b: create anchor global (no exceptions)
    GlobalVariable *GV = new GlobalVariable(
        M, 
        Type::getInt8Ty(Ctx), 
        false,
        GlobalValue::InternalLinkage, 
        ConstantInt::get(Type::getInt8Ty(Ctx), 0),
        "__unsafe_coverage_anchor"
    );

    if (!GV) return PreservedAnalyses::none();

    // step c: collect valid DILocations
    SmallVector<Metadata *, 256> MetadataRefs;
    for (Function &F : M) {
        for (BasicBlock &BB : F) {
            for (Instruction &I : BB) {
                if (const DILocation *Loc = I.getDebugLoc()) {
                    if (isValidDebugLocation(Loc)) {
                        // wrap in ArrayRef via initializer list
                        MetadataRefs.push_back(MDNode::get(Ctx, {Loc}));
                    }
                }
            }
        }
    }

    // step d: attach metadata
    if (!MetadataRefs.empty()) {
        MDNode *DebugMD = MDNode::get(Ctx, MetadataRefs);
        if (DebugMD) GV->addMetadata("preserved.debuginfo", *DebugMD);
    }

    // step e: keep anchor global
    appendToCompilerUsed(M, {GV});

    return Modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
