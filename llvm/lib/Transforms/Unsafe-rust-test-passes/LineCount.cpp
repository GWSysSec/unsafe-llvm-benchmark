#include "llvm/Transforms/Unsafe-rust-test-passes/LineCount.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Constants.h"
#include <string>

using namespace llvm;

PreservedAnalyses LineCount::run(Module &M,
                                      ModuleAnalysisManager &AM) {

    unsigned int total_line_count = 0;
    unsigned int total_unsafe_line_count = 0;
    llvm::StringRef unsafe_string = llvm::StringRef("unsafe_inst"); //Official string ref for unsafe instructions
    static const char *UNSAFE_MARKER_BEGIN = "nop # marker_begin"; //new unsafe inst marker start
    static const char *UNSAFE_MARKER_END = "nop # marker_end"; //new unsafe inst marker end
    bool unsafe_block_started = false; //When true, all instructions are unsafe as they're inside the marker bounds

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
    unsigned int functions_with_unsafe_instructions = 0;

    std::error_code e;
    std::string json_filename = M.getName().str();
    //json_filename.append("_");
    //json_filename.append(std::to_string(rand()));
    //json_filename.append("_");
    json_filename.append(".json");
    llvm::StringRef json_filename_stringref = llvm::StringRef(json_filename);
    llvm::raw_fd_ostream OS = raw_fd_ostream(json_filename_stringref, e);

    if (e.value() != 0) {
        llvm::errs() << "Bad FD\n";
    }

    json::OStream J(OS, 4u);

    J.flush();

    //llvm::errs() << "LineCount Initialised\n";
    for (Function &F : M) {

        unsigned int function_line_count = 0;
        unsigned int unsafe_function_line_count = 0;

        for (BasicBlock &BB : F) {
            
            total_line_count += BB.size();
            function_line_count += BB.size();

            for (Instruction &I : BB) {
                
                //MDNode* is_unsafe = I.getMetadata(unsafe_string);
                if (llvm::CallInst *CI = llvm::dyn_cast<llvm::CallInst>(&I)) { //Checking for INST marker flag - derived from HeapTracker.cpp
                    //llvm::errs() << "CallInst passed\n";
                    if (llvm::InlineAsm *IA = llvm::dyn_cast<llvm::InlineAsm>(CI->getCalledOperand())) {
                        //llvm::errs() << "IA passed\n";
                        llvm::StringRef AsmStr = IA->getAsmString();
                        llvm::errs() << "ASM String: " << AsmStr << "\n";
                        if (AsmStr == UNSAFE_MARKER_BEGIN) {
                            //llvm::errs() << "unsafe block started\n";
                            unsafe_block_started = true;
                        } else if (AsmStr == UNSAFE_MARKER_END) {
                            //llvm::errs() << "unsafe block ended\n";
                            unsafe_block_started = false;
                        }
                    }
                }

                if (unsafe_block_started != false) {
                    ++total_unsafe_line_count;
                    ++unsafe_function_line_count;
                    //llvm::errs() << "total_unsafe_line_count: " << total_unsafe_line_count << "\n";
                    //llvm::errs() << "unsafe_function_line_count: " << unsafe_function_line_count << "\n";
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

        if (unsafe_function_line_count > 0) { //If the function has unsafe IR instructions, count it
            functions_with_unsafe_instructions++;
        }

    }
    //llvm::errs() << "LineCount loop finished\n";
    //llvm::errs() << "# Of instructions in " << M.getName() << ": " << total_line_count << "\n"
    //<< "# Of unsafe instructions in " << M.getName() << ": " << total_unsafe_line_count << "\n";
    
    float percent_unsafe = 0;

    if (total_unsafe_line_count != 0) {
        percent_unsafe = (float) ( (float) total_unsafe_line_count / (float) total_line_count) * 100.0;
    }

    J.object([&] {
        J.attribute("module_name", M.getName()); //Module name
        J.attribute("total_instruction_count", total_line_count); //Total instruction count
        J.attribute("total_unsafe_instruction_count", total_unsafe_line_count); //Total unsafe instruction count
        J.attribute("total_unsafe_add", unsafe_add_instructions); //Total unsafe add instruction count
        J.attribute("total_unsafe_load", unsafe_load_instructions); //Total unsafe load instruction count
        J.attribute("total_unsafe_store", unsafe_store_instructions); //Total unsafe store instruction count
        J.attribute("total_unsafe_get_element_ptr", unsafe_get_element_ptr_instructions); //Total unsafe pointer arithmetic instruction count
        J.attribute("total_unsafe_sub", unsafe_sub_instructions); //Total unsafe sub instruction count
        J.attribute("total_unsafe_alloca", unsafe_alloca_instructions); //Total unsafe alloca instruction count
        J.attribute("total_unsafe_ptrtoint", unsafe_ptrtoint_instructions); //Total unsafe ptrtoint instruction count
        J.attribute("total_unsafe_inttoptr", unsafe_inttoptr_instructions); //Total unsafe inttoptr instruction count
        J.attribute("total_unsafe_bitcast", unsafe_bitcast_instructions); //Total unsafe bitcast instruction count
        J.attribute("functions_with_unsafe_instructions", functions_with_unsafe_instructions); //Total IR functions with unsafe code in them
        J.attribute("percent_unsafe", percent_unsafe); //Percentage unsafe code
    });

    J.~OStream();
    OS.close();

    //llvm::errs() << "# Of unsafe add instructions in " << M.getName() << ": " << unsafe_add_instructions << "\n"
    //<< "# Of unsafe load instructions in " << M.getName() << ": " << unsafe_load_instructions << "\n" 
    //<< "# Of unsafe store instructions in " << M.getName() << ": " << unsafe_store_instructions << "\n"
    //<< "# Of unsafe pointer calculation instructions in " << M.getName() << ": " << unsafe_get_element_ptr_instructions << "\n";

    //llvm::errs() << "LineCount Returned\n";
  return PreservedAnalyses::all();
}