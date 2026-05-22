//===- Parser.cpp ----------------------------------------------------------===//
#include "kal/Parser.h"

#include "kal/Diagnostic.h"
#include "kal/Lexer.h"

using namespace kal;

int kal::binPrecedence(Tok kind) {
  switch (kind) {
  case Tok::Less:
  case Tok::Greater:
    return 10;
  case Tok::Plus:
  case Tok::Minus:
    return 20;
  case Tok::Star:
  case Tok::Slash:
    return 40;
  default:
    return -1;
  }
}

Parser::Parser(Lexer &lexer, DiagnosticEngine &diag)
    : lexer_(lexer), diag_(diag) {
  advance();
}

void Parser::advance() { cur_ = lexer_.next(); }

void Parser::recover() {
  while (cur_.kind != Tok::Semicolon && cur_.kind != Tok::Eof)
    advance();
  if (cur_.kind == Tok::Semicolon)
    advance();
}

ExprPtr Parser::parseNumberExpr() {
  auto e = std::make_unique<NumberExpr>(cur_.span, cur_.isFloat, cur_.floatValue,
                                        cur_.intValue);
  advance();
  return e;
}

bool Parser::parseType(Type &out) {
  if (cur_.kind != Tok::Identifier) {
    diag_.error(cur_.span, "E0030", "型名が必要です");
    return false;
  }
  if (!typeFromName(cur_.text, out)) {
    diag_.error(cur_.span, "E0031", "未知の型名です");
    return false;
  }
  advance();
  return true;
}

ExprPtr Parser::parseParenExpr() {
  advance(); // '('
  auto v = parseExpression();
  if (!v)
    return nullptr;
  if (cur_.kind != Tok::RParen) {
    diag_.error(cur_.span, "E0010", "')' が必要です");
    return nullptr;
  }
  advance(); // ')'
  return v;
}

ExprPtr Parser::parseIdentifierExpr() {
  Span idSpan = cur_.span;
  std::string name = cur_.text;
  advance();

  if (cur_.kind != Tok::LParen) // 変数参照
    return std::make_unique<VariableExpr>(idSpan, std::move(name));

  advance(); // '('
  std::vector<ExprPtr> args;
  if (cur_.kind != Tok::RParen) {
    for (;;) {
      auto a = parseExpression();
      if (!a)
        return nullptr;
      args.push_back(std::move(a));
      if (cur_.kind == Tok::RParen)
        break;
      if (cur_.kind != Tok::Comma) {
        diag_.error(cur_.span, "E0011", "引数リストには ',' か ')' が必要です");
        return nullptr;
      }
      advance();
    }
  }
  Span end = cur_.span;
  advance(); // ')'
  Span full{idSpan.fileId, idSpan.start, end.end};
  return std::make_unique<CallExpr>(full, std::move(name), idSpan,
                                    std::move(args));
}

ExprPtr Parser::parseIfExpr() {
  Span s = cur_.span;
  advance(); // 'if'
  auto cond = parseExpression();
  if (!cond)
    return nullptr;
  if (cur_.kind != Tok::Then) {
    diag_.error(cur_.span, "E0012", "'then' が必要です");
    return nullptr;
  }
  advance();
  auto then = parseExpression();
  if (!then)
    return nullptr;
  if (cur_.kind != Tok::Else) {
    diag_.error(cur_.span, "E0013", "'else' が必要です");
    return nullptr;
  }
  advance();
  auto els = parseExpression();
  if (!els)
    return nullptr;
  return std::make_unique<IfExpr>(s, std::move(cond), std::move(then),
                                  std::move(els));
}

ExprPtr Parser::parseForExpr() {
  Span s = cur_.span;
  advance(); // 'for'
  if (cur_.kind != Tok::Identifier) {
    diag_.error(cur_.span, "E0014", "'for' の後にはループ変数名が必要です");
    return nullptr;
  }
  std::string var = cur_.text;
  advance();
  if (cur_.kind != Tok::Equal) {
    diag_.error(cur_.span, "E0015", "ループ変数の後に '=' が必要です");
    return nullptr;
  }
  advance();
  auto start = parseExpression();
  if (!start)
    return nullptr;
  if (cur_.kind != Tok::Comma) {
    diag_.error(cur_.span, "E0016", "開始値の後に ',' が必要です");
    return nullptr;
  }
  advance();
  auto cond = parseExpression();
  if (!cond)
    return nullptr;
  ExprPtr step;
  if (cur_.kind == Tok::Comma) {
    advance();
    step = parseExpression();
    if (!step)
      return nullptr;
  }
  if (cur_.kind != Tok::In) {
    diag_.error(cur_.span, "E0017", "ループ条件の後に 'in' が必要です");
    return nullptr;
  }
  advance();
  auto body = parseExpression();
  if (!body)
    return nullptr;
  return std::make_unique<ForExpr>(s, std::move(var), std::move(start),
                                   std::move(cond), std::move(step),
                                   std::move(body));
}

