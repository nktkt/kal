//===- Parser.cpp ----------------------------------------------------------===//
#include "kal/Parser.h"

#include "kal/Diagnostic.h"
#include "kal/Lexer.h"

using namespace kal;

int kal::binPrecedence(Tok kind) {
  switch (kind) {
  case Tok::PipePipe:
    return 4;
  case Tok::AmpAmp:
    return 6;
  case Tok::Less:
  case Tok::Greater:
  case Tok::Le:
  case Tok::Ge:
  case Tok::EqEq:
  case Tok::BangEq:
    return 10;
  case Tok::Plus:
  case Tok::Minus:
    return 20;
  case Tok::Star:
  case Tok::Slash:
  case Tok::Percent:
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
  // 参照型:  &T  /  &mut T
  if (cur_.kind == Tok::Amp) {
    advance();
    bool mut = false;
    if (cur_.kind == Tok::Mut) {
      mut = true;
      advance();
    }
    // スライス型:  &[T]  /  &mut [T]
    if (cur_.kind == Tok::LBracket) {
      advance();
      Type elem;
      if (!parseType(elem))
        return false;
      if (cur_.kind != Tok::RBracket) {
        diag_.error(cur_.span, "E0038",
                    "スライス型は '&[T]' の形式です (']' が必要)");
        return false;
      }
      advance();
      out = Type::sliceTy(elem, mut);
      return true;
    }
    Type pointee;
    if (!parseType(pointee))
      return false;
    out = Type::refTy(pointee, mut);
    return true;
  }
  // 配列型:  [T; N]
  if (cur_.kind == Tok::LBracket) {
    advance();
    Type elem;
    if (!parseType(elem))
      return false;
    if (cur_.kind != Tok::Semicolon) {
      diag_.error(cur_.span, "E0033", "配列型は '[T; N]' の形式です (';' が必要)");
      return false;
    }
    advance();
    if (cur_.kind != Tok::Number || cur_.isFloat) {
      diag_.error(cur_.span, "E0034", "配列の長さには整数リテラルが必要です");
      return false;
    }
    unsigned len = static_cast<unsigned>(cur_.intValue);
    advance();
    if (cur_.kind != Tok::RBracket) {
      diag_.error(cur_.span, "E0035", "配列型には ']' が必要です");
      return false;
    }
    advance();
    out = Type::arrayTy(elem, len);
    return true;
  }
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
  // 組み込み型名、なければ struct/enum 名 (存在チェックは Sema)
  if (typeFromName(cur_.text, out)) {
    advance();
    return true;
  }
  out = Type::structTy(cur_.text);
  advance();
  // 型引数:  Name<T1, T2, ...>  (ジェネリック具体化。enum 名解決は Sema)
  if (cur_.kind == Tok::Less) {
    advance();
    std::vector<Type> args;
    for (;;) {
      Type a;
      if (!parseType(a))
        return false;
      args.push_back(std::move(a));
      if (cur_.kind == Tok::Greater)
        break;
      if (cur_.kind != Tok::Comma) {
        diag_.error(cur_.span, "E0039", "型引数には ',' か '>' が必要です");
        return false;
      }
      advance();
    }
    advance(); // '>'
    out.elems = std::move(args);
  }
  return true;
}

ExprPtr Parser::parseParenExpr() {
  Span s = cur_.span;
  advance(); // '('
  bool savedNSL = noStructLit_;
  noStructLit_ = false; // 括弧の中では構造体リテラル可 (末尾で復元)
  auto first = parseExpression();
  if (!first) {
    noStructLit_ = savedNSL;
    return nullptr;
  }

  if (cur_.kind == Tok::Comma) {
    // タプルリテラル
    std::vector<ExprPtr> elems;
    elems.push_back(std::move(first));
    while (cur_.kind == Tok::Comma) {
      advance();
      if (cur_.kind == Tok::RParen)
        break; // 末尾カンマを許可
      auto e = parseExpression();
      if (!e) {
        noStructLit_ = savedNSL;
        return nullptr;
      }
      elems.push_back(std::move(e));
    }
    if (cur_.kind != Tok::RParen) {
      diag_.error(cur_.span, "E0010", "')' が必要です");
      noStructLit_ = savedNSL;
      return nullptr;
    }
    Span end = cur_.span;
    advance();
    noStructLit_ = savedNSL;
    return std::make_unique<TupleLitExpr>(Span{s.fileId, s.start, end.end},
                                          std::move(elems));
  }

  if (cur_.kind != Tok::RParen) {
    diag_.error(cur_.span, "E0010", "')' が必要です");
    noStructLit_ = savedNSL;
    return nullptr;
  }
  advance(); // ')'
  noStructLit_ = savedNSL;
  return first;
}

ExprPtr Parser::parseIdentifierExpr() {
  Span idSpan = cur_.span;
  std::string name = cur_.text;
  advance();

  // 構造体リテラル:  Name { f: e, ... }
  // (noStructLit_ 中 = match の対象式などでは '{' を奪わない)
  if (cur_.kind == Tok::LBrace && !noStructLit_) {
    advance();
    auto lit = std::make_unique<StructLitExpr>(idSpan, name, idSpan);
    bool savedNSL = noStructLit_;
    noStructLit_ = false; // フィールド値の中では構造体リテラル可
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
    noStructLit_ = savedNSL;
    lit->span = {idSpan.fileId, idSpan.start, end.end};
    return lit;
  }

  if (cur_.kind != Tok::LParen) // 変数参照
    return std::make_unique<VariableExpr>(idSpan, std::move(name));

  advance(); // '('
  bool savedNSLcall = noStructLit_;
  noStructLit_ = false; // 引数の中では構造体リテラル可
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
  noStructLit_ = savedNSLcall;
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
  case Tok::Match:
    return parseMatchExpr();
  case Tok::LBracket:
    return parseArrayExpr();
  case Tok::LBrace:
    return parseBlock();
  default:
    diag_.error(cur_.span, "E0002", "式が必要です");
    return nullptr;
  }
}

