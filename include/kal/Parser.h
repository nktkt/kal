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
  ExprPtr parsePrimary();
  ExprPtr parseBinOpRHS(int exprPrec, ExprPtr lhs);
  ExprPtr parseNumberExpr();
  ExprPtr parseParenExpr();
  ExprPtr parseIdentifierExpr();
  ExprPtr parseIfExpr();
  ExprPtr parseForExpr();

  std::unique_ptr<Prototype> parsePrototype();
  std::unique_ptr<FunctionDef> parseDefinition();
  std::unique_ptr<Prototype> parseExtern();

  Lexer &lexer_;
  DiagnosticEngine &diag_;
  Token cur_;
};

} // namespace kal
