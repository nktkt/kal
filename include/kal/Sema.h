//===- Sema.h - the type checker ------------------------------------------===//
#pragma once

#include "kal/AST.h"
#include "kal/Type.h"
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
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
    std::vector<Type> payloadTypes;      // ジェネリックなら Param 型を含む
    std::vector<std::string> typeParams; // 所属 enum の型引数 (空なら非総称)
  };

  Type check(Expr *e, std::optional<Type> expected);
  Type checkNumber(NumberExpr *e, std::optional<Type> expected);
  Type checkVariable(VariableExpr *e, std::optional<Type> expected);
  Type checkBinary(BinaryExpr *e, std::optional<Type> expected);
  Type checkUnary(UnaryExpr *e, std::optional<Type> expected);
  Type checkCall(CallExpr *e, std::optional<Type> expected);
  Type checkIf(IfExpr *e, std::optional<Type> expected);
  Type checkFor(ForExpr *e);
  Type checkCast(CastExpr *e);
  Type checkStructLit(StructLitExpr *e, std::optional<Type> expected);
  Type checkField(FieldExpr *e);
  Type checkTupleLit(TupleLitExpr *e, std::optional<Type> expected);
  Type checkTupleIndex(TupleIndexExpr *e);
  Type checkMatch(MatchExpr *e, std::optional<Type> expected);
  Type checkBorrow(BorrowExpr *e, std::optional<Type> expected);
  Type checkDeref(DerefExpr *e);
  Type checkBlock(BlockExpr *e, std::optional<Type> expected);
  Type checkAssign(AssignExpr *e);
  Type checkArrayLit(ArrayLitExpr *e, std::optional<Type> expected);
  Type checkIndex(IndexExpr *e);
  Type checkMethodCall(MethodCallExpr *e);
  Type checkReturn(ReturnExpr *e);
  Type checkTry(TryExpr *e);
  // 代入先 / &mut で借用できる「可変な場所」か
  bool isMutablePlace(const Expr *e);

  void checkFunction(FunctionDef &f);
  const StructDef *findStruct(const std::string &name) const;
  const EnumDef *findEnum(const std::string &name) const;
  // パーサは未知の名前型を Struct 種として作る。enum 名なら Enum 種へ直す。
  Type resolve(Type t) const;
  // ジェネリック支援
  Type bindParams(Type t, const std::set<std::string> &params) const; // 名前→Param
  Type substType(const Type &t, const std::map<std::string, Type> &subst) const;
  void unifyParam(const Type &decl, const Type &actual,
                  std::map<std::string, Type> &subst) const; // 型引数を推論
  static bool hasParam(const Type &t); // 未解決の Param を含むか
  // 検査中の関数の型引数 (activeTypeParams_) でない Param を含むか。
  // = 「解くべき推論変数」(値から決める) かどうかの判定。
  bool hasNonActiveParam(const Type &t) const;

  struct Local {
    Type type;
    bool isMut = false;
  };

  DiagnosticEngine &diag_;
  std::map<std::string, FuncSig> funcs_;
  std::map<std::string, const Prototype *> genericFuncs_; // ジェネリック関数
  std::map<std::string, Local> locals_;
  std::map<std::string, const StructDef *> structs_;
  std::map<std::string, const EnumDef *> enums_;
  std::map<std::string, VariantInfo> variants_;
  // 型名 → メソッド名 → メソッド定義 (proto.args[0]="self")
  std::map<std::string, std::map<std::string, const FunctionDef *>> methods_;
  // 型名 → 関連関数名 → 定義 (self なし。Type::name() で呼ぶ)
  std::map<std::string, std::map<std::string, const FunctionDef *>> assocFns_;
  std::map<std::string, const TraitDef *> traits_; // トレイト名 → 定義
  std::set<std::pair<std::string, std::string>> traitImpls_; // (トレイト, 型)
  std::set<std::string> activeTypeParams_; // 検査中の関数の型引数 (resolve 用)
  std::optional<Type> currentRetType_;     // 検査中の関数の戻り値型 (return/? 用)
  // 検査中の関数の型引数の境界: 型引数名 → トレイト名の集合
  std::map<std::string, std::vector<std::string>> paramBounds_;
};

} // namespace kal
