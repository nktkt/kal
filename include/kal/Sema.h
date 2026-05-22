//===- Sema.h - the type checker ------------------------------------------===//
#pragma once

#include "kal/AST.h"
#include "kal/Type.h"
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace kal {

class DiagnosticEngine;

/// 型検査器。各 Expr に型を注釈し、型エラーを診断する。
/// リテラルは「期待型」を伝播させる双方向推論で型付けする。
class Sema {
public:
  explicit Sema(DiagnosticEngine &diag);

  /// 成功 (型エラーなし) なら true。
  bool run(Program &program);

private:
  struct FuncSig {
    std::vector<Type> params;
    Type ret;
  };

  Type check(Expr *e, std::optional<Type> expected);
  Type checkNumber(NumberExpr *e, std::optional<Type> expected);
  Type checkVariable(VariableExpr *e);
  Type checkBinary(BinaryExpr *e, std::optional<Type> expected);
  Type checkCall(CallExpr *e);
  Type checkIf(IfExpr *e, std::optional<Type> expected);
  Type checkFor(ForExpr *e);
  Type checkCast(CastExpr *e);
  Type checkStructLit(StructLitExpr *e);
  Type checkField(FieldExpr *e);
  Type checkTupleLit(TupleLitExpr *e, std::optional<Type> expected);
  Type checkTupleIndex(TupleIndexExpr *e);
  Type checkLet(LetExpr *e, std::optional<Type> expected);

  void checkFunction(FunctionDef &f);
  const StructDef *findStruct(const std::string &name) const;

  DiagnosticEngine &diag_;
  std::map<std::string, FuncSig> funcs_;
  std::map<std::string, Type> locals_;
  std::map<std::string, const StructDef *> structs_;
};

} // namespace kal
