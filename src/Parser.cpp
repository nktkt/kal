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
  // タプル型 / unit / 括弧:  ()  (T)  (T1, T2, ...)
  if (cur_.kind == Tok::LParen) {
    advance();
    if (cur_.kind == Tok::RParen) {
      advance();
      out = Type::unit();
      return true;
    }
    std::vector<Type> elems;
    for (;;) {
      Type e;
      if (!parseType(e))
        return false;
      elems.push_back(e);
      if (cur_.kind == Tok::RParen)
        break;
      if (cur_.kind != Tok::Comma) {
        diag_.error(cur_.span, "E0032", "型リストには ',' か ')' が必要です");
        return false;
      }
      advance();
    }
    advance(); // ')'
    out = elems.size() == 1 ? elems[0] : Type::tupleTy(std::move(elems));
    return true;
  }
  if (cur_.kind != Tok::Identifier) {
    diag_.error(cur_.span, "E0030", "型が必要です");
    return false;
  }
  // 組み込み型名、なければ struct 名 (存在チェックは Sema)
  if (typeFromName(cur_.text, out)) {
    advance();
    return true;
  }
  out = Type::structTy(cur_.text);
  advance();
  return true;
}

ExprPtr Parser::parseParenExpr() {
  Span s = cur_.span;
  advance(); // '('
  auto first = parseExpression();
  if (!first)
    return nullptr;

  if (cur_.kind == Tok::Comma) {
    // タプルリテラル
    std::vector<ExprPtr> elems;
    elems.push_back(std::move(first));
    while (cur_.kind == Tok::Comma) {
      advance();
      if (cur_.kind == Tok::RParen)
        break; // 末尾カンマを許可
      auto e = parseExpression();
      if (!e)
        return nullptr;
      elems.push_back(std::move(e));
    }
    if (cur_.kind != Tok::RParen) {
      diag_.error(cur_.span, "E0010", "')' が必要です");
      return nullptr;
    }
    Span end = cur_.span;
    advance();
    return std::make_unique<TupleLitExpr>(Span{s.fileId, s.start, end.end},
                                          std::move(elems));
  }

  if (cur_.kind != Tok::RParen) {
    diag_.error(cur_.span, "E0010", "')' が必要です");
    return nullptr;
  }
  advance(); // ')'
  return first;
}

ExprPtr Parser::parseIdentifierExpr() {
  Span idSpan = cur_.span;
  std::string name = cur_.text;
  advance();

  // 構造体リテラル:  Name { f: e, ... }
  if (cur_.kind == Tok::LBrace) {
    advance();
    auto lit = std::make_unique<StructLitExpr>(idSpan, name, idSpan);
    if (cur_.kind != Tok::RBrace) {
      for (;;) {
        if (cur_.kind != Tok::Identifier) {
          diag_.error(cur_.span, "E0053", "フィールド名が必要です");
          return nullptr;
        }
        std::string fname = cur_.text;
        advance();
        if (cur_.kind != Tok::Colon) {
          diag_.error(cur_.span, "E0054", "フィールドには ': 値' が必要です");
          return nullptr;
        }
        advance();
        auto fv = parseExpression();
        if (!fv)
          return nullptr;
        lit->fieldNames.push_back(std::move(fname));
        lit->fieldValues.push_back(std::move(fv));
        if (cur_.kind == Tok::RBrace)
          break;
        if (cur_.kind != Tok::Comma) {
          diag_.error(cur_.span, "E0055", "フィールドの後には ',' か '}' が必要です");
          return nullptr;
        }
        advance();
      }
    }
    Span end = cur_.span;
    advance(); // '}'
    lit->span = {idSpan.fileId, idSpan.start, end.end};
    return lit;
  }

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
  case Tok::Let:
    return parseLetExpr();
  default:
    diag_.error(cur_.span, "E0002", "式が必要です");
    return nullptr;
  }
}

