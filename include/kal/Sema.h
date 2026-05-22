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
  struct VariantInfo {
    std::string enumName;
    int tag;
    std::vector<Type> payloadTypes;
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
  Type checkMatch(MatchExpr *e, std::optional<Type> expected);
  Type checkBorrow(BorrowExpr *e, std::optional<Type> expected);
  Type checkDeref(DerefExpr *e);

  void checkFunction(FunctionDef &f);
  const StructDef *findStruct(const std::string &name) const;
  const EnumDef *findEnum(const std::string &name) const;
  // パーサは未知の名前型を Struct 種として作る。enum 名なら Enum 種へ直す。
  Type resolve(Type t) const;

  DiagnosticEngine &diag_;
  std::map<std::string, FuncSig> funcs_;
  std::map<std::string, Type> locals_;
  std::map<std::string, const StructDef *> structs_;
  std::map<std::string, const EnumDef *> enums_;
  std::map<std::string, VariantInfo> variants_;
};

} // namespace kal
