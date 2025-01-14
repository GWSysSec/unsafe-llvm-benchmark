//===-- SourceCodeMapping.cpp - Example Transformations
//---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/SourceCodeMapping/SourceCodeMapping.h"
#include "json.hpp" // Include JSON library
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Function.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

using json = nlohmann::json;

using namespace llvm;

PreservedAnalyses SourceCodeMappingPass::run(Function &F,
                                             FunctionAnalysisManager &AM) {
  errs() << "[SourceCodeMapping3]: Processing Fn " << F.getParent()->getName()
         << "::" << F.getName() << "\n";

  errs() << "json outputJson initialisation\n";
  json outputJson;
  std::string outputFile = "output.json";
  errs() << "json outputJson post initialisation\n";

  if (std::filesystem::exists(outputFile)) {
    errs() << "output file exists\n";
    std::ifstream inFile(outputFile);
    if (inFile.is_open()) {
      errs() << "file is open\n";
      inFile >> outputJson;
      errs() << "inFile >> outputJson operator run\n";
      inFile.close();
      errs() << "file is closed\n";
    } else {
      errs() << "Error: Could not open existing output.json for reading\n";
    }
  }
  errs() << "output file doesn't exist\n";
  // .find(unsafe_inst)
  errs() << "json functionJson initialisation\n";
  json functionJson;
  functionJson["Function"] =
      F.getParent()->getName().str() + "::" + F.getName().str();
  errs() << "json functionJson post initialisation\n";

  llvm::StringRef unsafe_inst = llvm::StringRef("unsafe_inst");
  errs() << "initialising json array\n";
  json instructionsJson = json::array();
  errs() << "post initialising json array\n";

  for (auto &BB : F) {
    for (auto &I : BB) {
      if (DILocation *Loc = I.getDebugLoc()) {
        MDNode *is_unsafe = I.getMetadata(unsafe_inst);
        if (is_unsafe) {
          errs() << "if is_unsafe triggered\n";
          unsigned Line = Loc->getLine();
          StringRef File = Loc->getFilename();
          StringRef Directory = Loc->getDirectory();
          std::string FullPath = (Directory + "/" + File).str();

          std::string sourceLine;
          std::ifstream file(FullPath);
          if (file.is_open()) {
            std::string line;
            int currentLine = 1;
            while (std::getline(file, line)) {
              if (currentLine == Line) {
                sourceLine = line;
                break;
              }
              ++currentLine;
            }
            file.close();
          }

          json instructionJson;
          instructionJson["File"] = FullPath;
          instructionJson["Line"] = Line;
          instructionJson["SourceLine"] = sourceLine;
          std::string instrStr;
          llvm::raw_string_ostream instrStream(instrStr);
          I.print(instrStream); // Convert LLVM IR to string
          instructionJson["LLVM IR"] = instrStream.str();
          errs() << "push back called\n";
          instructionsJson.push_back(instructionJson);

          // unsigned num_ops = Loc->getNumOperands();
          // for (int i = 0; i++; i < num_ops) {
          //   errs() << "[MD Operand]: " << Loc->getOperand(i) << "\n";
          // }

          // errs() << "MD Ops: " << num_ops << "\n";

          // errs() << "[Source Lines]: " << FullPath << ":" << Line << "\n";

          // std::ifstream file(FullPath);

          // std::string line;
          // int currentLine = 1;
          // while (std::getline(file, line)) {
          //   if (currentLine == Line) {
          //     errs() << "Line " << Line << ": " << line << "\n";
          //   }
          //   ++currentLine;
          // }

          // errs() << "[LLVM IR]: " << I << "\n";
        }
        errs() << "safe inst\n";
      }
    }
  }

  functionJson["Instructions"] = instructionsJson;
  if (!outputJson.contains("Functions")) {
    errs() << "function array called\n";
    outputJson["Functions"] = json::array();
  }
  outputJson["Functions"].push_back(functionJson);

  std::ofstream outFile(outputFile);
  if (outFile.is_open()) {
    outFile << outputJson.dump(4); // Pretty print with 4 spaces
    outFile.close();
    //errs() << "Output appended to output.json\n";
  } else {
    errs() << "Error: Could not open output.json for writing\n";
  }

  return PreservedAnalyses::all();
}
