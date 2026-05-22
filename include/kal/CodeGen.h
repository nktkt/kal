//===- CodeGen.h - lowers the AST to an LLVM module -----------------------===//
#pragma once

#include "kal/AST.h"
#include "kal/Diagnostic.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include <map>
#include <memory>
#include <string>

namespace kal {

class CodeGen {
public:
  CodeGen(llvm::LLVMContext &ctx, DiagnosticEngine &diag);

  /// Program を LLVM モジュールに変換する。
  /// 失敗時は nullptr を返す (診断は diag に報告済み)。
  std::unique_ptr<llvm::Module> run(const Program &program);

private:
  llvm::Type *doubleTy();

  llvm::Value *genExpr(const Expr *e);
  llvm::Value *genNumber(const NumberExpr *e);
  llvm::Value *genVariable(const VariableExpr *e);
  llvm::Value *genBinary(const BinaryExpr *e);
  llvm::Value *genCall(const CallExpr *e);
  llvm::Value *genIf(const IfExpr *e);
  llvm::Value *genFor(const ForExpr *e);

  llvm::Function *declareProto(const Prototype &p);
  bool genFunction(const FunctionDef &f);

  llvm::LLVMContext &ctx_;
  DiagnosticEngine &diag_;
  std::unique_ptr<llvm::Module> module_;
  llvm::IRBuilder<> builder_;
  std::map<std::string, llvm::Value *> namedValues_;
};

} // namespace kal
