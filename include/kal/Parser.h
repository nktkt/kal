//===- Parser.h - recursive descent + operator precedence -----------------===//
#pragma once

#include "kal/AST.h"
#include "kal/Token.h"

namespace kal {

class Lexer;
class DiagnosticEngine;

class Parser {
public:
  Parser(Lexer &lexer, DiagnosticEngine &diag);

  /// トップレベル項目をすべてパースして Program を返す。
  /// エラーは DiagnosticEngine に報告される (diag.numErrors() で確認)。
  Program parseProgram();

private:
  void advance();
  void recover(); // エラー後 ';' まで読み飛ばして再同期

  ExprPtr parseExpression();
  ExprPtr parseUnary(); // primary に後置 `as` キャストを付ける
  ExprPtr parsePrimary();
  ExprPtr parseBinOpRHS(int exprPrec, ExprPtr lhs);
  ExprPtr parseNumberExpr();
  ExprPtr parseParenExpr();
  ExprPtr parseIdentifierExpr();
  ExprPtr parseIfExpr();
  ExprPtr parseForExpr();
  ExprPtr parseMatchExpr();
  ExprPtr parseArrayExpr();
  ExprPtr parseBlock();

  bool parseType(Type &out); // 型を 1 つ読む (組み込み型/struct名/タプル)

  std::unique_ptr<Prototype> parsePrototype();
  std::unique_ptr<FunctionDef> parseDefinition();
  std::unique_ptr<Prototype> parseExtern();
  std::unique_ptr<StructDef> parseStructDef();
  std::unique_ptr<EnumDef> parseEnumDef();
  std::unique_ptr<ImplBlock> parseImplBlock();

  Lexer &lexer_;
  DiagnosticEngine &diag_;
  Token cur_;
  // match の対象などで、識別子直後の '{' を構造体リテラルと解釈しない
  // (`match s { ... }` の '{' を奪わないため)。括弧/引数の中では解除する。
  bool noStructLit_ = false;
};

} // namespace kal
