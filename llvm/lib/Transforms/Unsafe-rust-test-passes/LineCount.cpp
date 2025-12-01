#include "llvm/Transforms/Unsafe-rust-test-passes/LineCount.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Constants.h"
#include <string>
#include <list>
#include "json.hpp" // Include JSON library
#include <fstream>
#include <iostream>
#include <sstream>

using json = nlohmann::json;

using namespace llvm;

//Types of instructions
static unsigned int total_line_count = 0;
static unsigned int total_unsafe_line_count = 0;
static unsigned int unsafe_load_instructions = 0;
static unsigned int unsafe_store_instructions = 0;
static unsigned int unsafe_add_instructions = 0;
static unsigned int unsafe_get_element_ptr_instructions = 0;
static unsigned int unsafe_sub_instructions = 0;
static unsigned int unsafe_alloca_instructions = 0;
static unsigned int unsafe_ptrtoint_instructions = 0;
static unsigned int unsafe_inttoptr_instructions = 0;
static unsigned int unsafe_bitcast_instructions = 0;
static unsigned int unsafe_call_instructions = 0;
static unsigned int functions_with_unsafe_instructions = 0;

static std::list<std::string> function_list;

PreservedAnalyses LineCount::run(Module &M,
                                      ModuleAnalysisManager &AM) {

    llvm::StringRef unsafe_string = llvm::StringRef("unsafe_inst"); //Official string ref for unsafe instructions
    static const char *UNSAFE_MARKER_BEGIN = "nop # marker_begin"; //new unsafe inst marker start
    static const char *UNSAFE_MARKER_END = "nop # marker_end"; //new unsafe inst marker end
    bool unsafe_block_started = false; //When true, all instructions are unsafe as they're inside the marker bounds

    //std::error_code e;
    //std::string json_filename = M.getName().str();
    //json_filename.append("_");
    //json_filename.append(std::to_string(rand()));
    //json_filename.append("_");
    //json_filename.append(".json");
    //llvm::StringRef json_filename_stringref = llvm::StringRef(json_filename);
    //llvm::raw_fd_ostream OS = raw_fd_ostream(json_filename_stringref, e);

    // if (e.value() != 0) {
    //     llvm::errs() << "Bad FD\n";
    // }

    //json::OStream J(OS, 4u);

    //J.flush();

    nlohmann::json outputJson;
    outputJson["Function List"] = nlohmann::json::array();
    std::string outputFile = "linecount_output.json";

    //llvm::errs() << "LineCount Initialised\n";
    for (Function &F : M) {

        unsigned int function_line_count = 0;
        unsigned int unsafe_function_line_count = 0;
        unsigned int unsafe_function_load_instructions = 0;
        unsigned int unsafe_function_store_instructions = 0;
        unsigned int unsafe_function_add_instructions = 0;
        unsigned int unsafe_function_get_element_ptr_instructions = 0;
        unsigned int unsafe_function_sub_instructions = 0;
        unsigned int unsafe_function_alloca_instructions = 0;
        unsigned int unsafe_function_ptrtoint_instructions = 0;
        unsigned int unsafe_function_inttoptr_instructions = 0;
        unsigned int unsafe_function_bitcast_instructions = 0;
        unsigned int unsafe_function_call_instructions = 0;

        bool repeat_function = false;

        nlohmann::json functionJson;
        functionJson["Function"] = nlohmann::json::array(); //Array where we'll put all of our functions we track

        std::string function_name = F.getName().str();

        //Check function list to see if we've counted this one already
        for (std::string n : function_list) {
            if (n.compare(function_name) == 0) {
                repeat_function = true;
                break;
            }
        }

        //If it's a repeat, don't count, move on to next function
        if (repeat_function == true) {
            llvm::errs() << "Repeat function: " << function_name << "\n";
            continue;
        }

        for (BasicBlock &BB : F) {
            
            total_line_count += BB.size();
            function_line_count += BB.size();

            if (unsafe_block_started == true) {
                unsafe_block_started == false; //Prevents double counting instructions/unpaired instructions screwing up counts
            }

            for (Instruction &I : BB) {
                
                //MDNode* is_unsafe = I.getMetadata(unsafe_string);
                if (llvm::CallInst *CI = llvm::dyn_cast<llvm::CallInst>(&I)) { //Checking for INST marker flag - derived from HeapTracker.cpp
                    //llvm::errs() << "CallInst passed\n";
                    if (llvm::InlineAsm *IA = llvm::dyn_cast<llvm::InlineAsm>(CI->getCalledOperand())) {
                        //llvm::errs() << "IA passed\n";
                        llvm::StringRef AsmStr = IA->getAsmString();
                        //llvm::errs() << "ASM String: " << AsmStr << "\n";
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
                            ++unsafe_function_add_instructions;
                            break;
                        case Instruction::Load:
                            ++unsafe_load_instructions;
                            ++unsafe_function_load_instructions;
                            break;
                        case Instruction::Store:
                            ++unsafe_store_instructions;
                            ++unsafe_function_store_instructions;
                            break;
                        case Instruction::GetElementPtr:
                            ++unsafe_get_element_ptr_instructions;
                            ++unsafe_function_get_element_ptr_instructions;
                            break;
                        case Instruction::Sub:
                            ++unsafe_sub_instructions;
                            ++unsafe_function_sub_instructions;
                            break;
                        case Instruction::Alloca:
                            ++unsafe_alloca_instructions;
                            ++unsafe_function_alloca_instructions;
                            break;
                        case Instruction::PtrToInt:
                            ++unsafe_ptrtoint_instructions;
                            ++unsafe_function_ptrtoint_instructions;
                            break;
                        case Instruction::IntToPtr:
                            ++unsafe_inttoptr_instructions;
                            ++unsafe_function_inttoptr_instructions;
                            break;
                        case Instruction::BitCast:
                            ++unsafe_bitcast_instructions;
                            ++unsafe_function_bitcast_instructions;
                            break;
                        case Instruction::Call: //Counts when an unsafe function call is made
                            ++unsafe_call_instructions;
                            ++unsafe_function_call_instructions;
                            break;
                        default:
                            break;
                    }

                    //Instruction for loop
                }

            }

        }

        if (unsafe_function_line_count > 0) { //If the function has unsafe IR instructions, count it
            functions_with_unsafe_instructions++;
        } else {
            continue; //Don't count functions with 0 unsafe lines individually on the json file
        }

        //Add function elements to json file (how much of each instruction type is in each function)

        nlohmann::json instructions;
        instructions["function_name"] = function_name;
        instructions["IR_count"] = function_line_count;
        instructions["unsafe_IR_count"] = unsafe_function_line_count;
        instructions["unsafe_adds"] = unsafe_function_add_instructions;
        instructions["unsafe_subs"] = unsafe_function_sub_instructions;
        instructions["unsafe_loads"] = unsafe_function_load_instructions;
        instructions["unsafe_stores"] = unsafe_function_store_instructions;
        instructions["unsafe_getelementptrs"] = unsafe_function_get_element_ptr_instructions;
        instructions["unsafe_allocas"] = unsafe_function_alloca_instructions;
        instructions["unsafe_PtrtoInts"] = unsafe_function_ptrtoint_instructions;
        instructions["unsafe_InttoPtrs"] = unsafe_function_inttoptr_instructions;
        instructions["unsafe_bitcasts"] = unsafe_function_bitcast_instructions;
        instructions["unsafe_calls"] = unsafe_function_call_instructions;

        functionJson["Function"].push_back(instructions); //Adding specific data to our json function array entry

        outputJson["Function List"].push_back(functionJson); //Adding specific data to overall function list

        function_list.push_back(function_name);

    }
    //llvm::errs() << "LineCount loop finished\n";
    //llvm::errs() << "# Of instructions in " << M.getName() << ": " << total_line_count << "\n"
    //<< "# Of unsafe instructions in " << M.getName() << ": " << total_unsafe_line_count << "\n";
    
    float percent_unsafe = 0;

    if (total_unsafe_line_count != 0) {
        percent_unsafe = (float) ( (float) total_unsafe_line_count / (float) total_line_count) * 100.0;
    }

    nlohmann::json file_totals;
    file_totals["module_name"] = M.getName().str();
    file_totals["IR_count"] = total_line_count;
    file_totals["unsafe_IR_count"] = total_unsafe_line_count;
    file_totals["unsafe_adds"] = unsafe_add_instructions;
    file_totals["unsafe_subs"] = unsafe_sub_instructions;
    file_totals["unsafe_loads"] = unsafe_load_instructions;
    file_totals["unsafe_stores"] = unsafe_store_instructions;
    file_totals["unsafe_getelementptrs"] = unsafe_get_element_ptr_instructions;
    file_totals["unsafe_allocas"] = unsafe_alloca_instructions;
    file_totals["unsafe_PtrtoInts"] = unsafe_ptrtoint_instructions;
    file_totals["unsafe_InttoPtrs"] = unsafe_inttoptr_instructions;
    file_totals["unsafe_bitcasts"] = unsafe_bitcast_instructions;
    file_totals["unsafe_calls"] = unsafe_call_instructions;
    file_totals["percent_unsafe"] = percent_unsafe;

    if (outputJson["Function List"].empty() == false) { //If something's in the function list, append to json file
        //First, append module/file total
        outputJson["Module Total"] = nlohmann::json::array();
        outputJson["Module Total"].push_back(file_totals);

        std::ofstream outFile(outputFile, std::ios::app);
        if (outFile.is_open()) {
            outFile << outputJson.dump(4); // Pretty print with 4 spaces
            outFile.close();
            errs() << "Output appended to linecount_output.json\n";
        } else {
            errs() << "Error: Could not open linecount_output.json for writing\n";
        }
    }

    //New final JSON output for whole file at the end

    // J.object([&] {
    //     J.attribute("module_name", M.getName()); //Module name
    //     J.attribute("total_IR_count", total_line_count); //Total instruction count
    //     J.attribute("total_unsafe_IR_count", total_unsafe_line_count); //Total unsafe instruction count
    //     J.attribute("total_unsafe_add", unsafe_add_instructions); //Total unsafe add instruction count
    //     J.attribute("total_unsafe_load", unsafe_load_instructions); //Total unsafe load instruction count
    //     J.attribute("total_unsafe_store", unsafe_store_instructions); //Total unsafe store instruction count
    //     J.attribute("total_unsafe_get_element_ptr", unsafe_get_element_ptr_instructions); //Total unsafe pointer arithmetic instruction count
    //     J.attribute("total_unsafe_sub", unsafe_sub_instructions); //Total unsafe sub instruction count
    //     J.attribute("total_unsafe_alloca", unsafe_alloca_instructions); //Total unsafe alloca instruction count
    //     J.attribute("total_unsafe_ptrtoint", unsafe_ptrtoint_instructions); //Total unsafe ptrtoint instruction count
    //     J.attribute("total_unsafe_inttoptr", unsafe_inttoptr_instructions); //Total unsafe inttoptr instruction count
    //     J.attribute("total_unsafe_bitcast", unsafe_bitcast_instructions); //Total unsafe bitcast instruction count
    //     J.attribute("functions_with_unsafe_IR", functions_with_unsafe_instructions); //Total IR functions with unsafe code in them
    //     J.attribute("percent_unsafe", percent_unsafe); //Percentage unsafe code
    // });

    // J.~OStream();
    // OS.close();

    //llvm::errs() << "# Of unsafe add instructions in " << M.getName() << ": " << unsafe_add_instructions << "\n"
    //<< "# Of unsafe load instructions in " << M.getName() << ": " << unsafe_load_instructions << "\n" 
    //<< "# Of unsafe store instructions in " << M.getName() << ": " << unsafe_store_instructions << "\n"
    //<< "# Of unsafe pointer calculation instructions in " << M.getName() << ": " << unsafe_get_element_ptr_instructions << "\n";

    //llvm::errs() << "LineCount Returned\n";
    //llvm::errs() << "function_list\n";

    //for (std::string n : function_list) {
    //    llvm::errs() << n << "\n";
    //}

  return PreservedAnalyses::all();
}

