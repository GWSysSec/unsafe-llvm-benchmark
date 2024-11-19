//===-- SourceCodeMapping.cpp - Example Transformations
//---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/SourceCodeMapping/SourceCodeMapping.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Function.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

using namespace llvm;

PreservedAnalyses SourceCodeMappingPass::run(Function &F,
                                             FunctionAnalysisManager &AM) {
  errs() << "[SourceCodeMapping3]: Processing Fn " << F.getParent()->getName()
         << "::" << F.getName() << "\n";

  // .find(unsafe_inst)
  llvm::StringRef unsafe_inst = llvm::StringRef("unsafe_inst");

  for (auto &BB : F) {
    for (auto &I : BB) {
      if (DILocation *Loc = I.getDebugLoc()) {
        MDNode *is_unsafe = I.getMetadata(unsafe_inst);
        if (is_unsafe) {

          // errs() << "Metadata Node: " << is_unsafe << "\n";
          unsigned Line = Loc->getLine();
          StringRef File = Loc->getFilename();
          StringRef Directory = Loc->getDirectory();
          std::string FullPath = (Directory + "/" + File).str();

          unsigned num_ops = Loc->getNumOperands();
          for (int i = 0; i++; i < num_ops) {
            errs() << "[MD Operand]: " << Loc->getOperand(i) << "\n";
          }

          errs() << "MD Ops: " << num_ops << "\n";

          errs() << "[Source Lines]: " << FullPath << ":" << Line << "\n";

          std::ifstream file(FullPath);

          std::string line;
          int currentLine = 1;
          while (std::getline(file, line)) {
            if (currentLine == Line) {
              errs() << "Line " << Line << ": " << line << "\n";
            }
            ++currentLine;
          }

          errs() << "[LLVM IR]: " << I << "\n";
        }
      }
    }
  }

  return PreservedAnalyses::all();
}
