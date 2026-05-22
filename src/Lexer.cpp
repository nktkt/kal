//===- Lexer.cpp -----------------------------------------------------------===//
#include "kal/Lexer.h"

#include "kal/Diagnostic.h"
#include "kal/SourceManager.h"

#include <cctype>
#include <cstdlib>
#include <string>

using namespace kal;

Lexer::Lexer(const SourceManager &sm, DiagnosticEngine &diag, uint32_t fileId)
    : sm_(sm), diag_(diag), fileId_(fileId), buf_(sm.buffer(fileId)) {}

Token Lexer::next() {
  // 空白とコメント (# から行末) を読み飛ばす
  for (;;) {
    while (pos_ < buf_.size() &&
           std::isspace(static_cast<unsigned char>(buf_[pos_])))
      ++pos_;
    if (pos_ < buf_.size() && buf_[pos_] == '#') {
      while (pos_ < buf_.size() && buf_[pos_] != '\n')
        ++pos_;
      continue;
    }
    break;
  }

  Token t;
  uint32_t start = pos_;
  auto make = [&](Tok k) {
    t.kind = k;
    t.span = {fileId_, start, pos_};
    return t;
  };

  if (pos_ >= buf_.size())
    return make(Tok::Eof);

  char c = buf_[pos_];

  // 識別子 / キーワード
  if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
    while (pos_ < buf_.size() &&
           (std::isalnum(static_cast<unsigned char>(buf_[pos_])) ||
            buf_[pos_] == '_'))
      ++pos_;
    llvm::StringRef s = buf_.substr(start, pos_ - start);
    Tok k = Tok::Identifier;
    if (s == "fn")
      k = Tok::Fn;
    else if (s == "extern")
      k = Tok::Extern;
    else if (s == "if")
      k = Tok::If;
    else if (s == "then")
      k = Tok::Then;
    else if (s == "else")
      k = Tok::Else;
    else if (s == "for")
      k = Tok::For;
    else if (s == "in")
      k = Tok::In;
    else if (s == "as")
      k = Tok::As;
    else if (s == "struct")
      k = Tok::Struct;
    else if (s == "let")
      k = Tok::Let;
    t.text = s.str();
    return make(k);
  }

  // 数値リテラル (整数 or 小数)。先頭は必ず数字 ('.' 始まりは認めない →
  // '.' をフィールド/タプルアクセスの Dot として使えるようにする)
  if (std::isdigit(static_cast<unsigned char>(c))) {
    bool isFloat = false;
    while (pos_ < buf_.size() &&
           std::isdigit(static_cast<unsigned char>(buf_[pos_])))
      ++pos_;
    if (pos_ < buf_.size() && buf_[pos_] == '.') {
      isFloat = true;
      ++pos_;
      while (pos_ < buf_.size() &&
             std::isdigit(static_cast<unsigned char>(buf_[pos_])))
        ++pos_;
    }
    if (pos_ < buf_.size() && (buf_[pos_] == 'e' || buf_[pos_] == 'E')) {
      isFloat = true;
      ++pos_;
      if (pos_ < buf_.size() && (buf_[pos_] == '+' || buf_[pos_] == '-'))
        ++pos_;
      while (pos_ < buf_.size() &&
             std::isdigit(static_cast<unsigned char>(buf_[pos_])))
        ++pos_;
    }
    llvm::StringRef s = buf_.substr(start, pos_ - start);
    t.isFloat = isFloat;
    if (isFloat)
      t.floatValue = std::strtod(s.str().c_str(), nullptr);
    else
      t.intValue = std::strtoull(s.str().c_str(), nullptr, 10);
    return make(Tok::Number);
  }

  // 演算子・記号
  ++pos_;
  switch (c) {
  case '+':
    return make(Tok::Plus);
  case '-':
    if (pos_ < buf_.size() && buf_[pos_] == '>') {
      ++pos_;
      return make(Tok::Arrow); // ->
    }
    return make(Tok::Minus);
  case '*':
    return make(Tok::Star);
  case '/':
    return make(Tok::Slash);
  case '<':
    return make(Tok::Less);
  case '>':
    return make(Tok::Greater);
  case '=':
    return make(Tok::Equal);
  case ':':
    return make(Tok::Colon);
  case '.':
    return make(Tok::Dot);
  case '(':
    return make(Tok::LParen);
  case ')':
    return make(Tok::RParen);
  case '{':
    return make(Tok::LBrace);
  case '}':
    return make(Tok::RBrace);
  case ',':
    return make(Tok::Comma);
  case ';':
    return make(Tok::Semicolon);
  default: {
    std::string msg = "不明な文字 '";
    msg += c;
    msg += "' です";
    diag_.error({fileId_, start, pos_}, "E0001", msg);
    return next(); // 読み飛ばして続行
  }
  }
}
