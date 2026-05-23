//===- AST.h - pure syntax tree (no codegen coupling) ---------------------===//
#pragma once

#include "kal/Token.h"
#include "kal/Type.h"
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace kal {

/// 式ノードの基底。codegen は持たない (CodeGen が kind で分岐する)。
/// `type` は Sema (型検査) が後から埋める。
struct Expr {
  enum class Kind {
    Number,
    Variable,
    Binary,
    Call,
    If,
    For,
    Cast,
    StructLit,
    Field,
    TupleLit,
    TupleIndex,
    Match,
    Borrow,
    Deref,
    Block,
    Assign,
    Unary,
    ArrayLit,
    Index,
    BoolLit,
    StringLit,
    MethodCall,
    Return,
    Try,
  };
  Kind kind;
  Span span;
  Type type; // Sema が設定する計算結果の型
  Expr(Kind k, Span s) : kind(k), span(s) {}
  virtual ~Expr() = default;
};
using ExprPtr = std::unique_ptr<Expr>;

/// 真偽値リテラル: `true` / `false`
struct BoolLitExpr : Expr {
  bool value;
  BoolLitExpr(Span s, bool value) : Expr(Kind::BoolLit, s), value(value) {}
};

/// 文字列リテラル: `"..."`。value はエスケープ展開済みのバイト列。型は `str`。
struct StringLitExpr : Expr {
  std::string value;
  StringLitExpr(Span s, std::string value)
      : Expr(Kind::StringLit, s), value(std::move(value)) {}
};

/// 数値リテラル。整数か小数かを保持し、型は Sema が文脈から決める。
struct NumberExpr : Expr {
  bool isFloat;
  double floatValue;
  uint64_t intValue;
  NumberExpr(Span s, bool isFloat, double fv, uint64_t iv)
      : Expr(Kind::Number, s), isFloat(isFloat), floatValue(fv), intValue(iv) {}
};

/// 型キャスト: `operand as targetType`
struct CastExpr : Expr {
  ExprPtr operand;
  Type targetType;
  CastExpr(Span s, ExprPtr operand, Type targetType)
      : Expr(Kind::Cast, s), operand(std::move(operand)),
        targetType(targetType) {}
};

/// 構造体リテラル: `Name { f1: e1, f2: e2 }` (fieldNames[i] = fieldValues[i])
struct StructLitExpr : Expr {
  std::string structName;
  Span nameSpan;
  std::vector<std::string> fieldNames;
  std::vector<ExprPtr> fieldValues;
  StructLitExpr(Span s, std::string n, Span nameSpan)
      : Expr(Kind::StructLit, s), structName(std::move(n)), nameSpan(nameSpan) {}
};

/// フィールドアクセス: `operand.field`
struct FieldExpr : Expr {
  ExprPtr operand;
  std::string field;
  Span fieldSpan;
  int fieldIndex = -1; // Sema が設定する
  FieldExpr(Span s, ExprPtr operand, std::string field, Span fieldSpan)
      : Expr(Kind::Field, s), operand(std::move(operand)),
        field(std::move(field)), fieldSpan(fieldSpan) {}
};

/// タプルリテラル: `(e1, e2, ...)`
struct TupleLitExpr : Expr {
  std::vector<ExprPtr> elems;
  TupleLitExpr(Span s, std::vector<ExprPtr> elems)
      : Expr(Kind::TupleLit, s), elems(std::move(elems)) {}
};

/// タプル要素アクセス: `operand.N`
struct TupleIndexExpr : Expr {
  ExprPtr operand;
  unsigned index;
  Span indexSpan;
  TupleIndexExpr(Span s, ExprPtr operand, unsigned index, Span indexSpan)
      : Expr(Kind::TupleIndex, s), operand(std::move(operand)), index(index),
        indexSpan(indexSpan) {}
};

/// ブロック内の文。
///   Let : `let [mut] name [: T] = init;`
///   Expr: `expr;`
struct Stmt {
  enum class Kind { Let, Expr } kind;
  Span span;
  // Let 用:
  std::string name;
  bool isMut = false;
  Type annotatedType;
  bool hasAnnotation = false;
  Span nameSpan;
  // Let の init / Expr の式
  ExprPtr expr;
};

/// ブロック式: `{ stmt* tail? }` (tail なしなら unit)
struct BlockExpr : Expr {
  std::vector<Stmt> stmts;
  ExprPtr tail;
  BlockExpr(Span s) : Expr(Kind::Block, s) {}
};

/// 代入式 (値は unit): `target = value`
struct AssignExpr : Expr {
  ExprPtr target;
  ExprPtr value;
  AssignExpr(Span s, ExprPtr target, ExprPtr value)
      : Expr(Kind::Assign, s), target(std::move(target)),
        value(std::move(value)) {}
};

/// match の 1 アーム:  `Variant(b1, b2) => body`  または  `_ => body`
struct MatchArm {
  bool isWildcard = false;
  std::string variant;               // バリアント名 (wildcard 以外)
  Span variantSpan;
  std::vector<std::string> bindings; // ペイロード束縛名 ("_" 可)
  std::vector<Span> bindingSpans;
  ExprPtr body;
  // Sema が解決:
  int tag = -1;
  std::vector<Type> payloadTypes;
};

/// パターンマッチ: `match scrutinee { arm, arm, ... }`
struct MatchExpr : Expr {
  ExprPtr scrutinee;
  std::vector<MatchArm> arms;
  MatchExpr(Span s, ExprPtr scrutinee)
      : Expr(Kind::Match, s), scrutinee(std::move(scrutinee)) {}
};

/// 借用: `&operand` または `&mut operand`
struct BorrowExpr : Expr {
  ExprPtr operand;
  bool isMut;
  BorrowExpr(Span s, ExprPtr operand, bool isMut)
      : Expr(Kind::Borrow, s), operand(std::move(operand)), isMut(isMut) {}
};

/// 参照外し: `*operand`
struct DerefExpr : Expr {
  ExprPtr operand;
  // MoveCheck が、これが Box の中身をムーブ取り出しする (= 箱を free する) なら設定
  bool movesOutOfBox = false;
  DerefExpr(Span s, ExprPtr operand)
      : Expr(Kind::Deref, s), operand(std::move(operand)) {}
};

/// 配列リテラル: `[e1, e2, ...]` (要素は同じ型・要素数 >= 1)
struct ArrayLitExpr : Expr {
  std::vector<ExprPtr> elems;
  ArrayLitExpr(Span s, std::vector<ExprPtr> elems)
      : Expr(Kind::ArrayLit, s), elems(std::move(elems)) {}
};

/// 添字アクセス: `base[index]` (読み書き両用)
struct IndexExpr : Expr {
  ExprPtr base;
  ExprPtr index;
  IndexExpr(Span s, ExprPtr base, ExprPtr index)
      : Expr(Kind::Index, s), base(std::move(base)), index(std::move(index)) {}
};

/// 早期リターン: `return value` または `return` (値なし=unit)。型は発散。
struct ReturnExpr : Expr {
  ExprPtr value; // null なら unit を返す
  ReturnExpr(Span s, ExprPtr value)
      : Expr(Kind::Return, s), value(std::move(value)) {}
};

/// `?` 演算子: `operand?`。operand が Ok/Some なら中身、Err/None なら早期リターン。
struct TryExpr : Expr {
  ExprPtr operand;
  // Sema が解決: 0=Option, 1=Result
  int kind = 0;
  TryExpr(Span s, ExprPtr operand)
      : Expr(Kind::Try, s), operand(std::move(operand)) {}
};

/// メソッド呼び出し: `receiver.method(args)`
struct MethodCallExpr : Expr {
  ExprPtr receiver;
  std::string method;
  Span methodSpan;
  std::vector<ExprPtr> args;
  // Sema が解決する:
  std::string ownerType;      // レシーバの (deref 後の) 型名
  std::vector<Type> typeArgs; // impl の型引数 (具体)
  int selfKind = 0;           // 0=self(値), 1=&self, 2=&mut self
  bool recvIsRef = false;     // レシーバ式が既に参照型か
  MethodCallExpr(Span s, ExprPtr receiver, std::string method, Span methodSpan,
                 std::vector<ExprPtr> args)
      : Expr(Kind::MethodCall, s), receiver(std::move(receiver)),
        method(std::move(method)), methodSpan(methodSpan),
        args(std::move(args)) {}
};

/// 単項演算: `-operand` (Tok::Minus) または `!operand` (Tok::Bang)
struct UnaryExpr : Expr {
  Tok op;
  ExprPtr operand;
  UnaryExpr(Span s, Tok op, ExprPtr operand)
      : Expr(Kind::Unary, s), op(op), operand(std::move(operand)) {}
};

struct VariableExpr : Expr {
  std::string name;
  // Sema が、これが引数なしの enum バリアントなら設定する
  int variantTag = -1;
  std::string variantEnum;
  // MoveCheck が、この使用が値をムーブする (= ドロップフラグを下ろす) なら設定する
  bool movesValue = false;
  VariableExpr(Span s, std::string n)
      : Expr(Kind::Variable, s), name(std::move(n)) {}
};

struct BinaryExpr : Expr {
  Tok op;
  Span opSpan;
  ExprPtr lhs, rhs;
  BinaryExpr(Span s, Tok op, Span opSpan, ExprPtr lhs, ExprPtr rhs)
      : Expr(Kind::Binary, s), op(op), opSpan(opSpan), lhs(std::move(lhs)),
        rhs(std::move(rhs)) {}
};

struct CallExpr : Expr {
  std::string callee;
  Span calleeSpan;
  std::vector<ExprPtr> args;
  // Sema が、これが enum バリアント構築なら設定する
  int variantTag = -1;
  std::string variantEnum;
  // Sema が、これが組み込み len(s) なら設定する (引数は借用・ムーブしない)
  bool isLenBuiltin = false;
  // Sema が、これが組み込み box(e) なら設定する (ヒープ確保して Box<T> を返す)
  bool isBoxBuiltin = false;
  // Sema が、これが組み込み vec() なら設定する (空の Vec<T> を返す)
  bool isVecBuiltin = false;
  // Sema が、これが組み込み push(v, x) なら設定する (v に x を追加・v は可変借用)
  bool isPushBuiltin = false;
  // Sema が、これが組み込み prints(s) なら設定する (str/String を出力)
  bool isPrintsBuiltin = false;
  // Sema が、これが組み込み string(s) なら設定する (str から所有 String を作る)
  bool isStringBuiltin = false;
  // Sema が、これが組み込み push_str(s, t) なら設定する (String に str を追記)
  bool isPushStrBuiltin = false;
  // Sema が、これがジェネリック関数呼び出しなら推論した型引数を設定する
  std::vector<Type> typeArgs;
  CallExpr(Span s, std::string callee, Span calleeSpan,
           std::vector<ExprPtr> args)
      : Expr(Kind::Call, s), callee(std::move(callee)), calleeSpan(calleeSpan),
        args(std::move(args)) {}
};

struct IfExpr : Expr {
  ExprPtr cond, then, els;
  IfExpr(Span s, ExprPtr cond, ExprPtr then, ExprPtr els)
      : Expr(Kind::If, s), cond(std::move(cond)), then(std::move(then)),
        els(std::move(els)) {}
};

struct ForExpr : Expr {
  std::string var;
  ExprPtr start, cond, step, body; // step は省略時 null
  ForExpr(Span s, std::string var, ExprPtr start, ExprPtr cond, ExprPtr step,
          ExprPtr body)
      : Expr(Kind::For, s), var(std::move(var)), start(std::move(start)),
        cond(std::move(cond)), step(std::move(step)), body(std::move(body)) {}
};

/// 関数プロトタイプ (引数名 + 引数型 + 戻り値型)。
/// typeParams が空でなければジェネリック関数 (呼び出しごとに単態化される)。
struct Prototype {
  std::string name;
  Span nameSpan;
  std::vector<std::string> typeParams; // ジェネリックな型引数名 (空なら非総称)
  // 型引数の境界: (型引数名, トレイト名)。例 <T: Area> → {"T","Area"}
  std::vector<std::pair<std::string, std::string>> bounds;
  std::vector<std::string> args;
  std::vector<Type> paramTypes;
  Type retType;
  Span span;
};

/// 関数定義 (プロトタイプ + 本体)。
/// 名前は llvm::Function との衝突を避けて FunctionDef。
struct FunctionDef {
  std::unique_ptr<Prototype> proto;
  ExprPtr body;
};

/// 構造体の 1 フィールド。
struct StructField {
  std::string name;
  Type type;
  Span span;
};

/// 構造体定義: `struct Name<P, ...> { f: T, ... }`
/// typeParams が空でなければジェネリック (使用箇所ごとに単態化される)。
struct StructDef {
  std::string name;
  Span nameSpan;
  std::vector<std::string> typeParams; // ジェネリックな型引数名 (空なら非総称)
  std::vector<StructField> fields;
  Span span;
};

/// enum の 1 バリアント: `Name` または `Name(T, ...)`
struct EnumVariant {
  std::string name;
  Span span;
  std::vector<Type> payloadTypes;
};

/// enum 定義 (Rust 流 ADT): `enum Name<P, ...> { V1, V2(T, ...), ... }`
/// typeParams が空でなければジェネリック (使用箇所ごとに単態化される)。
struct EnumDef {
  std::string name;
  Span nameSpan;
  std::vector<std::string> typeParams; // ジェネリックな型引数名 (空なら非総称)
  std::vector<EnumVariant> variants;
  Span span;
};

/// トレイト宣言: `trait Name { fn m(&self, ...) -> T; ... }` (シグネチャのみ)。
/// 各メソッドは Prototype (args[0]="self"、paramTypes[0]=self の種類を表す型)。
struct TraitDef {
  std::string name;
  Span nameSpan;
  std::vector<std::unique_ptr<Prototype>> methods;
  Span span;
};

/// impl ブロック: `impl Name<P, ...> { ... }` (固有メソッド) または
/// `impl Trait for Name { ... }` (トレイト実装。traitName が非空)。
/// 各メソッドは FunctionDef (proto.name = "Name#method"、args[0]="self"、
/// proto.typeParams = impl の型引数)。
struct ImplBlock {
  std::string traitName; // 空でなければトレイト実装
  Span traitSpan;
  std::string typeName;
  Span typeSpan;
  std::vector<std::string> typeParams;
  std::vector<std::unique_ptr<FunctionDef>> methods;
  Span span;
};

/// 1 つのソースをパースした結果。
struct Program {
  std::vector<std::unique_ptr<StructDef>> structs;
  std::vector<std::unique_ptr<EnumDef>> enums;
  std::vector<std::unique_ptr<Prototype>> externs;
  std::vector<std::unique_ptr<FunctionDef>> functions;
  std::vector<std::unique_ptr<TraitDef>> traits;
  std::vector<std::unique_ptr<ImplBlock>> impls;
  std::vector<ExprPtr> topExprs;
};

} // namespace kal
