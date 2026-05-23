//===- CodeGen.h - lowers the AST to an LLVM module -----------------------===//
#pragma once

#include "kal/AST.h"
#include "kal/Diagnostic.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include <map>
#include <memory>
#include <string>

namespace kal {

class CodeGen {
public:
  CodeGen(llvm::LLVMContext &ctx, DiagnosticEngine &diag,
          const llvm::DataLayout &dl);

  /// Program を LLVM モジュールに変換する。
  /// emitRuntime=true なら自己完結バイナリ用に printi/printd/putchard の本体と
  /// C の main を生成する (AOT 用)。false なら宣言のみ (JIT がホスト側で解決)。
  /// 失敗時は nullptr を返す (診断は diag に報告済み)。
  std::unique_ptr<llvm::Module> run(const Program &program,
                                    bool emitRuntime = false);

private:
  llvm::Type *toLLVM(const Type &t); // kal::Type → llvm::Type (unit は void)

  llvm::Value *genExpr(const Expr *e);
  llvm::Value *genNumber(const NumberExpr *e);
  llvm::Value *genVariable(const VariableExpr *e);
  llvm::Value *genBinary(const BinaryExpr *e);
  llvm::Value *genUnary(const UnaryExpr *e);
  llvm::Value *genCall(const CallExpr *e);
  llvm::Value *genIf(const IfExpr *e);
  llvm::Value *genFor(const ForExpr *e);
  llvm::Value *genCast(const CastExpr *e);
  llvm::Value *genStructLit(const StructLitExpr *e);
  llvm::Value *genField(const FieldExpr *e);
  llvm::Value *genTupleLit(const TupleLitExpr *e);
  llvm::Value *genTupleIndex(const TupleIndexExpr *e);
  llvm::Value *genBlock(const BlockExpr *e);
  llvm::Value *genAssign(const AssignExpr *e);
  llvm::Value *genArrayLit(const ArrayLitExpr *e);
  llvm::Value *genIndex(const IndexExpr *e);
  llvm::Value *genMatch(const MatchExpr *e);
  llvm::Value *genBorrow(const BorrowExpr *e);
  llvm::Value *genDeref(const DerefExpr *e);
  // 場所 (lvalue) のアドレスを返す。一時値はメモリに退避する。
  llvm::Value *genAddr(const Expr *e);
  // entry ブロックに alloca を作る (mem2reg が昇格できる)
  llvm::AllocaInst *entryAlloca(llvm::Type *ty, const std::string &name);
  // enum バリアント構築 (tag + ペイロード)
  llvm::Value *genVariant(const std::string &enumName, int tag,
                          llvm::ArrayRef<llvm::Value *> payload);

  llvm::StructType *getStructType(const std::string &name); // 名前→LLVM構造体型
  llvm::StructType *getEnumType(const std::string &name);   // 名前→タグ付き共用体
  llvm::Function *declareProto(const Prototype &p);
  bool genFunction(const FunctionDef &f);
  void emitRuntimeDefs(); // printi/printd/putchard の本体 (libc 呼び出し)
  void emitCMain();       // i32 main() { __main(); return 0; }

  llvm::LLVMContext &ctx_;
  DiagnosticEngine &diag_;
  llvm::DataLayout dl_;
  std::unique_ptr<llvm::Module> module_;
  llvm::IRBuilder<> builder_;
  std::map<std::string, llvm::Value *> namedValues_;
  std::map<std::string, const StructDef *> structDefs_;
  std::map<std::string, llvm::StructType *> structTypes_;
  std::map<std::string, const EnumDef *> enumDefs_;
  std::map<std::string, llvm::StructType *> enumTypes_;
};

} // namespace kal
