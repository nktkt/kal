//===- Type.h - the Kal type system ---------------------------------------===//
#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>
#include <vector>

namespace kal {

/// Kal の型。整数・浮動小数点・bool・unit に加え、struct (公称) と tuple (構造的)。
struct Type {
  enum class Kind {
    Unknown, Unit, Bool, Int, Float, Struct, Tuple, Enum, Ref, Array, Slice,
    Param
  };
  Kind kind = Kind::Unknown;
  unsigned bits = 0;        // Int: 8/16/32/64, Float: 32/64, Bool: 1
  bool isSigned = true;     // Int のみ
  bool refMut = false;      // Ref が &mut か
  unsigned arrayLen = 0;    // Array の要素数
  std::string name;         // Struct / Enum の名前、Param の型変数名
  std::vector<Type> elems;  // Tuple の要素型 / Ref の指す型 / Array の要素型(elems[0])
                            // / Struct・Enum の型引数 (ジェネリック具体化)

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
  static Type enumTy(std::string n) {
    Type t;
    t.kind = Kind::Enum;
    t.name = std::move(n);
    return t;
  }
  static Type enumTy(std::string n, std::vector<Type> args) {
    Type t;
    t.kind = Kind::Enum;
    t.name = std::move(n);
    t.elems = std::move(args); // 型引数 (ジェネリック具体化)
    return t;
  }
  static Type paramTy(std::string n) {
    Type t;
    t.kind = Kind::Param;
    t.name = std::move(n);
    return t;
  }
  static Type refTy(Type pointee, bool mut) {
    Type t;
    t.kind = Kind::Ref;
    t.refMut = mut;
    t.elems.push_back(std::move(pointee));
    return t;
  }
  static Type arrayTy(Type elem, unsigned len) {
    Type t;
    t.kind = Kind::Array;
    t.arrayLen = len;
    t.elems.push_back(std::move(elem));
    return t;
  }
  static Type sliceTy(Type elem, bool mut) {
    Type t;
    t.kind = Kind::Slice;
    t.refMut = mut;
    t.elems.push_back(std::move(elem));
    return t;
  }

  bool isInt() const { return kind == Kind::Int; }
  bool isFloat() const { return kind == Kind::Float; }
  bool isBool() const { return kind == Kind::Bool; }
  bool isUnit() const { return kind == Kind::Unit; }
  bool isStruct() const { return kind == Kind::Struct; }
  bool isTuple() const { return kind == Kind::Tuple; }
  bool isEnum() const { return kind == Kind::Enum; }
  bool isRef() const { return kind == Kind::Ref; }
  bool isArray() const { return kind == Kind::Array; }
  bool isSlice() const { return kind == Kind::Slice; }
  bool isParam() const { return kind == Kind::Param; }
  bool isNumeric() const { return isInt() || isFloat(); }
  bool isKnown() const { return kind != Kind::Unknown; }
  const Type &pointee() const { return elems[0]; }   // Ref のとき有効
  const Type &elemType() const { return elems[0]; }  // Array / Slice のとき有効

  bool operator==(const Type &o) const {
    if (kind != o.kind)
      return false;
    switch (kind) {
    case Kind::Int:
      return bits == o.bits && isSigned == o.isSigned;
    case Kind::Float:
      return bits == o.bits;
    case Kind::Struct:
    case Kind::Enum:
      return name == o.name && elems == o.elems; // 公称型 + 型引数
    case Kind::Param:
      return name == o.name;
    case Kind::Tuple:
      return elems == o.elems; // 構造的
    case Kind::Ref:
      return refMut == o.refMut && elems[0] == o.elems[0];
    case Kind::Array:
      return arrayLen == o.arrayLen && elems[0] == o.elems[0];
    case Kind::Slice:
      return refMut == o.refMut && elems[0] == o.elems[0];
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
