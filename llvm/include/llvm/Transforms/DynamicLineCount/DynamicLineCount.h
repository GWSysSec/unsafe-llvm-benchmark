#ifndef LLVM_TRANSFORMS_DYNAMICLINECOUNT_DYNAMICLINECOUNT_H
#define LLVM_TRANSFORMS_DYNAMICLINECOUNT_DYNAMICLINECOUNT_H

#include "llvm/IR/PassManager.h"
#include "llvm/IR/IRBuilder.h"
#include <set>
#include <string>
#include <utility>

namespace llvm {

class DynamicLineCountPass : public PassInfoMixin<DynamicLineCountPass> {
private:
    struct SourceLocation {
        std::string File;
        unsigned Line;
        bool IsUnsafe;

        bool operator<(const SourceLocation &Other) const {
            if (File != Other.File) return File < Other.File;
            if (Line != Other.Line) return Line < Other.Line;
            return IsUnsafe < Other.IsUnsafe;
        }

        bool operator==(const SourceLocation &Other) const {
            return File == Other.File && Line == Other.Line && IsUnsafe == Other.IsUnsafe;
        }
    };

    using LineKey = std::pair<std::string, unsigned>;

    std::set<SourceLocation> SeenLines;

    bool isCompilerGenerated(const llvm::Instruction &I) const;
    bool skipLibraryFile(llvm::StringRef File) const;
    bool isUnsafeInstruction(const llvm::Instruction &I) const;

    void processSourceLocation(const llvm::DILocation *Loc, bool IsUnsafe, llvm::Function &F);
    void insertLineCounter(llvm::IRBuilder<> &Builder, const SourceLocation &Loc);
    void insertExecutionCounter(llvm::IRBuilder<> &Builder, const SourceLocation &Loc);
    void instrumentBasicBlock(llvm::BasicBlock &BB, llvm::Function &F);

public:
    llvm::PreservedAnalyses run(llvm::Function &F, llvm::FunctionAnalysisManager &AM);
    static bool isRequired() { return true; }
};

} // end namespace llvm

#endif // LLVM_TRANSFORMS_DYNAMICLINECOUNT_DYNAMICLINECOUNT_H