ExprPtr Parser::parseUnary() {
  auto e = parsePrimary();
  if (!e)
    return nullptr;
  // 後置: フィールド/タプルアクセス `.x` `.0` とキャスト `as T`
  // (いずれも二項演算子より強く結合する)
  for (;;) {
    if (cur_.kind == Tok::Dot) {
      advance();
      if (cur_.kind == Tok::Identifier) {
        Span fspan = cur_.span;
        std::string fname = cur_.text;
        advance();
        Span full{e->span.fileId, e->span.start, fspan.end};
        e = std::make_unique<FieldExpr>(full, std::move(e), std::move(fname),
                                        fspan);
      } else if (cur_.kind == Tok::Number && !cur_.isFloat) {
        Span ispan = cur_.span;
        unsigned idx = static_cast<unsigned>(cur_.intValue);
        advance();
        Span full{e->span.fileId, e->span.start, ispan.end};
        e = std::make_unique<TupleIndexExpr>(full, std::move(e), idx, ispan);
      } else {
        diag_.error(cur_.span, "E0056",
                    "'.' の後にはフィールド名かタプル番号が必要です");
        return nullptr;
      }
    } else if (cur_.kind == Tok::As) {
      advance();
      Span typeSpan = cur_.span;
      Type t;
      if (!parseType(t))
        return nullptr;
      Span full{e->span.fileId, e->span.start, typeSpan.end};
      e = std::make_unique<CastExpr>(full, std::move(e), t);
    } else {
      break;
    }
  }
  return e;
}

ExprPtr Parser::parseLetExpr() {
  Span s = cur_.span;
  advance(); // 'let'
  if (cur_.kind != Tok::Identifier) {
    diag_.error(cur_.span, "E0050", "let の後には変数名が必要です");
    return nullptr;
  }
  std::string name = cur_.text;
  advance();
  if (cur_.kind != Tok::Equal) {
    diag_.error(cur_.span, "E0051", "let には '=' が必要です");
    return nullptr;
  }
  advance();
  auto value = parseExpression();
  if (!value)
    return nullptr;
  if (cur_.kind != Tok::In) {
    diag_.error(cur_.span, "E0052", "let には 'in' が必要です");
    return nullptr;
  }
  advance();
  auto body = parseExpression();
  if (!body)
    return nullptr;
  return std::make_unique<LetExpr>(s, std::move(name), std::move(value),
                                   std::move(body));
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

std::unique_ptr<StructDef> Parser::parseStructDef() {
  Span start = cur_.span;
  advance(); // 'struct'
  if (cur_.kind != Tok::Identifier) {
    diag_.error(cur_.span, "E0040", "構造体名が必要です");
    return nullptr;
  }
  auto sd = std::make_unique<StructDef>();
  sd->name = cur_.text;
  sd->nameSpan = cur_.span;
  advance();

  if (cur_.kind != Tok::LBrace) {
    diag_.error(cur_.span, "E0041", "構造体には '{' が必要です");
    return nullptr;
  }
  advance();

  while (cur_.kind != Tok::RBrace) {
    if (cur_.kind != Tok::Identifier) {
      diag_.error(cur_.span, "E0042", "フィールド名が必要です");
      return nullptr;
    }
    StructField f;
    f.name = cur_.text;
    f.span = cur_.span;
    advance();
    if (cur_.kind != Tok::Colon) {
      diag_.error(cur_.span, "E0043", "フィールドには ': 型' が必要です");
      return nullptr;
    }
    advance();
    if (!parseType(f.type))
      return nullptr;
    sd->fields.push_back(std::move(f));
    if (cur_.kind != Tok::Comma)
      break;
    advance(); // ',' (末尾カンマ可)
  }
  if (cur_.kind != Tok::RBrace) {
    diag_.error(cur_.span, "E0044", "構造体には '}' が必要です");
    return nullptr;
  }
  Span end = cur_.span;
  advance(); // '}'
  sd->span = {start.fileId, start.start, end.end};
  return sd;
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
    if (cur_.kind == Tok::Struct) {
      if (auto sd = parseStructDef())
        prog.structs.push_back(std::move(sd));
      else
        recover();
    } else if (cur_.kind == Tok::Fn) {
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
