#include "llvm/Transforms/DynamicLineCount/DynamicLineCount.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include <string>
#include <vector>
#include <set>
#include <map>

using namespace llvm;

PreservedAnalyses DynamicLineCountPass::run(Module &M, ModuleAnalysisManager &AM) {
  LLVMContext &Ctx = M.getContext();
  Type *VoidTy = Type::getVoidTy(Ctx);
  Type *Int64Ty = Type::getInt64Ty(Ctx);
  Type *Int8PtrTy = PointerType::getUnqual(Type::getInt8Ty(Ctx));
  
  FunctionType *RuntimeFnTy = FunctionType::get(VoidTy, {Int64Ty, Int8PtrTy}, false);
  FunctionCallee UpdateUnsafeLine = M.getOrInsertFunction(UPDATE_UNSAFE_LINE_FN, RuntimeFnTy);
  FunctionCallee MarkUnsafeLine = M.getOrInsertFunction(MARK_UNSAFE_LINE_FN, RuntimeFnTy);
  
  for (auto *RuntimeFn : {
      dyn_cast<Function>(UpdateUnsafeLine.getCallee()),
      dyn_cast<Function>(MarkUnsafeLine.getCallee())}) {
    if (RuntimeFn) {
      RuntimeFn->removeFnAttr(Attribute::ReadNone);
      RuntimeFn->removeFnAttr(Attribute::ReadOnly);
      RuntimeFn->addFnAttr(Attribute::NoInline);
      RuntimeFn->setLinkage(GlobalValue::ExternalLinkage);
    }
  }

  bool Modified = false;
  int instrumentedLines = 0;
  std::set<std::pair<std::string, unsigned>> RegisteredLines;
  std::map<std::string, Value*> FileGlobals;
  
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
      
    std::string FnName = F.getName().str();
    if (FnName.find("llvm.") == 0 || 
        FnName.find("__") == 0 || 
        FnName.find("update_unsafe_line_") == 0 ||
        FnName.find("mark_unsafe_line_") == 0 ||
        FnName == "main" ||
        FnName.find("_ZN") == 0 && (
           FnName.find("_ZN9__dynamic") == 0 ||
           FnName.find("_ZN4core") == 0 || 
           FnName.find("_ZN3std") == 0)) {
      continue;
    }
    
    for (BasicBlock &BB : F) {
      Instruction *MarkerBegin = nullptr;
      
      for (Instruction &I : BB) {
        if (auto *CallInst = dyn_cast<CallBase>(&I)) {
          if (auto *InlineAsmCall = dyn_cast<InlineAsm>(CallInst->getCalledOperand()->stripPointerCasts())) {
            StringRef AsmStr = InlineAsmCall->getAsmString();
            
            if (AsmStr.contains("marker_begin")) {
              MarkerBegin = &I;
            } else if (AsmStr.contains("marker_end") && MarkerBegin) {
              
              DebugLoc DL = MarkerBegin->getDebugLoc();
              if (DL) {
                const DILocation *Loc = DL.get();
                StringRef File = Loc->getFilename();
                if (!File.empty()) {
                  unsigned Line = Loc->getLine();
                  
                  Value *FileArg;
                  if (FileGlobals.find(File.str()) == FileGlobals.end()) {
                    std::string GlobalName = "unsafe_line_str_" + std::to_string(FileGlobals.size());
                    auto *FileConstant = ConstantDataArray::getString(Ctx, File, true);
                    GlobalVariable *GV = new GlobalVariable(
                      M, FileConstant->getType(), true,
                      GlobalValue::InternalLinkage, FileConstant, GlobalName
                    );
                    GV->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
                    FileGlobals[File.str()] = IRBuilder<>(Ctx).CreateBitCast(GV, Int8PtrTy);
                  }
                  FileArg = FileGlobals[File.str()];
                  
                  auto Key = std::make_pair(File.str(), Line);
                  if (RegisteredLines.insert(Key).second) {
                    if (Function *MainFn = M.getFunction("main")) {
                      if (!MainFn->empty()) {
                        BasicBlock &EntryBB = MainFn->getEntryBlock();
                        if (!EntryBB.empty()) {
                          IRBuilder<> RegBuilder(&EntryBB.front());
                          RegBuilder.CreateCall(UpdateUnsafeLine, {
                            ConstantInt::get(Int64Ty, Line),
                            FileArg
                          });
                        }
                      }
                    }
                  }
                  
                  IRBuilder<> Builder(MarkerBegin->getNextNode() ? 
                                    MarkerBegin->getNextNode() : MarkerBegin);
                  Builder.CreateCall(MarkUnsafeLine, {
                    ConstantInt::get(Int64Ty, Line),
                    FileArg
                  });
                  
                  instrumentedLines++;
                  Modified = true;
                }
              }
              MarkerBegin = nullptr;
            }
          }
        }
      }
    }
  }
  
  if (instrumentedLines > 0) {
    errs() << "[DynamicLineCount] Instrumented " << instrumentedLines << " unsafe lines\n";
  }
  
  return Modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}