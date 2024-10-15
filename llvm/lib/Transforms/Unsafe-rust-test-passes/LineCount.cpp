#include "llvm/Transforms/Unsafe-rust-test-passes/LineCount.h"

using namespace llvm;

PreservedAnalyses LineCount::run(Module &M,
                                      ModuleAnalysisManager &AM) {

    unsigned int total_line_count = 0;
    unsigned int unsafe_line_count = 0;
    llvm::StringRef unsafe_string = llvm::StringRef("unsafe_inst"); //Official string ref for unsafe instructions

    //Types of instructions
    unsigned int unsafe_load_instructions = 0;
    unsigned int unsafe_store_instructions = 0;
    unsigned int unsafe_add_instructions = 0;
    unsigned int unsafe_get_element_ptr_instructions = 0;

    //llvm::SmallVectorImpl<llvm::StringRef> meta_list = new llvm::SmallVectorImpl(10);

    //JSON Boilerplate
    //std::string buf = "";
    //llvm::raw_string_ostream OS = llvm::raw_string_ostream(buf);
    //json::OStream J(OS, 4u);

    //J.value(M.getName().str());
    
    for (Function &F : M) {

        unsigned int function_line_count = 0;
        unsigned int unsafe_function_line_count = 0;

        for (BasicBlock &BB : F) {
            
            total_line_count += BB.size();
            function_line_count += BB.size();

            for (Instruction &I : BB) {
                
                MDNode* is_unsafe = I.getMetadata(unsafe_string);

                if (is_unsafe != NULL) {
                    ++unsafe_line_count;
                    ++unsafe_function_line_count;

                    switch (I.getOpcode()) {
                        case Instruction::Add:
                            ++unsafe_add_instructions;
                            break;
                        case Instruction::Load:
                            ++unsafe_load_instructions;
                            break;
                        case Instruction::Store:
                            ++unsafe_store_instructions;
                        case Instruction::GetElementPtr:
                            ++unsafe_get_element_ptr_instructions;
                            break;
                        default:
                            break;
                    }

                    //Instruction for
                }

            }

        }

    }

    llvm::errs() << "# Of instructions in " << M.getName() << ": " << total_line_count << "\n"
    << "# Of unsafe instructions in " << M.getName() << ": " << unsafe_line_count << "\n";

    llvm::errs() << "# Of unsafe add instructions in " << M.getName() << ": " << unsafe_add_instructions << "\n"
    << "# Of unsafe load instructions in " << M.getName() << ": " << unsafe_load_instructions << "\n" 
    << "# Of unsafe store instructions in " << M.getName() << ": " << unsafe_store_instructions << "\n"
    << "# Of unsafe pointer calculation instructions in " << M.getName() << ": " << unsafe_get_element_ptr_instructions << "\n";

  return PreservedAnalyses::all();
}