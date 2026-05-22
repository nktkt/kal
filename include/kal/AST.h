//===- AST.h - pure syntax tree (no codegen coupling) ---------------------===//
#pragma once

#include "kal/Token.h"
#include "kal/Type.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace kal {

/// 式ノードの基底。codegen は持たない (CodeGen が kind で分岐する)。
/// `type` は Sema (型検査) が後から埋める。
struct Expr {
  enum class Kind {
    Number,
    Variable,
    Binary,
    Call,
    If,
    For,
    Cast,
    StructLit,
    Field,
    TupleLit,
    TupleIndex,
    Let,
    Match,
  };
  Kind kind;
  Span span;
  Type type; // Sema が設定する計算結果の型
  Expr(Kind k, Span s) : kind(k), span(s) {}
  virtual ~Expr() = default;
};
using ExprPtr = std::unique_ptr<Expr>;

/// 数値リテラル。整数か小数かを保持し、型は Sema が文脈から決める。
struct NumberExpr : Expr {
  bool isFloat;
  double floatValue;
  uint64_t intValue;
  NumberExpr(Span s, bool isFloat, double fv, uint64_t iv)
      : Expr(Kind::Number, s), isFloat(isFloat), floatValue(fv), intValue(iv) {}
};

/// 型キャスト: `operand as targetType`
struct CastExpr : Expr {
  ExprPtr operand;
  Type targetType;
  CastExpr(Span s, ExprPtr operand, Type targetType)
      : Expr(Kind::Cast, s), operand(std::move(operand)),
        targetType(targetType) {}
};

/// 構造体リテラル: `Name { f1: e1, f2: e2 }` (fieldNames[i] = fieldValues[i])
struct StructLitExpr : Expr {
  std::string structName;
  Span nameSpan;
  std::vector<std::string> fieldNames;
  std::vector<ExprPtr> fieldValues;
  StructLitExpr(Span s, std::string n, Span nameSpan)
      : Expr(Kind::StructLit, s), structName(std::move(n)), nameSpan(nameSpan) {}
};

/// フィールドアクセス: `operand.field`
struct FieldExpr : Expr {
  ExprPtr operand;
  std::string field;
  Span fieldSpan;
  int fieldIndex = -1; // Sema が設定する
  FieldExpr(Span s, ExprPtr operand, std::string field, Span fieldSpan)
      : Expr(Kind::Field, s), operand(std::move(operand)),
        field(std::move(field)), fieldSpan(fieldSpan) {}
};

/// タプルリテラル: `(e1, e2, ...)`
struct TupleLitExpr : Expr {
  std::vector<ExprPtr> elems;
  TupleLitExpr(Span s, std::vector<ExprPtr> elems)
      : Expr(Kind::TupleLit, s), elems(std::move(elems)) {}
};

/// タプル要素アクセス: `operand.N`
struct TupleIndexExpr : Expr {
  ExprPtr operand;
  unsigned index;
  Span indexSpan;
  TupleIndexExpr(Span s, ExprPtr operand, unsigned index, Span indexSpan)
      : Expr(Kind::TupleIndex, s), operand(std::move(operand)), index(index),
        indexSpan(indexSpan) {}
};

/// ローカル束縛 (不変): `let name = value in body`
struct LetExpr : Expr {
  std::string name;
  ExprPtr value;
  ExprPtr body;
  LetExpr(Span s, std::string name, ExprPtr value, ExprPtr body)
      : Expr(Kind::Let, s), name(std::move(name)), value(std::move(value)),
        body(std::move(body)) {}
};

/// match の 1 アーム:  `Variant(b1, b2) => body`  または  `_ => body`
struct MatchArm {
  bool isWildcard = false;
  std::string variant;               // バリアント名 (wildcard 以外)
  Span variantSpan;
  std::vector<std::string> bindings; // ペイロード束縛名 ("_" 可)
  std::vector<Span> bindingSpans;
  ExprPtr body;
  // Sema が解決:
  int tag = -1;
  std::vector<Type> payloadTypes;
};

