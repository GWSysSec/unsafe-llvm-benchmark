#include "llvm/Transforms/Unsafe-rust-test-passes/LineCount.h"

using namespace llvm;

PreservedAnalyses LineCount::run(Function &F,
                                      FunctionAnalysisManager &AM) {
    LLVM_DEBUG(dbgs() << "LINECOUNT: running on function " << F.getName()
                    << "\n");

    unsigned int total_line_count = 0;
    unsigned int unsafe_line_count = 0;
    llvm::StringRef unsafe_string = llvm::StringRef("unsafe_inst"); //Official string ref for unsafe instructions
    //llvm::SmallVectorImpl<llvm::StringRef> meta_list = new llvm::SmallVectorImpl(10);
    
    for (BasicBlock &BB : F) {
        
        total_line_count += BB.size();

        for (Instruction &I : BB) {
            
            MDNode* is_unsafe = I.getMetadata(unsafe_string);

            if (is_unsafe != NULL) {
                ++unsafe_line_count;
            }

        }

    }

    llvm:errs() << "# Of instructions in " << F.getName() << ": " << total_line_count << "\n"
    << "# Of unsafe instructions in " << F.getName() << ": " << unsafe_line_count << "\n";

  return PreservedAnalyses::all();
}