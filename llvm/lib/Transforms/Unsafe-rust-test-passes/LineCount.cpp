#include "llvm/Transforms/Unsafe-rust-test-passes/LineCount.h"

using namespace llvm;

PreservedAnalyses LineCount::run(Module &M,
                                      ModuleAnalysisManager &AM) {

    unsigned int total_line_count = 0;
    unsigned int total_unsafe_line_count = 0;
    llvm::StringRef unsafe_string = llvm::StringRef("unsafe_inst"); //Official string ref for unsafe instructions

    //Types of instructions
    unsigned int unsafe_load_instructions = 0;
    unsigned int unsafe_store_instructions = 0;
    unsigned int unsafe_add_instructions = 0;
    unsigned int unsafe_get_element_ptr_instructions = 0;
    unsigned int unsafe_sub_instructions = 0;
    unsigned int unsafe_alloca_instructions = 0;
    unsigned int unsafe_ptrtoint_instructions = 0;
    unsigned int unsafe_inttoptr_instructions = 0;
    unsigned int unsafe_bitcast_instructions = 0;


    std::error_code e;
    std::string json_filename = M.getName().str();
    json_filename.append(".json");
    llvm::StringRef json_filename_stringref = llvm::StringRef(json_filename);
    llvm::raw_fd_ostream OS = raw_fd_ostream(json_filename_stringref, e);

    if (e.value() != 0) {
        llvm::errs() << "Bad FD\n";
    }

    json::OStream J(OS, 4u);

    J.flush();

    
    for (Function &F : M) {

        unsigned int function_line_count = 0;
        unsigned int unsafe_function_line_count = 0;

        for (BasicBlock &BB : F) {
            
            total_line_count += BB.size();
            function_line_count += BB.size();

            for (Instruction &I : BB) {
                
                MDNode* is_unsafe = I.getMetadata(unsafe_string);

                if (is_unsafe != NULL) {
                    ++total_unsafe_line_count;
                    ++unsafe_function_line_count;

                    switch (I.getOpcode()) {
                        case Instruction::Add:
                            ++unsafe_add_instructions;
                        case Instruction::Load:
                            ++unsafe_load_instructions;
                        case Instruction::Store:
                            ++unsafe_store_instructions;
                        case Instruction::GetElementPtr:
                            ++unsafe_get_element_ptr_instructions;
                        case Instruction::Sub:
                            ++unsafe_sub_instructions;
                        case Instruction::Alloca:
                            ++unsafe_alloca_instructions;
                        case Instruction::PtrToInt:
                            ++unsafe_ptrtoint_instructions;
                        case Instruction::IntToPtr:
                            ++unsafe_inttoptr_instructions;
                        case Instruction::BitCast:
                            ++unsafe_bitcast_instructions;
                        default:
                            break;
                    }

                    //Instruction for loop
                }

            }

        }

        //llvm::errs() << "# Of instructions in function " << F.getName() << ": " << function_line_count << "\n"
        //<< "# Of unsafe instructions in function " << F.getName() << ": " << unsafe_function_line_count << "\n";

    }

    //llvm::errs() << "# Of instructions in " << M.getName() << ": " << total_line_count << "\n"
    //<< "# Of unsafe instructions in " << M.getName() << ": " << total_unsafe_line_count << "\n";

    //llvm::errs() << "# Of unsafe add instructions in " << M.getName() << ": " << unsafe_add_instructions << "\n"
    //<< "# Of unsafe load instructions in " << M.getName() << ": " << unsafe_load_instructions << "\n" 
    //<< "# Of unsafe store instructions in " << M.getName() << ": " << unsafe_store_instructions << "\n"
    //<< "# Of unsafe pointer calculation instructions in " << M.getName() << ": " << unsafe_get_element_ptr_instructions << "\n";
    
    float percent_unsafe = 0;

    if (total_unsafe_line_count != 0) {
        percent_unsafe = (float) ( (float) total_unsafe_line_count / (float) total_line_count) * 100.0;
    }

    J.object([&] {
        J.attribute("module_name", M.getName()); //Module name
        J.attribute("total_instruction_count", total_line_count); //Total instruction count
        J.attribute("total_unsafe_instruction_count", total_unsafe_line_count); //Total unsafe instruction count
        J.attribute("total_unsafe_add_instruction_count", unsafe_add_instructions); //Total unsafe add instruction count
        J.attribute("total_unsafe_load_instruction_count", unsafe_load_instructions); //Total unsafe load instruction count
        J.attribute("total_unsafe_store_instruction_count", unsafe_store_instructions); //Total unsafe store instruction count
        J.attribute("total_unsafe_get_element_ptr_instruction_count", unsafe_get_element_ptr_instructions); //Total unsafe pointer arithmetic instruction count
        J.attribute("total_unsafe_sub_instruction_count", unsafe_sub_instructions); //Total unsafe sub instruction count
        J.attribute("total_unsafe_alloca_instruction_count", unsafe_alloca_instructions); //Total unsafe alloca instruction count
        J.attribute("total_unsafe_ptrtoint_instruction_count", unsafe_ptrtoint_instructions); //Total unsafe ptrtoint instruction count
        J.attribute("total_unsafe_inttoptr_instruction_count", unsafe_inttoptr_instructions); //Total unsafe inttoptr instruction count
        J.attribute("total_unsafe_bitcast_instruction_count", unsafe_bitcast_instructions); //Total unsafe bitcast instruction count
        J.attribute("percent_unsafe", percent_unsafe); //Percentage unsafe code
    });

    J.~OStream();
    OS.close();

  return PreservedAnalyses::all();
}