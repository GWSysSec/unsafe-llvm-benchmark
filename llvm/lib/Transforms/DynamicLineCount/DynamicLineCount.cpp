//===--- DynamicLineCount.cpp - Track unsafe instructions ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// DynamicLineCount builds upon InstMarker to provide line-level coverage analysis:
// 1. Uses UnsafeAnalysis from InstMarker to identify unsafe instructions
// 2. Registers all unsafe lines with update_unsafe_line_counter() at program start
// 3. Adds mark_unsafe_line_executed() calls at each unsafe instruction
// 4. Generates coverage reports showing which unsafe lines were executed
// 5. Supports primary package filtering with CARGO_PRIMARY_PACKAGE=1
//
// The runtime library keeps track of all unsafe lines and reports coverage
// statistics at program exit, showing which unsafe code paths were exercised.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/DynamicLineCount/DynamicLineCount.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Constants.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include <set>
#include <string>
#include <algorithm>
#include <map>
#include <vector>

using namespace llvm;

PreservedAnalyses DynamicLineCountPass::run(Function &F, FunctionAnalysisManager &AM) {
  if (F.isDeclaration())
    return PreservedAnalyses::all();

  // Skip functions with certain prefixes (LLVM's own functions)
  std::string FnName = F.getName().str();
  if (FnName.find("llvm.") == 0 || 
      FnName.find("__") == 0 || 
      FnName == "main" || 
      FnName.find("_ZN") == 0 && (
         FnName.find("_ZN9__dynamic") == 0 ||
         FnName.find("_ZN4core") == 0 || 
         FnName.find("_ZN3std") == 0)) {
    return PreservedAnalyses::all();
  }
  
  // Check if we should only instrument the primary package
  if (!UnsafeAnalysisResult::isPrimaryPackage())
    return PreservedAnalyses::all();

  // Get the unsafe analysis result from InstMarkerPass
  auto &UnsafeResult = AM.getResult<UnsafeAnalysis>(F);
  
  // If there are no unsafe instructions, nothing to do
  if (UnsafeResult.TotalUnsafeInst == 0)
    return PreservedAnalyses::all();

  Module *M = F.getParent();
  LLVMContext &Ctx = M->getContext();
  bool Modified = false;

  // Prepare runtime function prototypes
  Type *VoidTy = Type::getVoidTy(Ctx);
  Type *Int64Ty = Type::getInt64Ty(Ctx);
  Type *Int8Ty = Type::getInt8Ty(Ctx);
  Type *Int8PtrTy = PointerType::getUnqual(Int8Ty);
  
  FunctionType *RuntimeFnTy = FunctionType::get(VoidTy, {Int64Ty, Int8PtrTy}, false);
  FunctionCallee UpdateUnsafeLine = M->getOrInsertFunction(UPDATE_UNSAFE_LINE_FN, RuntimeFnTy);
  FunctionCallee MarkUnsafeLine = M->getOrInsertFunction(MARK_UNSAFE_LINE_FN, RuntimeFnTy);
  
  // Set function attributes for runtime calls
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

  // STEP 1: Collect unique unsafe lines to register at module initialization
  std::map<std::string, Value*> FileGlobals;
  std::vector<std::pair<int64_t, std::string>> UnsafeLineRegistry;
  
  for (const auto &Info : UnsafeResult.UnsafeInsts) {
    // Only add each line once to the registry
    auto Key = std::make_pair(Info.File.str(), Info.Line);
    if (!UnsafeResult.InstrumentedLines.insert(Key).second)
      continue;
      
    UnsafeLineRegistry.push_back({Info.Line, Info.File.str()});
  }
  
  // Report if there are many unsafe lines
  if (UnsafeLineRegistry.size() > 10) {
    errs() << "[DynamicLineCount] Registering " << UnsafeLineRegistry.size() 
           << " unsafe lines in " << F.getName() << "\n";
  }
  
  // STEP 2: Create a module constructor to register all unsafe lines at program start
  if (!UnsafeLineRegistry.empty()) {
    std::string RegistrationFnName = "unsafe_line_register_" + F.getName().str() + 
                                     "_" + std::to_string(UnsafeLineRegistry.size());
    
    FunctionType *RegFnTy = FunctionType::get(VoidTy, false);
    Function *RegistrationFn = Function::Create(
      RegFnTy, GlobalValue::InternalLinkage, RegistrationFnName, M);
    
    BasicBlock *EntryBB = BasicBlock::Create(Ctx, "entry", RegistrationFn);
    IRBuilder<> RegBuilder(EntryBB);
    
    // Add calls to update_unsafe_line_counter for each unsafe line
    for (const auto &[Line, File] : UnsafeLineRegistry) {
      // Create or reuse global string for File
      Value *FileArg;
      if (FileGlobals.find(File) == FileGlobals.end()) {
        std::string GlobalName = "unsafe_reg_str_" + UnsafeAnalysisResult::sanitizeFileName(File);
        auto *FileConstant = ConstantDataArray::getString(Ctx, File, true);
        GlobalVariable *GV = new GlobalVariable(
          *M, FileConstant->getType(), /*isConstant=*/true,
          GlobalValue::InternalLinkage, FileConstant, GlobalName
        );
        GV->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
        FileGlobals[File] = RegBuilder.CreateBitCast(GV, Int8PtrTy);
      }
      FileArg = FileGlobals[File];
      
      // Register this unsafe line with the runtime
      RegBuilder.CreateCall(UpdateUnsafeLine, {
        ConstantInt::get(Int64Ty, Line),
        FileArg
      });
    }
    
    RegBuilder.CreateRetVoid();
    appendToGlobalCtors(*M, RegistrationFn, 0);
    Modified = true;
  }
  
  // STEP 3: Instrument each unsafe instruction to track execution
  int instrumentedCount = 0;
  
  for (const auto &Info : UnsafeResult.UnsafeInsts) {
    Instruction *UnsafeInst = Info.Inst;
    BasicBlock *BB = UnsafeInst->getParent();
    
    // Skip PHI nodes with no appropriate insertion point
    if ((isa<PHINode>(*UnsafeInst) || isa<LandingPadInst>(*UnsafeInst))) {
      bool HasInsertPoint = false;
      for (Instruction &I : *BB) {
        if (!isa<PHINode>(I) && !isa<LandingPadInst>(I)) {
          HasInsertPoint = true;
          break;
        }
      }
      if (!HasInsertPoint)
        continue;
    }
    
    // Create or reuse a global string for the file name
    std::string GlobalName = "unsafe_str_" + UnsafeAnalysisResult::sanitizeFileName(Info.File.str());
    Value *FileArg;
    
    if (FileGlobals.find(Info.File.str()) != FileGlobals.end()) {
      // Reuse already created global for this file
      FileArg = FileGlobals[Info.File.str()];
    } else {
      // Create a new global string for this file
      GlobalVariable *GV = M->getNamedGlobal(GlobalName);
      if (!GV) {
        auto *FileConstant = ConstantDataArray::getString(Ctx, Info.File.str(), true);
        GV = new GlobalVariable(
          *M, FileConstant->getType(), /*isConstant=*/true,
          GlobalValue::InternalLinkage, FileConstant, GlobalName
        );
        GV->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
      }
      FileArg = IRBuilder<>(Ctx).CreateBitCast(GV, Int8PtrTy);
      FileGlobals[Info.File.str()] = FileArg;
    }
    
    // Pick an appropriate insertion point for the execution tracking
    IRBuilder<> Builder(Ctx);
    if (isa<PHINode>(*UnsafeInst) || isa<LandingPadInst>(*UnsafeInst)) {
      // For PHI nodes, insert after the first non-PHI instruction
      for (Instruction &I : *BB) {
        if (!isa<PHINode>(I) && !isa<LandingPadInst>(I)) {
          Builder.SetInsertPoint(&I);
          break;
        }
      }
    } else {
      // For regular instructions, insert at the instruction
      Builder.SetInsertPoint(UnsafeInst);
    }
    
    // Insert call to mark_unsafe_line_executed
    Builder.CreateCall(MarkUnsafeLine, {
      ConstantInt::get(Int64Ty, Info.Line),
      FileArg
    });
    
    instrumentedCount++;
    Modified = true;
  }
  
  // Only output summary for functions with significant instrumentation
  if (instrumentedCount > 10) {
    errs() << "[DynamicLineCount] Instrumented " << instrumentedCount 
           << " lines in " << F.getName() << "\n";
  }
  
  return Modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
