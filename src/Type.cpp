//===- Type.cpp ------------------------------------------------------------===//
#include "kal/Type.h"

using namespace kal;

std::string Type::str() const {
  switch (kind) {
  case Kind::Unknown:
    return "<unknown>";
  case Kind::Unit:
    return "()";
  case Kind::Bool:
    return "bool";
  case Kind::Int:
    return (isSigned ? "i" : "u") + std::to_string(bits);
  case Kind::Float:
    return "f" + std::to_string(bits);
  case Kind::Struct:
  case Kind::Enum: {
    if (elems.empty())
      return name;
    std::string s = name + "<";
    for (size_t i = 0; i < elems.size(); ++i) {
      if (i)
        s += ", ";
      s += elems[i].str();
    }
    return s + ">";
  }
  case Kind::Param:
    return name;
  case Kind::Tuple: {
    std::string s = "(";
    for (size_t i = 0; i < elems.size(); ++i) {
      if (i)
        s += ", ";
      s += elems[i].str();
    }
    return s + ")";
  }
  case Kind::Ref:
    return std::string("&") + (refMut ? "mut " : "") + elems[0].str();
  case Kind::Array:
    return "[" + elems[0].str() + "; " + std::to_string(arrayLen) + "]";
  case Kind::Slice:
    return std::string("&") + (refMut ? "mut " : "") + "[" + elems[0].str() + "]";
  case Kind::Box:
    return "Box<" + elems[0].str() + ">";
  case Kind::Vec:
    return "Vec<" + elems[0].str() + ">";
  case Kind::Str:
    return "str";
  }
  return "<?>";
}

bool kal::typeFromName(llvm::StringRef name, Type &out) {
  if (name == "bool") {
    out = Type::boolean();
    return true;
  }
  if (name == "i8") {
    out = Type::intTy(8, true);
    return true;
  }
  if (name == "i16") {
    out = Type::intTy(16, true);
    return true;
  }
  if (name == "i32") {
    out = Type::intTy(32, true);
    return true;
  }
  if (name == "i64") {
    out = Type::intTy(64, true);
    return true;
  }
  if (name == "u8") {
    out = Type::intTy(8, false);
    return true;
  }
  if (name == "u16") {
    out = Type::intTy(16, false);
    return true;
  }
  if (name == "u32") {
    out = Type::intTy(32, false);
    return true;
  }
  if (name == "u64") {
    out = Type::intTy(64, false);
    return true;
  }
  if (name == "f32") {
    out = Type::floatTy(32);
    return true;
  }
  if (name == "f64") {
    out = Type::floatTy(64);
    return true;
  }
  if (name == "str") {
    out = Type::strTy();
    return true;
  }
  return false;
}
