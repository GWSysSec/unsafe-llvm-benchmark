#ifndef LLVM_TRANSFORMS_DYNAMICLINECOUNT_DYNAMICLINECOUNT_H
#define LLVM_TRANSFORMS_DYNAMICLINECOUNT_DYNAMICLINECOUNT_H

#include "llvm/IR/PassManager.h"
#include "llvm/IR/Function.h"
#include <string>
#include <set>
#include <fstream>

namespace llvm {

// source location structure
struct SourceLocation {
    std::string File;
    unsigned Line;
    bool IsUnsafe;

    SourceLocation(std::string F, unsigned L, bool U) 
        : File(std::move(F)), Line(L), IsUnsafe(U) {}
    
    bool operator<(const SourceLocation &Other) const {
        if (File != Other.File)
            return File < Other.File;
        return Line < Other.Line;
    }
};

struct DynamicLineCountPass : PassInfoMixin<DynamicLineCountPass> {
    PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
    static bool isRequired() { return true; }
};

// external function to export unsafe lines
void exportUnsafeLines(const std::string& Filename);

} // namespace llvm

#endif