ExprPtr Parser::parseUnary() {
  // 前置: 借用 `&` `&mut`、参照外し `*`
  if (cur_.kind == Tok::Amp) {
    Span s = cur_.span;
    advance();
    bool mut = false;
    if (cur_.kind == Tok::Mut) {
      mut = true;
      advance();
    }
    auto operand = parseUnary();
    if (!operand)
      return nullptr;
    Span full{s.fileId, s.start, operand->span.end};
    return std::make_unique<BorrowExpr>(full, std::move(operand), mut);
  }
  if (cur_.kind == Tok::Star) {
    Span s = cur_.span;
    advance();
    auto operand = parseUnary();
    if (!operand)
      return nullptr;
    Span full{s.fileId, s.start, operand->span.end};
    return std::make_unique<DerefExpr>(full, std::move(operand));
  }
  if (cur_.kind == Tok::Minus || cur_.kind == Tok::Bang) {
    Tok op = cur_.kind;
    Span s = cur_.span;
    advance();
    auto operand = parseUnary();
    if (!operand)
      return nullptr;
    Span full{s.fileId, s.start, operand->span.end};
    return std::make_unique<UnaryExpr>(full, op, std::move(operand));
  }

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
    } else if (cur_.kind == Tok::LBracket) {
      advance(); // '['
      bool savedNSL = noStructLit_;
      noStructLit_ = false; // 添字の中では構造体リテラル可
      auto idx = parseExpression();
      noStructLit_ = savedNSL;
      if (!idx)
        return nullptr;
      if (cur_.kind != Tok::RBracket) {
        diag_.error(cur_.span, "E0037", "添字アクセスには ']' が必要です");
        return nullptr;
      }
      Span end = cur_.span;
      advance(); // ']'
      Span full{e->span.fileId, e->span.start, end.end};
      e = std::make_unique<IndexExpr>(full, std::move(e), std::move(idx));
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

// ブロック: `{ stmt* tail? }`
//   stmt = `let [mut] name [: T] = e;`  |  `e;`
//   tail = 末尾の `;` なし式 (ブロックの値。なければ unit)
ExprPtr Parser::parseBlock() {
  Span s = cur_.span;
  advance(); // '{'
  bool savedNSL = noStructLit_;
  noStructLit_ = false; // ブロック内では構造体リテラル可
  auto blk = std::make_unique<BlockExpr>(s);

  while (cur_.kind != Tok::RBrace && cur_.kind != Tok::Eof) {
    if (cur_.kind == Tok::Let) {
      Stmt st;
      st.kind = Stmt::Kind::Let;
      st.span = cur_.span;
      advance(); // 'let'
      if (cur_.kind == Tok::Mut) {
        st.isMut = true;
        advance();
      }
      if (cur_.kind != Tok::Identifier) {
        diag_.error(cur_.span, "E0050", "let の後には変数名が必要です");
        noStructLit_ = savedNSL;
        return nullptr;
      }
      st.name = cur_.text;
      st.nameSpan = cur_.span;
      advance();
      if (cur_.kind == Tok::Colon) {
        advance();
        if (!parseType(st.annotatedType)) {
          noStructLit_ = savedNSL;
          return nullptr;
        }
        st.hasAnnotation = true;
      }
      if (cur_.kind != Tok::Equal) {
        diag_.error(cur_.span, "E0051", "let には '=' が必要です");
        noStructLit_ = savedNSL;
        return nullptr;
      }
      advance();
      st.expr = parseExpression();
      if (!st.expr) {
        noStructLit_ = savedNSL;
        return nullptr;
      }
      if (cur_.kind != Tok::Semicolon) {
        diag_.error(cur_.span, "E0057", "let 文の後には ';' が必要です");
        noStructLit_ = savedNSL;
        return nullptr;
      }
      advance();
      blk->stmts.push_back(std::move(st));
    } else {
      auto e = parseExpression();
      if (!e) {
        noStructLit_ = savedNSL;
        return nullptr;
      }
      if (cur_.kind == Tok::Semicolon) {
        advance();
        Stmt st;
        st.kind = Stmt::Kind::Expr;
        st.span = e->span;
        st.expr = std::move(e);
        blk->stmts.push_back(std::move(st));
      } else if (cur_.kind == Tok::RBrace) {
        blk->tail = std::move(e); // 末尾式 = ブロックの値
        break;
      } else {
        diag_.error(cur_.span, "E0058", "式の後には ';' か '}' が必要です");
        noStructLit_ = savedNSL;
        return nullptr;
      }
    }
  }
  noStructLit_ = savedNSL;
  if (cur_.kind != Tok::RBrace) {
    diag_.error(cur_.span, "E0059", "ブロックには '}' が必要です");
    return nullptr;
  }
  Span end = cur_.span;
  advance(); // '}'
  blk->span = {s.fileId, s.start, end.end};
  return blk;
}

ExprPtr Parser::parseMatchExpr() {
  Span s = cur_.span;
  advance(); // 'match'
  bool savedNSL = noStructLit_;
  noStructLit_ = true; // 対象式の直後の '{' は match のブロック
  auto scrut = parseExpression();
  noStructLit_ = savedNSL;
  if (!scrut)
    return nullptr;
  if (cur_.kind != Tok::LBrace) {
    diag_.error(cur_.span, "E0070", "match には '{' が必要です");
    return nullptr;
  }
  advance();

  auto me = std::make_unique<MatchExpr>(s, std::move(scrut));
  while (cur_.kind != Tok::RBrace) {
    MatchArm arm;
    if (cur_.kind != Tok::Identifier) {
      diag_.error(cur_.span, "E0071", "パターンが必要です");
      return nullptr;
    }
    if (cur_.text == "_") {
      arm.isWildcard = true;
      arm.variantSpan = cur_.span;
      advance();
    } else {
      arm.variant = cur_.text;
      arm.variantSpan = cur_.span;
      advance();
      if (cur_.kind == Tok::LParen) { // ペイロード束縛
        advance();
        if (cur_.kind != Tok::RParen) {
          for (;;) {
            if (cur_.kind != Tok::Identifier) {
              diag_.error(cur_.span, "E0072", "束縛名が必要です");
              return nullptr;
            }
            arm.bindings.push_back(cur_.text);
            arm.bindingSpans.push_back(cur_.span);
            advance();
            if (cur_.kind == Tok::RParen)
              break;
            if (cur_.kind != Tok::Comma) {
              diag_.error(cur_.span, "E0073", "束縛には ',' か ')' が必要です");
              return nullptr;
            }
            advance();
          }
        }
        advance(); // ')'
      }
    }
    if (cur_.kind != Tok::FatArrow) {
      diag_.error(cur_.span, "E0074", "パターンの後には '=>' が必要です");
      return nullptr;
    }
    advance();
    auto body = parseExpression();
    if (!body)
      return nullptr;
    arm.body = std::move(body);
    me->arms.push_back(std::move(arm));
    if (cur_.kind != Tok::Comma)
      break;
    advance(); // ',' (末尾カンマ可)
  }
  if (cur_.kind != Tok::RBrace) {
    diag_.error(cur_.span, "E0075", "match には '}' が必要です");
    return nullptr;
  }
  Span end = cur_.span;
  advance(); // '}'
  me->span = {s.fileId, s.start, end.end};
  return me;
}

// 配列リテラル: `[e1, e2, ...]` (末尾カンマ可)
ExprPtr Parser::parseArrayExpr() {
  Span s = cur_.span;
  advance(); // '['
  bool savedNSL = noStructLit_;
  noStructLit_ = false; // 要素の中では構造体リテラル可
  std::vector<ExprPtr> elems;
  if (cur_.kind != Tok::RBracket) {
    for (;;) {
      auto el = parseExpression();
      if (!el) {
        noStructLit_ = savedNSL;
        return nullptr;
      }
      elems.push_back(std::move(el));
      if (cur_.kind == Tok::RBracket)
        break;
      if (cur_.kind != Tok::Comma) {
        diag_.error(cur_.span, "E0036",
                    "配列要素の後には ',' か ']' が必要です");
        noStructLit_ = savedNSL;
        return nullptr;
      }
      advance(); // ','
      if (cur_.kind == Tok::RBracket)
        break; // 末尾カンマを許可
    }
  }
  Span end = cur_.span;
  advance(); // ']'
  noStructLit_ = savedNSL;
  return std::make_unique<ArrayLitExpr>(Span{s.fileId, s.start, end.end},
                                        std::move(elems));
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
  lhs = parseBinOpRHS(0, std::move(lhs));
  if (!lhs)
    return nullptr;
  // 代入 (最も弱い結合・右結合):  place = value
  if (cur_.kind == Tok::Equal) {
    Span eq = cur_.span;
    advance();
    auto rhs = parseExpression();
    if (!rhs)
      return nullptr;
    Span full{lhs->span.fileId, lhs->span.start, rhs->span.end};
    (void)eq;
    return std::make_unique<AssignExpr>(full, std::move(lhs), std::move(rhs));
  }
  return lhs;
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
  // 本体は `{ ブロック }` または `= 式;`
  ExprPtr body;
  if (cur_.kind == Tok::LBrace) {
    body = parseBlock();
  } else {
    if (cur_.kind != Tok::Equal) {
      diag_.error(cur_.span, "E0026", "関数本体の前に '=' か '{' が必要です");
      return nullptr;
    }
    advance();
    body = parseExpression();
  }
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

std::unique_ptr<EnumDef> Parser::parseEnumDef() {
  Span start = cur_.span;
  advance(); // 'enum'
  if (cur_.kind != Tok::Identifier) {
    diag_.error(cur_.span, "E0080", "enum 名が必要です");
    return nullptr;
  }
  auto ed = std::make_unique<EnumDef>();
  ed->name = cur_.text;
  ed->nameSpan = cur_.span;
  advance();

  // 型引数:  <P1, P2, ...>  (省略時は非総称)
  if (cur_.kind == Tok::Less) {
    advance();
    for (;;) {
      if (cur_.kind != Tok::Identifier) {
        diag_.error(cur_.span, "E0087", "型引数名が必要です");
        return nullptr;
      }
      ed->typeParams.push_back(cur_.text);
      advance();
      if (cur_.kind == Tok::Greater)
        break;
      if (cur_.kind != Tok::Comma) {
        diag_.error(cur_.span, "E0088", "型引数には ',' か '>' が必要です");
        return nullptr;
      }
      advance();
    }
    advance(); // '>'
  }

  if (cur_.kind != Tok::LBrace) {
    diag_.error(cur_.span, "E0081", "enum には '{' が必要です");
    return nullptr;
  }
  advance();

  while (cur_.kind != Tok::RBrace) {
    if (cur_.kind != Tok::Identifier) {
      diag_.error(cur_.span, "E0082", "バリアント名が必要です");
      return nullptr;
    }
    EnumVariant v;
    v.name = cur_.text;
    v.span = cur_.span;
    advance();
    if (cur_.kind == Tok::LParen) { // ペイロード型
      advance();
      if (cur_.kind != Tok::RParen) {
        for (;;) {
          Type pt;
          if (!parseType(pt))
            return nullptr;
          v.payloadTypes.push_back(pt);
          if (cur_.kind == Tok::RParen)
            break;
          if (cur_.kind != Tok::Comma) {
            diag_.error(cur_.span, "E0083", "ペイロードには ',' か ')' が必要です");
            return nullptr;
          }
          advance();
        }
      }
      advance(); // ')'
    }
    ed->variants.push_back(std::move(v));
    if (cur_.kind != Tok::Comma)
      break;
    advance(); // ',' (末尾カンマ可)
  }
  if (cur_.kind != Tok::RBrace) {
    diag_.error(cur_.span, "E0084", "enum には '}' が必要です");
    return nullptr;
  }
  Span end = cur_.span;
  advance(); // '}'
  ed->span = {start.fileId, start.start, end.end};
  return ed;
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
    } else if (cur_.kind == Tok::Enum) {
      if (auto ed = parseEnumDef())
        prog.enums.push_back(std::move(ed));
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
