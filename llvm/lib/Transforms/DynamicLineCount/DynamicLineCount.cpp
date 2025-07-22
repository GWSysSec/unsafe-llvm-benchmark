//===-- DynamicLineCount.cpp - Track unsafe source line coverage -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file implements the DynamicLineCount pass for tracking unsafe source
/// line coverage.
///
//===----------------------------------------------------------------------==//

#include "llvm/Transforms/DynamicLineCount/DynamicLineCount.h"
#include "llvm/Transforms/InstMarker/InstMarker.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/Casting.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include <map>
#include <set>
#include <string>

using namespace llvm;

const char *llvm::REGISTER_UNSAFE_LINE_FN = "register_unsafe_line";
const char *llvm::EXECUTE_UNSAFE_BLOCK_FN = "execute_unsafe_block";
const char *llvm::DYNAMIC_LINE_PRINT_STATS_FN = "print_unsafe_coverage_stats";

namespace {

/// \brief Information about a unique source line within an unsafe block.
struct LineInfo {
  unsigned Line;
  std::string File;

  bool operator<(const LineInfo &Other) const {
    if (File != Other.File) return File < Other.File;
    return Line < Other.Line;
  }
};

/// \brief Creates a global string constant for the given string value.
/// \param Builder IRBuilder to use for creating the global.
/// \param Str The string value to create a global for.
/// \returns A pointer to the global string constant.
Value *createGlobalString(IRBuilder<> &Builder, StringRef Str) {
  Module *M = Builder.GetInsertBlock()->getParent()->getParent();
  LLVMContext &Ctx = M->getContext();

  Constant *StrConstant = ConstantDataArray::getString(Ctx, Str, true);
  GlobalVariable *GV = new GlobalVariable(*M, StrConstant->getType(), true,
                                          GlobalValue::InternalLinkage, StrConstant);
  return Builder.CreateBitCast(GV, PointerType::getUnqual(Type::getInt8Ty(Ctx)));
}

/// \brief Processes an unsafe block, inserts execution tracking, and returns line info.
/// \param MarkerBegin The instruction containing UNSAFE_MARKER_BEGIN.
/// \param MarkerEnd The instruction containing UNSAFE_MARKER_END.
/// \param BlockId Unique identifier for this unsafe block.
/// \param ExecuteBlockFn Function to call for tracking block execution.
/// \returns A set of unique source lines found within the block.
std::set<LineInfo> processUnsafeBlock(Instruction *MarkerBegin, Instruction *MarkerEnd,
                                      int BlockId, FunctionCallee ExecuteBlockFn) {
  std::set<LineInfo> UniqueLines;

  // Collect unique lines between markers using preserved metadata
  for (Instruction *I = MarkerBegin->getNextNode(); I != MarkerEnd;
       I = I->getNextNode()) {
    if (MDNode *LineInfoMD = I->getMetadata("unsafe_line_info")) {
      if (LineInfoMD->getNumOperands() >= 2) {
        if (auto *LineConst = dyn_cast<ConstantAsMetadata>(LineInfoMD->getOperand(0))) {
          if (auto *FileStr = dyn_cast<MDString>(LineInfoMD->getOperand(1))) {
            unsigned Line = LineConst->getValue()->getUniqueInteger().getZExtValue();
            std::string File = FileStr->getString().str();
            UniqueLines.insert({Line, File});
          }
        }
      }
    }
  }

  IRBuilder<> Builder(MarkerBegin->getNextNode());
  Value *BlockArg = ConstantInt::get(Type::getInt32Ty(Builder.getContext()), BlockId);
  Builder.CreateCall(ExecuteBlockFn, {BlockArg});

  return UniqueLines;
}


/// \brief Instruments unsafe blocks to track execution and collect line info.
/// \param F The target function to be instrumented.
/// \param ExecuteBlockFn Function to call for tracking block execution.
/// \param BlockCounter Reference to global block counter for unique IDs.
/// \param AllUnsafeLines A map to populate with line info from all found blocks.
/// \returns True if the function was modified, false otherwise.
bool instrumentUnsafeBlocks(Function &F, FunctionCallee ExecuteBlockFn, int &BlockCounter,
                           std::map<int, std::set<LineInfo>> &AllUnsafeLines) {
  bool Modified = false;

  for (BasicBlock &BB : F) {
    Instruction *MarkerBegin = nullptr;

    for (Instruction &I : BB) {
      if (auto *CallInst = dyn_cast<CallBase>(&I)) {
        if (auto *InlineAsm = dyn_cast<llvm::InlineAsm>(CallInst->getCalledOperand())) {
          StringRef AsmStr = InlineAsm->getAsmString();

          if (AsmStr == UNSAFE_MARKER_BEGIN) {
            MarkerBegin = &I;
          }
          else if (AsmStr == UNSAFE_MARKER_END && MarkerBegin) {
            bool AlreadyInstrumented = false;
            for (Instruction *CheckI = MarkerBegin->getNextNode(); CheckI != &I;
                 CheckI = CheckI->getNextNode()) {
              if (CallBase *CheckCall = dyn_cast<CallBase>(CheckI)) {
                if (CheckCall->getCalledFunction() &&
                    CheckCall->getCalledFunction()->getName() == EXECUTE_UNSAFE_BLOCK_FN) {
                  AlreadyInstrumented = true;
                  break;
                }
              }
            }

            if (!AlreadyInstrumented) {
              // Process the block to insert execution tracking and get line info back.
              std::set<LineInfo> BlockLines = processUnsafeBlock(MarkerBegin, &I, BlockCounter, ExecuteBlockFn);
              if (!BlockLines.empty()) {
                  AllUnsafeLines[BlockCounter] = BlockLines;
              }
              BlockCounter++; // Increment for a unique ID for the next block.
              Modified = true;
            }
            MarkerBegin = nullptr;
          }
        }
      }
    }
  }
  return Modified;
}

} // anonymous namespace

