//===- AST.h - pure syntax tree (no codegen coupling) ---------------------===//
#pragma once

#include "kal/Token.h"
#include <memory>
#include <string>
#include <vector>

namespace kal {

/// 式ノードの基底。codegen は持たない (CodeGen が kind で分岐する)。
struct Expr {
  enum class Kind { Number, Variable, Binary, Call, If, For };
  Kind kind;
  Span span;
  Expr(Kind k, Span s) : kind(k), span(s) {}
  virtual ~Expr() = default;
};
using ExprPtr = std::unique_ptr<Expr>;

struct NumberExpr : Expr {
  double value;
  NumberExpr(Span s, double v) : Expr(Kind::Number, s), value(v) {}
};

struct VariableExpr : Expr {
  std::string name;
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

/// 関数プロトタイプ (引数はすべて double)。
struct Prototype {
  std::string name;
  Span nameSpan;
  std::vector<std::string> args;
  Span span;
};

/// 関数定義 (プロトタイプ + 本体)。
/// 名前は llvm::Function との衝突を避けて FunctionDef。
struct FunctionDef {
  std::unique_ptr<Prototype> proto;
  ExprPtr body;
};

/// 1 つのソースをパースした結果。
struct Program {
  std::vector<std::unique_ptr<Prototype>> externs;
  std::vector<std::unique_ptr<FunctionDef>> functions;
  std::vector<ExprPtr> topExprs;
};

} // namespace kal
