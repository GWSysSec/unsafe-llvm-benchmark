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

  json outputJson;
  std::string outputFile = "output.json";

  // if (std::filesystem::exists(outputFile)) {
  //   std::ifstream inFile(outputFile);
  //   if (inFile.is_open()) {
  //     inFile >> outputJson;
  //     inFile.close();
  //   } else {
  //     errs() << "Error: Could not open existing output.json for reading\n";
  //   }
  // }

  // .find(unsafe_inst)
  json functionJson;
  functionJson["Function"] =
      F.getParent()->getName().str() + "::" + F.getName().str();

  llvm::StringRef unsafe_inst = llvm::StringRef("unsafe_inst");
  json instructionsJson = json::array();

  for (auto &BB : F) {
    for (auto &I : BB) {
      if (DILocation *Loc = I.getDebugLoc()) {
        MDNode *is_unsafe = I.getMetadata(unsafe_inst);
        if (is_unsafe) {

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

          instructionsJson.push_back(instructionJson);
        }
      }
    }
  }

  functionJson["Instructions"] = instructionsJson;
  if (!outputJson.contains("Functions")) {
    outputJson["Functions"] = json::array();
  }
  outputJson["Functions"].push_back(functionJson);

  std::ofstream outFile(outputFile, std::ios::app);
  if (outFile.is_open()) {
    outFile << outputJson.dump(4); // Pretty print with 4 spaces
    outFile.close();
    errs() << "Output appended to output.json\n";
  } else {
    errs() << "Error: Could not open output.json for writing\n";
  }

  return PreservedAnalyses::all();
}
