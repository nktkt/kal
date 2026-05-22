//===- Token.h - token kinds produced by the lexer ------------------------===//
#pragma once

#include "kal/SourceManager.h"
#include <string>

namespace kal {

enum class Tok {
  Eof,
  // キーワード
  Def,
  Extern,
  If,
  Then,
  Else,
  For,
  In,
  // 主要トークン
  Identifier,
  Number,
  // 演算子・記号
  Plus,
  Minus,
  Star,
  Slash,
  Less,
  Greater,
  Equal,
  LParen,
  RParen,
  Comma,
  Semicolon,
  Unknown,
};

struct Token {
  Tok kind = Tok::Eof;
  Span span;
  std::string text; // Identifier のとき有効
  double value = 0; // Number のとき有効
};

/// 二項演算子の優先順位。二項演算子でなければ -1。
int binPrecedence(Tok kind);

} // namespace kal