ExprPtr Parser::parsePrimary() {
  switch (cur_.kind) {
  case Tok::Identifier:
    return parseIdentifierExpr();
  case Tok::Number:
    return parseNumberExpr();
  case Tok::LParen:
    return parseParenExpr();
  case Tok::If:
    return parseIfExpr();
  case Tok::For:
    return parseForExpr();
  default:
    diag_.error(cur_.span, "E0002", "式が必要です");
    return nullptr;
  }
}

ExprPtr Parser::parseUnary() {
  auto e = parsePrimary();
  if (!e)
    return nullptr;
  // 後置キャスト: `expr as Type` (二項演算子より強く結合)
  while (cur_.kind == Tok::As) {
    advance();
    Span typeSpan = cur_.span;
    Type t;
    if (!parseType(t))
      return nullptr;
    Span full{e->span.fileId, e->span.start, typeSpan.end};
    e = std::make_unique<CastExpr>(full, std::move(e), t);
  }
  return e;
}

ExprPtr Parser::parseBinOpRHS(int exprPrec, ExprPtr lhs) {
  for (;;) {
    int prec = binPrecedence(cur_.kind);
    if (prec < exprPrec)
      return lhs;

    Tok op = cur_.kind;
    Span opSpan = cur_.span;
    advance();

    auto rhs = parseUnary();
    if (!rhs)
      return nullptr;

    int nextPrec = binPrecedence(cur_.kind);
    if (prec < nextPrec) {
      rhs = parseBinOpRHS(prec + 1, std::move(rhs));
      if (!rhs)
        return nullptr;
    }

    Span full{lhs->span.fileId, lhs->span.start, rhs->span.end};
    lhs = std::make_unique<BinaryExpr>(full, op, opSpan, std::move(lhs),
                                       std::move(rhs));
  }
}

ExprPtr Parser::parseExpression() {
  auto lhs = parseUnary();
  if (!lhs)
    return nullptr;
  return parseBinOpRHS(0, std::move(lhs));
}

std::unique_ptr<Prototype> Parser::parsePrototype() {
  if (cur_.kind != Tok::Identifier) {
    diag_.error(cur_.span, "E0020", "関数名が必要です");
    return nullptr;
  }
  Span nameSpan = cur_.span;
  std::string name = cur_.text;
  advance();

  if (cur_.kind != Tok::LParen) {
    diag_.error(cur_.span, "E0021", "プロトタイプには '(' が必要です");
    return nullptr;
  }
  advance();

  // 引数:  name: Type, name: Type, ...
  std::vector<std::string> args;
  std::vector<Type> paramTypes;
  if (cur_.kind != Tok::RParen) {
    for (;;) {
      if (cur_.kind != Tok::Identifier) {
        diag_.error(cur_.span, "E0023", "引数名が必要です");
        return nullptr;
      }
      std::string pname = cur_.text;
      advance();
      if (cur_.kind != Tok::Colon) {
        diag_.error(cur_.span, "E0024", "引数には ': 型' の注釈が必要です");
        return nullptr;
      }
      advance();
      Type pt;
      if (!parseType(pt))
        return nullptr;
      args.push_back(std::move(pname));
      paramTypes.push_back(pt);
      if (cur_.kind == Tok::RParen)
        break;
      if (cur_.kind != Tok::Comma) {
        diag_.error(cur_.span, "E0025", "引数リストには ',' か ')' が必要です");
        return nullptr;
      }
      advance();
    }
  }
  Span end = cur_.span;
  advance(); // ')'

  // 戻り値型:  -> Type  (省略時は unit)
  Type retType = Type::unit();
  if (cur_.kind == Tok::Arrow) {
    advance();
    if (!parseType(retType))
      return nullptr;
  }

  auto p = std::make_unique<Prototype>();
  p->name = std::move(name);
  p->nameSpan = nameSpan;
  p->args = std::move(args);
  p->paramTypes = std::move(paramTypes);
  p->retType = retType;
  p->span = {nameSpan.fileId, nameSpan.start, end.end};
  return p;
}

std::unique_ptr<FunctionDef> Parser::parseDefinition() {
  advance(); // 'fn'
  auto proto = parsePrototype();
  if (!proto)
    return nullptr;
  if (cur_.kind != Tok::Equal) {
    diag_.error(cur_.span, "E0026", "関数本体の前に '=' が必要です");
    return nullptr;
  }
  advance();
  auto body = parseExpression();
  if (!body)
    return nullptr;
  auto f = std::make_unique<FunctionDef>();
  f->proto = std::move(proto);
  f->body = std::move(body);
  return f;
}

std::unique_ptr<Prototype> Parser::parseExtern() {
  advance(); // 'extern'
  return parsePrototype();
}

Program Parser::parseProgram() {
  Program prog;
  for (;;) {
    if (cur_.kind == Tok::Eof)
      break;
    if (cur_.kind == Tok::Semicolon) {
      advance();
      continue;
    }
    if (cur_.kind == Tok::Fn) {
      if (auto f = parseDefinition())
        prog.functions.push_back(std::move(f));
      else
        recover();
    } else if (cur_.kind == Tok::Extern) {
      if (auto e = parseExtern())
        prog.externs.push_back(std::move(e));
      else
        recover();
    } else {
      if (auto e = parseExpression())
        prog.topExprs.push_back(std::move(e));
      else
        recover();
    }
  }
  return prog;
}
