//===- Type.h - the Kal type system ---------------------------------------===//
#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>
#include <vector>

namespace kal {

/// Kal の型。整数・浮動小数点・bool・unit に加え、struct (公称) と tuple (構造的)。
struct Type {
  enum class Kind { Unknown, Unit, Bool, Int, Float, Struct, Tuple };
  Kind kind = Kind::Unknown;
  unsigned bits = 0;        // Int: 8/16/32/64, Float: 32/64, Bool: 1
  bool isSigned = true;     // Int のみ
  std::string name;         // Struct の名前
  std::vector<Type> elems;  // Tuple の要素型

  static Type unknown() { return {}; }
  static Type unit() { return {Kind::Unit, 0, true}; }
  static Type boolean() { return {Kind::Bool, 1, false}; }
  static Type intTy(unsigned b, bool s) { return {Kind::Int, b, s}; }
  static Type floatTy(unsigned b) { return {Kind::Float, b, true}; }
  static Type structTy(std::string n) {
    Type t;
    t.kind = Kind::Struct;
    t.name = std::move(n);
    return t;
  }
  static Type tupleTy(std::vector<Type> e) {
    Type t;
    t.kind = Kind::Tuple;
    t.elems = std::move(e);
    return t;
  }

  bool isInt() const { return kind == Kind::Int; }
  bool isFloat() const { return kind == Kind::Float; }
  bool isBool() const { return kind == Kind::Bool; }
  bool isUnit() const { return kind == Kind::Unit; }
  bool isStruct() const { return kind == Kind::Struct; }
  bool isTuple() const { return kind == Kind::Tuple; }
  bool isNumeric() const { return isInt() || isFloat(); }
  bool isKnown() const { return kind != Kind::Unknown; }

  bool operator==(const Type &o) const {
    if (kind != o.kind)
      return false;
    switch (kind) {
    case Kind::Int:
      return bits == o.bits && isSigned == o.isSigned;
    case Kind::Float:
      return bits == o.bits;
    case Kind::Struct:
      return name == o.name; // 公称型: 名前で同一性判定
    case Kind::Tuple:
      return elems == o.elems; // 構造的
    default:
      return true;
    }
  }
  bool operator!=(const Type &o) const { return !(*this == o); }

  std::string str() const; // "i32", "f64", "bool", "()", "Point", "(i64, f64)"
};

/// 組み込み型名 ("i32", "f64", "bool" ...) を Type に変換する。未知なら false。
/// (struct 名や tuple はパーサ側で扱う)
bool typeFromName(llvm::StringRef name, Type &out);

} // namespace kal
