//===- Type.h - the Kal type system ---------------------------------------===//
#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace kal {

/// Kal の型。整数 (符号付き/なし・幅)、浮動小数点、bool、unit を表す。
struct Type {
  enum class Kind { Unknown, Unit, Bool, Int, Float };
  Kind kind = Kind::Unknown;
  unsigned bits = 0;    // Int: 8/16/32/64, Float: 32/64, Bool: 1
  bool isSigned = true; // Int のみ意味を持つ

  static Type unknown() { return {}; }
  static Type unit() { return {Kind::Unit, 0, true}; }
  static Type boolean() { return {Kind::Bool, 1, false}; }
  static Type intTy(unsigned b, bool s) { return {Kind::Int, b, s}; }
  static Type floatTy(unsigned b) { return {Kind::Float, b, true}; }

  bool isInt() const { return kind == Kind::Int; }
  bool isFloat() const { return kind == Kind::Float; }
  bool isBool() const { return kind == Kind::Bool; }
  bool isUnit() const { return kind == Kind::Unit; }
  bool isNumeric() const { return isInt() || isFloat(); }
  bool isKnown() const { return kind != Kind::Unknown; }

  bool operator==(const Type &o) const {
    if (kind != o.kind)
      return false;
    if (kind == Kind::Int)
      return bits == o.bits && isSigned == o.isSigned;
    if (kind == Kind::Float)
      return bits == o.bits;
    return true;
  }
  bool operator!=(const Type &o) const { return !(*this == o); }

  std::string str() const; // "i32", "u64", "f64", "bool", "()"
};

/// 型名 ("i32", "f64", "bool" ...) を Type に変換する。未知なら false。
bool typeFromName(llvm::StringRef name, Type &out);

} // namespace kal