/// パターンマッチ: `match scrutinee { arm, arm, ... }`
struct MatchExpr : Expr {
  ExprPtr scrutinee;
  std::vector<MatchArm> arms;
  MatchExpr(Span s, ExprPtr scrutinee)
      : Expr(Kind::Match, s), scrutinee(std::move(scrutinee)) {}
};

struct VariableExpr : Expr {
  std::string name;
  // Sema が、これが引数なしの enum バリアントなら設定する
  int variantTag = -1;
  std::string variantEnum;
  VariableExpr(Span s, std::string n)
      : Expr(Kind::Variable, s), name(std::move(n)) {}
};

struct BinaryExpr : Expr {
  Tok op;
  Span opSpan;
  ExprPtr lhs, rhs;
  BinaryExpr(Span s, Tok op, Span opSpan, ExprPtr lhs, ExprPtr rhs)
      : Expr(Kind::Binary, s), op(op), opSpan(opSpan), lhs(std::move(lhs)),
        rhs(std::move(rhs)) {}
};

struct CallExpr : Expr {
  std::string callee;
  Span calleeSpan;
  std::vector<ExprPtr> args;
  // Sema が、これが enum バリアント構築なら設定する
  int variantTag = -1;
  std::string variantEnum;
  CallExpr(Span s, std::string callee, Span calleeSpan,
           std::vector<ExprPtr> args)
      : Expr(Kind::Call, s), callee(std::move(callee)), calleeSpan(calleeSpan),
        args(std::move(args)) {}
};

struct IfExpr : Expr {
  ExprPtr cond, then, els;
  IfExpr(Span s, ExprPtr cond, ExprPtr then, ExprPtr els)
      : Expr(Kind::If, s), cond(std::move(cond)), then(std::move(then)),
        els(std::move(els)) {}
};

struct ForExpr : Expr {
  std::string var;
  ExprPtr start, cond, step, body; // step は省略時 null
  ForExpr(Span s, std::string var, ExprPtr start, ExprPtr cond, ExprPtr step,
          ExprPtr body)
      : Expr(Kind::For, s), var(std::move(var)), start(std::move(start)),
        cond(std::move(cond)), step(std::move(step)), body(std::move(body)) {}
};

/// 関数プロトタイプ (引数名 + 引数型 + 戻り値型)。
struct Prototype {
  std::string name;
  Span nameSpan;
  std::vector<std::string> args;
  std::vector<Type> paramTypes;
  Type retType;
  Span span;
};

/// 関数定義 (プロトタイプ + 本体)。
/// 名前は llvm::Function との衝突を避けて FunctionDef。
struct FunctionDef {
  std::unique_ptr<Prototype> proto;
  ExprPtr body;
};

/// 構造体の 1 フィールド。
struct StructField {
  std::string name;
  Type type;
  Span span;
};

/// 構造体定義: `struct Name { f: T, ... }`
struct StructDef {
  std::string name;
  Span nameSpan;
  std::vector<StructField> fields;
  Span span;
};

/// enum の 1 バリアント: `Name` または `Name(T, ...)`
struct EnumVariant {
  std::string name;
  Span span;
  std::vector<Type> payloadTypes;
};

/// enum 定義 (Rust 流 ADT): `enum Name { V1, V2(T, ...), ... }`
struct EnumDef {
  std::string name;
  Span nameSpan;
  std::vector<EnumVariant> variants;
  Span span;
};

/// 1 つのソースをパースした結果。
struct Program {
  std::vector<std::unique_ptr<StructDef>> structs;
  std::vector<std::unique_ptr<EnumDef>> enums;
  std::vector<std::unique_ptr<Prototype>> externs;
  std::vector<std::unique_ptr<FunctionDef>> functions;
  std::vector<ExprPtr> topExprs;
};

} // namespace kal
