//===- Token.h - token kinds produced by the lexer ------------------------===//
#pragma once

#include "kal/SourceManager.h"
#include <string>

namespace kal {

enum class Tok {
  Eof,
  // キーワード
  Fn,
  Extern,
  If,
  Then,
  Else,
  For,
  In,
  As,
  Struct,
  Let,
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
  Arrow, // ->
  Colon, // :
  Dot,   // .
  LParen,
  RParen,
  LBrace, // {
  RBrace, // }
  Comma,
  Semicolon,
  Unknown,
};

struct Token {
  Tok kind = Tok::Eof;
  Span span;
  std::string text;        // Identifier のとき有効
  bool isFloat = false;    // Number が小数リテラルか
  double floatValue = 0;   // Number(小数) のとき有効
  uint64_t intValue = 0;   // Number(整数) のとき有効
};

/// 二項演算子の優先順位。二項演算子でなければ -1。
int binPrecedence(Tok kind);

} // namespace kal