PreservedAnalyses DynamicLineCountPass::run(Module &M, ModuleAnalysisManager &AM) {
  bool Modified = false;
  LLVMContext &Ctx = M.getContext();

  // Declare the runtime functions
  Type *VoidTy = Type::getVoidTy(Ctx);
  Type *Int32Ty = Type::getInt32Ty(Ctx);
  Type *Int64Ty = Type::getInt64Ty(Ctx);
  Type *Int8PtrTy = PointerType::getUnqual(Type::getInt8Ty(Ctx));

  FunctionType *RegisterLineFnTy = FunctionType::get(VoidTy, {Int32Ty, Int64Ty, Int8PtrTy}, false);
  FunctionCallee RegisterLineFn = M.getOrInsertFunction(REGISTER_UNSAFE_LINE_FN, RegisterLineFnTy);

  FunctionType *ExecuteBlockFnTy = FunctionType::get(VoidTy, {Int32Ty}, false);
  FunctionCallee ExecuteBlockFn = M.getOrInsertFunction(EXECUTE_UNSAFE_BLOCK_FN, ExecuteBlockFnTy);

  int GlobalBlockCounter = 0;
  // A map to collect all unsafe lines from the entire module.
  std::map<int, std::set<LineInfo>> AllUnsafeLines;

  // Instrument all functions to track execution and collect line info
  for (Function &F : M) {
    if (F.isDeclaration() || F.getName().startswith("register_") ||
        F.getName().startswith("execute_") || F.getName().startswith("print_")) {
      continue;
    }
    // Pass the map to be populated.
    Modified |= instrumentUnsafeBlocks(F, ExecuteBlockFn, GlobalBlockCounter, AllUnsafeLines);
  }

  // If we found any unsafe lines, create a global constructor to register them at startup.
  if (!AllUnsafeLines.empty()) {
    FunctionType *CtorTy = FunctionType::get(Type::getVoidTy(Ctx), false);
    Function *Ctor = Function::Create(CtorTy, GlobalValue::InternalLinkage, "unsafe_coverage.ctor", &M);
    BasicBlock *CtorBB = BasicBlock::Create(Ctx, "entry", Ctor);
    IRBuilder<> CtorBuilder(CtorBB);

    // Create registration calls for every collected line
    for (const auto &BlockPair : AllUnsafeLines) {
        int BlockId = BlockPair.first;
        const std::set<LineInfo> &Lines = BlockPair.second;

        for (const auto &Line : Lines) {
            Value *FileArg = createGlobalString(CtorBuilder, Line.File);
            Value *LineArg = ConstantInt::get(Type::getInt64Ty(Ctx), Line.Line);
            Value *BlockArg = ConstantInt::get(Type::getInt32Ty(Ctx), BlockId);
            CtorBuilder.CreateCall(RegisterLineFn, {BlockArg, LineArg, FileArg});
        }
    }
    CtorBuilder.CreateRetVoid();
    appendToGlobalCtors(M, Ctor, 0);
  }

  // Add the global destructor to print stats at exit (this logic remains the same)
  if (Modified) {
    FunctionType *PrintFnTy = FunctionType::get(VoidTy, false);
    FunctionCallee PrintStatsFn = M.getOrInsertFunction(DYNAMIC_LINE_PRINT_STATS_FN, PrintFnTy);
    Function *Dtor = Function::Create(PrintFnTy, GlobalValue::InternalLinkage, "unsafe_coverage.dtor", &M);
    BasicBlock *DtorBB = BasicBlock::Create(Ctx, "entry", Dtor);
    IRBuilder<> DtorBuilder(DtorBB);
    DtorBuilder.CreateCall(PrintStatsFn, {});
    DtorBuilder.CreateRetVoid();
    appendToGlobalDtors(M, Dtor, 0);
  }

  return Modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
