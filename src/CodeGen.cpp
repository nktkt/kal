//===- CodeGen.cpp - lowers the typed AST to LLVM IR ----------------------===//
//
// Sema が各 Expr に型を注釈済みなので、ここでは型に従って整数/浮動小数点の
// 命令を選ぶだけ。型エラーは起きない前提 (起きたら内部バグ)。
//
//===----------------------------------------------------------------------===//
#include "kal/CodeGen.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Alignment.h"

using namespace kal;
using namespace llvm;

CodeGen::CodeGen(LLVMContext &ctx, DiagnosticEngine &diag,
                 const DataLayout &dl)
    : ctx_(ctx), diag_(diag), dl_(dl), builder_(ctx) {}

// subst に従って Param を具体型へ置換する (単態化用)。
static kal::Type substType(const kal::Type &t,
                           const std::map<std::string, kal::Type> &subst) {
  if (t.kind == kal::Type::Kind::Param) {
    auto it = subst.find(t.name);
    return it != subst.end() ? it->second : t;
  }
  kal::Type r = t;
  for (auto &e : r.elems)
    e = substType(e, subst);
  return r;
}

// 具体型ごとの型引数置換マップ (struct/enum のフィールド/ペイロード解決用)。
static std::map<std::string, kal::Type>
instSubst(const std::vector<std::string> &params,
          const std::vector<kal::Type> &args) {
  std::map<std::string, kal::Type> s;
  for (size_t i = 0; i < params.size() && i < args.size(); ++i)
    s[params[i]] = args[i];
  return s;
}

// 値として Box を含む (= スコープ離脱で解放が要る) 型か。
bool CodeGen::needsDrop(const kal::Type &t) {
  switch (t.kind) {
  case kal::Type::Kind::Box:
  case kal::Type::Kind::Vec:
  case kal::Type::Kind::String:
    return true; // ヒープを所有する (再帰せず即 true → 無限再帰しない)
  case kal::Type::Kind::Tuple:
  case kal::Type::Kind::Array:
    for (auto &e : t.elems)
      if (needsDrop(e))
        return true;
    return false;
  case kal::Type::Kind::Struct: {
    const StructDef *sd = structDefs_[t.name];
    auto sub = instSubst(sd->typeParams, t.elems);
    for (auto &f : sd->fields)
      if (needsDrop(substType(f.type, sub)))
        return true;
    return false;
  }
  case kal::Type::Kind::Enum: {
    const EnumDef *ed = enumDefs_[t.name];
    auto sub = instSubst(ed->typeParams, t.elems);
    for (auto &v : ed->variants)
      for (auto &pt : v.payloadTypes)
        if (needsDrop(substType(pt, sub)))
          return true;
    return false;
  }
  default:
    return false; // Ref/Slice/数値/Param 等は所有しない
  }
}

// 型 t のドロップグルー関数 void drop_<t>(ptr)。未生成なら宣言してキューへ。
llvm::Function *CodeGen::getDropFn(const kal::Type &t) {
  std::string mangled = "drop$" + t.str();
  auto it = dropFns_.find(mangled);
  if (it != dropFns_.end())
    return it->second;
  FunctionType *ft = FunctionType::get(llvm::Type::getVoidTy(ctx_),
                                       {PointerType::getUnqual(ctx_)}, false);
  Function *fn =
      Function::Create(ft, Function::InternalLinkage, mangled, module_.get());
  dropFns_[mangled] = fn;
  pendingDropFns_.push_back({t, fn}); // 本体は後で生成 (再帰型対応)
  return fn;
}

llvm::Type *CodeGen::toLLVM(const kal::Type &rawT) {
  // ジェネリック単態化中なら型引数 (Param) を具体型へ置換する
  kal::Type t = typeSubst_.empty() ? rawT : substType(rawT, typeSubst_);
  switch (t.kind) {
  case kal::Type::Kind::Bool:
    return llvm::Type::getInt1Ty(ctx_);
  case kal::Type::Kind::Int:
    return llvm::Type::getIntNTy(ctx_, t.bits);
  case kal::Type::Kind::Float:
    return t.bits == 32 ? llvm::Type::getFloatTy(ctx_)
                        : llvm::Type::getDoubleTy(ctx_);
  case kal::Type::Kind::Unit:
    return llvm::Type::getVoidTy(ctx_);
  case kal::Type::Kind::Struct:
    return getStructType(t);
  case kal::Type::Kind::Enum:
    return getEnumType(t);
  case kal::Type::Kind::Param:
    return nullptr; // 単態化後は現れない (内部バグなら nullptr)
  case kal::Type::Kind::Ref:
    return PointerType::getUnqual(ctx_);
  case kal::Type::Kind::Tuple: {
    std::vector<llvm::Type *> elems;
    for (auto &e : t.elems)
      elems.push_back(toLLVM(e));
    return StructType::get(ctx_, elems);
  }
  case kal::Type::Kind::Array:
    return ArrayType::get(toLLVM(t.elems[0]), t.arrayLen);
  case kal::Type::Kind::Slice:
    // fat pointer: { 要素先頭ポインタ, i64 長さ }
    return StructType::get(
        ctx_, {PointerType::getUnqual(ctx_), llvm::Type::getInt64Ty(ctx_)});
  case kal::Type::Kind::Box:
    return PointerType::getUnqual(ctx_); // ヒープへのポインタ
  case kal::Type::Kind::Vec:
    return vecLLVMTy(); // { ptr, i64 len, i64 cap }
  case kal::Type::Kind::Str:
    // str = 静的バイト列への fat pointer { ptr, i64 len } (スライスと同形)
    return StructType::get(
        ctx_, {PointerType::getUnqual(ctx_), llvm::Type::getInt64Ty(ctx_)});
  case kal::Type::Kind::String:
    return vecLLVMTy(); // String = 所有バイトバッファ { ptr, i64 len, i64 cap }
  case kal::Type::Kind::Unknown:
    return nullptr;
  }
  return nullptr;
}

// Vec<T> の LLVM 表現 { 要素バッファへのポインタ, i64 長さ, i64 容量 }。
// 要素型に依らず形は一定 (不透明ポインタ)。スライスと同じく len は添字 1。
StructType *CodeGen::vecLLVMTy() {
  llvm::Type *i64 = llvm::Type::getInt64Ty(ctx_);
  return StructType::get(ctx_,
                         {PointerType::getUnqual(ctx_), i64, i64});
}

StructType *CodeGen::getStructType(const kal::Type &rawType) {
  kal::Type structType =
      typeSubst_.empty() ? rawType : substType(rawType, typeSubst_);
  std::string mangled = structType.str(); // "Point" / "Pair<i64, f64>" など
  auto it = structTypes_.find(mangled);
  if (it != structTypes_.end())
    return it->second;
  const StructDef *sd = structDefs_[structType.name];
  // 型引数 → 具体型の対応 (非総称なら空)
  std::map<std::string, kal::Type> subst;
  for (size_t i = 0; i < sd->typeParams.size() && i < structType.elems.size();
       ++i)
    subst[sd->typeParams[i]] = structType.elems[i];
  std::vector<llvm::Type *> fieldTys;
  for (auto &f : sd->fields)
    fieldTys.push_back(toLLVM(substType(f.type, subst)));
  StructType *st = StructType::create(ctx_, fieldTys, mangled);
  structTypes_[mangled] = st;
  return st;
}

// enum を { i64 tag, [S x i8] payload } で表す (S = 最大バリアントのサイズ)。
// tag を i64 にすることで payload が 8 バイト境界に整列する。
// ジェネリック enum は具体化 (型引数) ごとに別々の型として単態化する。
StructType *CodeGen::getEnumType(const kal::Type &rawType) {
  kal::Type enumType =
      typeSubst_.empty() ? rawType : substType(rawType, typeSubst_);
  std::string mangled = enumType.str(); // "Shape" / "Option<i64>" など
  auto it = enumTypes_.find(mangled);
  if (it != enumTypes_.end())
    return it->second;
  const EnumDef *ed = enumDefs_[enumType.name];
  // 型引数 → 具体型の対応 (非総称なら空)
  std::map<std::string, kal::Type> subst;
  for (size_t i = 0; i < ed->typeParams.size() && i < enumType.elems.size(); ++i)
    subst[ed->typeParams[i]] = enumType.elems[i];

  uint64_t maxSize = 0;
  for (auto &v : ed->variants) {
    std::vector<llvm::Type *> fts;
    for (auto &pt : v.payloadTypes)
      fts.push_back(toLLVM(substType(pt, subst)));
    StructType *vs = StructType::get(ctx_, fts);
    maxSize = std::max(maxSize, dl_.getTypeAllocSize(vs).getFixedValue());
  }
  llvm::Type *tag = llvm::Type::getInt64Ty(ctx_);
  llvm::Type *payload = ArrayType::get(llvm::Type::getInt8Ty(ctx_), maxSize);
  StructType *et = StructType::create(ctx_, {tag, payload}, mangled);
  enumTypes_[mangled] = et;
  return et;
}

Value *CodeGen::genVariant(const kal::Type &enumType, int tag,
                           ArrayRef<Value *> payload) {
  StructType *et = getEnumType(enumType);
  // メモリ上に組み立ててから値としてロードする (タグ付き共用体)
  AllocaInst *slot = builder_.CreateAlloca(et);
  slot->setAlignment(Align(8));
  // tag を格納
  Value *tagPtr = builder_.CreateStructGEP(et, slot, 0, "tagptr");
  builder_.CreateStore(ConstantInt::get(llvm::Type::getInt64Ty(ctx_), tag),
                       tagPtr);
  // payload を格納 (バリアントの構造体型として書き込む)
  if (!payload.empty()) {
    std::vector<llvm::Type *> fts;
    for (Value *p : payload)
      fts.push_back(p->getType());
    StructType *vs = StructType::get(ctx_, fts);
    Value *payPtr = builder_.CreateStructGEP(et, slot, 1, "payptr");
    Value *agg = UndefValue::get(vs);
    for (unsigned i = 0; i < payload.size(); ++i)
      agg = builder_.CreateInsertValue(agg, payload[i], {i});
    builder_.CreateStore(agg, payPtr);
  }
  return builder_.CreateLoad(et, slot, "enumval");
}

Value *CodeGen::genExpr(const Expr *e) {
  // 既に発散 (return 等で現在ブロックが終端) していれば何も生成しない。
  // これにより発散後の兄弟部分式が命令を吐かず、null 値の連鎖を止める。
  if (blockDone())
    return nullptr;
  switch (e->kind) {
  case Expr::Kind::Number:
    return genNumber(static_cast<const NumberExpr *>(e));
  case Expr::Kind::Variable:
    return genVariable(static_cast<const VariableExpr *>(e));
  case Expr::Kind::Binary:
    return genBinary(static_cast<const BinaryExpr *>(e));
  case Expr::Kind::Call:
    return genCall(static_cast<const CallExpr *>(e));
  case Expr::Kind::If:
    return genIf(static_cast<const IfExpr *>(e));
  case Expr::Kind::For:
    return genFor(static_cast<const ForExpr *>(e));
  case Expr::Kind::Cast:
    return genCast(static_cast<const CastExpr *>(e));
  case Expr::Kind::StructLit:
    return genStructLit(static_cast<const StructLitExpr *>(e));
  case Expr::Kind::Field:
    return genField(static_cast<const FieldExpr *>(e));
  case Expr::Kind::TupleLit:
    return genTupleLit(static_cast<const TupleLitExpr *>(e));
  case Expr::Kind::TupleIndex:
    return genTupleIndex(static_cast<const TupleIndexExpr *>(e));
  case Expr::Kind::Block:
    return genBlock(static_cast<const BlockExpr *>(e));
  case Expr::Kind::Assign:
    return genAssign(static_cast<const AssignExpr *>(e));
  case Expr::Kind::Match:
    return genMatch(static_cast<const MatchExpr *>(e));
  case Expr::Kind::Borrow:
    return genBorrow(static_cast<const BorrowExpr *>(e));
  case Expr::Kind::Deref:
    return genDeref(static_cast<const DerefExpr *>(e));
  case Expr::Kind::Unary:
    return genUnary(static_cast<const UnaryExpr *>(e));
  case Expr::Kind::ArrayLit:
    return genArrayLit(static_cast<const ArrayLitExpr *>(e));
  case Expr::Kind::Index:
    return genIndex(static_cast<const IndexExpr *>(e));
  case Expr::Kind::BoolLit:
    return builder_.getInt1(static_cast<const BoolLitExpr *>(e)->value);
  case Expr::Kind::StringLit:
    return genStringLit(static_cast<const StringLitExpr *>(e));
  case Expr::Kind::MethodCall:
    return genMethodCall(static_cast<const MethodCallExpr *>(e));
  case Expr::Kind::Return:
    return genReturn(static_cast<const ReturnExpr *>(e));
  case Expr::Kind::Try:
    return genTry(static_cast<const TryExpr *>(e));
  }
  return nullptr;
}

bool CodeGen::blockDone() {
  return builder_.GetInsertBlock()->getTerminator() != nullptr;
}

Value *CodeGen::genStructLit(const StructLitExpr *e) {
  const StructDef *sd = structDefs_[e->structName];
  StructType *st = getStructType(e->type); // 具体化されたインスタンス型
  Value *agg = UndefValue::get(st);
  // 宣言順に、対応する初期化式を探して詰める
  for (size_t i = 0; i < sd->fields.size(); ++i) {
    Value *fv = nullptr;
    for (size_t j = 0; j < e->fieldNames.size(); ++j)
      if (e->fieldNames[j] == sd->fields[i].name) {
        fv = genExpr(e->fieldValues[j].get());
        break;
      }
    if (blockDone())
      return nullptr; // フィールド値が発散
    agg = builder_.CreateInsertValue(agg, fv, {static_cast<unsigned>(i)});
  }
  return agg;
}

Value *CodeGen::genField(const FieldExpr *e) {
  // &Struct は自動 deref: ポインタ経由でフィールドを読む
  if (e->operand->type.isRef()) {
    Value *addr = genAddr(e);
    return builder_.CreateLoad(toLLVM(e->type), addr, "field");
  }
  Value *v = genExpr(e->operand.get());
  if (blockDone())
    return nullptr;
  return builder_.CreateExtractValue(v, {static_cast<unsigned>(e->fieldIndex)},
                                     "field");
}

Value *CodeGen::genTupleLit(const TupleLitExpr *e) {
  llvm::Type *tt = toLLVM(e->type);
  Value *agg = UndefValue::get(tt);
  for (size_t i = 0; i < e->elems.size(); ++i) {
    Value *ev = genExpr(e->elems[i].get());
    if (blockDone())
      return nullptr;
    agg = builder_.CreateInsertValue(agg, ev, {static_cast<unsigned>(i)});
  }
  return agg;
}

Value *CodeGen::genTupleIndex(const TupleIndexExpr *e) {
  Value *v = genExpr(e->operand.get());
  if (blockDone())
    return nullptr;
  return builder_.CreateExtractValue(v, {e->index}, "telem");
}

Value *CodeGen::genArrayLit(const ArrayLitExpr *e) {
  llvm::Type *at = toLLVM(e->type); // [N x T]
  Value *agg = UndefValue::get(at);
  for (size_t i = 0; i < e->elems.size(); ++i) {
    Value *ev = genExpr(e->elems[i].get());
    if (blockDone())
      return nullptr;
    agg = builder_.CreateInsertValue(agg, ev, {static_cast<unsigned>(i)});
  }
  return agg;
}

Value *CodeGen::genIndex(const IndexExpr *e) {
  Value *addr = genAddr(e); // 要素アドレスを計算して
  return builder_.CreateLoad(toLLVM(e->type), addr, "idxval"); // 読み出す
}

// 場所 (lvalue) 式か。場所の値は他に所有者がいる (変数・フィールド等) ので、
// 捨てても drop してはいけない。それ以外は新しく作られた一時値とみなす。
static bool isPlaceExpr(const Expr *e) {
  switch (e->kind) {
  case Expr::Kind::Variable:
    return static_cast<const VariableExpr *>(e)->variantTag < 0; // 変数は場所
  case Expr::Kind::Field:
  case Expr::Kind::TupleIndex:
  case Expr::Kind::Index:
  case Expr::Kind::Deref:
    return true;
  default:
    return false; // Call / 構築 / if / match などは一時値
  }
}

// 捨てられる式文の値が「所有ヒープを持つ一時値」なら drop する。
// 一時値は新規に作られた所有値で他に所有者がいないため、ここで解放しても
// 二重解放にならない (場所式は除外。`box(x);` / `pop(v);` などのリークを防ぐ)。
void CodeGen::dropDiscardedValue(const Expr *e, llvm::Value *v) {
  if (!v || blockDone() || isPlaceExpr(e))
    return;
  kal::Type t = typeSubst_.empty() ? e->type : substType(e->type, typeSubst_);
  if (!needsDrop(t))
    return;
  AllocaInst *slot = entryAlloca(toLLVM(t), "discard");
  builder_.CreateStore(v, slot);
  builder_.CreateCall(getDropFn(t), {slot});
}

Value *CodeGen::genBlock(const BlockExpr *e) {
  std::vector<std::pair<std::string, Value *>> saved; // name, oldSlot(or null)
  dropScopes_.push_back({});                          // ブロックのドロップスコープ
  for (auto &st : e->stmts) {
    if (st.kind == Stmt::Kind::Let) {
      Value *v = genExpr(st.expr.get());
      if (blockDone())
        break; // init が発散 → 以降は到達不能
      AllocaInst *slot = entryAlloca(toLLVM(st.expr->type), st.name);
      builder_.CreateStore(v, slot);
      saved.push_back({st.name, namedValues_.count(st.name)
                                    ? namedValues_[st.name]
                                    : nullptr});
      namedValues_[st.name] = slot;
      registerLocal(slot, st.expr->type); // ドロップ対象なら登録
    } else {
      Value *sv = genExpr(st.expr.get()); // 式文: 値は捨てる
      if (blockDone())
        break; // 文が発散 → 以降は到達不能
      dropDiscardedValue(st.expr.get(), sv); // 一時値が所有ヒープなら解放
    }
  }
  Value *result =
      (!blockDone() && e->tail) ? genExpr(e->tail.get()) : nullptr;
  popDropScope(); // ブロック内ローカルを drop (発散済みなら return が drop 済み)
  // スコープ復元 (逆順)
  for (size_t i = saved.size(); i-- > 0;) {
    if (saved[i].second)
      namedValues_[saved[i].first] = saved[i].second;
    else
      namedValues_.erase(saved[i].first);
  }
  return result;
}

Value *CodeGen::genAssign(const AssignExpr *e) {
  Value *addr = genAddr(e->target.get());
  Value *v = genExpr(e->value.get());
  if (blockDone())
    return nullptr; // 対象/値の評価が発散
  // 旧値が drop を要するなら、上書き前に解放する (さもないとリーク)。
  kal::Type tt = typeSubst_.empty() ? e->target->type
                                    : substType(e->target->type, typeSubst_);
  if (needsDrop(tt)) {
    if (e->target->kind == Expr::Kind::Variable) {
      // 登録済みローカル: flag ガードで旧値を drop し (ムーブ済みなら何もしない)、
      // 新値は生存するので flag を立て直す。
      DropEntry *de = nullptr;
      for (auto &scope : dropScopes_)
        for (auto &d : scope)
          if (d.slot == addr)
            de = &d;
      if (de) {
        emitDrop(de->slot, de->flag, de->type); // live なら drop し flag=false
        builder_.CreateStore(v, addr);
        builder_.CreateStore(builder_.getInt1(true), de->flag);
        return nullptr;
      }
      // 登録が見つからない場合は安全側 (drop しない) に倒す。
    } else {
      // サブプレース (v[i] / s.f / *p): 旧値はコンテナが生存している限り生きて
      // いるので、その場で無条件に drop する。
      builder_.CreateCall(getDropFn(tt), {addr});
    }
  }
  builder_.CreateStore(v, addr);
  return nullptr; // 代入式は unit
}

Value *CodeGen::genMatch(const MatchExpr *e) {
  // &Enum レシーバ (例 &self) はポインタをそのまま使い、値はメモリへ退避する。
  StructType *et;
  Value *slot;
  if (e->scrutinee->type.isRef()) {
    slot = genExpr(e->scrutinee.get()); // enum へのポインタ
    if (blockDone())
      return nullptr; // 対象式が発散
    et = getEnumType(e->scrutinee->type.pointee());
  } else {
    Value *scrut = genExpr(e->scrutinee.get());
    if (blockDone())
      return nullptr; // 対象式が発散
    et = cast<StructType>(scrut->getType());
    AllocaInst *a = builder_.CreateAlloca(et);
    a->setAlignment(Align(8));
    builder_.CreateStore(scrut, a);
    slot = a;
  }

  // slot から tag と payload を読み出す
  Value *tagPtr = builder_.CreateStructGEP(et, slot, 0, "tagptr");
  Value *tag =
      builder_.CreateLoad(llvm::Type::getInt64Ty(ctx_), tagPtr, "tag");

  Function *fn = builder_.GetInsertBlock()->getParent();
  BasicBlock *mergeBB = BasicBlock::Create(ctx_, "matchcont");

  // 各アームのブロックを作る (wildcard は switch の default)
  std::vector<BasicBlock *> armBBs;
  BasicBlock *defaultBB = nullptr;
  for (auto &arm : e->arms) {
    BasicBlock *bb =
        BasicBlock::Create(ctx_, arm.isWildcard ? "wild" : "arm", fn);
    armBBs.push_back(bb);
    if (arm.isWildcard)
      defaultBB = bb;
  }
  BasicBlock *unreachBB = nullptr;
  if (!defaultBB)
    unreachBB = BasicBlock::Create(ctx_, "unreach", fn);

  SwitchInst *sw = builder_.CreateSwitch(
      tag, defaultBB ? defaultBB : unreachBB, e->arms.size());
  for (size_t i = 0; i < e->arms.size(); ++i)
    if (!e->arms[i].isWildcard)
      sw->addCase(
          ConstantInt::get(llvm::Type::getInt64Ty(ctx_), e->arms[i].tag),
          armBBs[i]);

  bool isUnit = !e->type.isKnown() || e->type.isUnit();
  std::vector<std::pair<Value *, BasicBlock *>> incoming;
  bool anyLive = false; // 末尾まで到達する (発散しない) アームがあるか

  for (size_t a = 0; a < e->arms.size(); ++a) {
    const MatchArm &arm = e->arms[a];
    builder_.SetInsertPoint(armBBs[a]);
    dropScopes_.push_back({}); // アームのドロップスコープ (束縛の解放用)

    // ペイロードを束縛 (退避して復元)
    std::vector<std::string> boundNames;
    std::vector<Value *> oldVals;
    if (!arm.isWildcard && !arm.payloadTypes.empty()) {
      std::vector<llvm::Type *> fts;
      for (auto &pt : arm.payloadTypes)
        fts.push_back(toLLVM(pt));
      StructType *vs = StructType::get(ctx_, fts);
      Value *payPtr = builder_.CreateStructGEP(et, slot, 1, "payptr");
      Value *pv = builder_.CreateLoad(vs, payPtr, "payld");
      for (size_t i = 0; i < arm.bindings.size(); ++i) {
        if (arm.bindings[i] == "_")
          continue;
        Value *fv = builder_.CreateExtractValue(pv, {static_cast<unsigned>(i)});
        AllocaInst *bslot =
            entryAlloca(toLLVM(arm.payloadTypes[i]), arm.bindings[i]);
        builder_.CreateStore(fv, bslot);
        boundNames.push_back(arm.bindings[i]);
        oldVals.push_back(namedValues_.count(arm.bindings[i])
                              ? namedValues_[arm.bindings[i]]
                              : nullptr);
        namedValues_[arm.bindings[i]] = bslot;
        registerLocal(bslot, arm.payloadTypes[i]); // ドロップ対象なら登録
      }
    }

    // 値で match した場合、対象 enum はこの match が所有する。このアームで
    // 束縛しなかったペイロードはどこからも drop されないのでここで解放する。
    // (参照経由の match は借用なので何も drop しない)
    if (!e->scrutinee->type.isRef()) {
      if (arm.isWildcard) {
        // ワイルドカード: バリアント不明 + 束縛なし → enum 全体を drop。
        kal::Type st = typeSubst_.empty()
                           ? e->scrutinee->type
                           : substType(e->scrutinee->type, typeSubst_);
        if (needsDrop(st))
          builder_.CreateCall(getDropFn(st), {slot});
      } else if (!arm.payloadTypes.empty()) {
        std::vector<llvm::Type *> fts;
        for (auto &pt : arm.payloadTypes)
          fts.push_back(toLLVM(pt));
        StructType *vs = StructType::get(ctx_, fts);
        Value *payPtr = builder_.CreateStructGEP(et, slot, 1, "payptr");
        for (size_t i = 0; i < arm.payloadTypes.size(); ++i) {
          bool bound = i < arm.bindings.size() && arm.bindings[i] != "_";
          if (bound)
            continue; // 束縛済みは binding ローカルが drop する
          kal::Type pt = typeSubst_.empty()
                             ? arm.payloadTypes[i]
                             : substType(arm.payloadTypes[i], typeSubst_);
          if (!needsDrop(pt))
            continue;
          Value *fp = builder_.CreateStructGEP(vs, payPtr,
                                               static_cast<unsigned>(i), "updr");
          builder_.CreateCall(getDropFn(pt), {fp});
        }
      }
    }

    Value *bv = genExpr(arm.body.get());
    popDropScope(); // 束縛を drop (発散済みなら return が drop 済み)

    for (size_t i = 0; i < boundNames.size(); ++i) {
      if (oldVals[i])
        namedValues_[boundNames[i]] = oldVals[i];
      else
        namedValues_.erase(boundNames[i]);
    }

    bool term = blockDone(); // アーム本体が発散したか
    BasicBlock *endBB = builder_.GetInsertBlock();
    if (!term) {
      builder_.CreateBr(mergeBB);
      anyLive = true;
      if (!isUnit)
        incoming.push_back({bv, endBB});
    }
  }

  if (unreachBB) {
    builder_.SetInsertPoint(unreachBB);
    builder_.CreateUnreachable();
  }

  fn->insert(fn->end(), mergeBB);
  builder_.SetInsertPoint(mergeBB);
  if (!anyLive) {
    builder_.CreateUnreachable(); // 全アームが発散 → merge は到達不能
    return nullptr;
  }
  if (isUnit)
    return nullptr;
  PHINode *phi =
      builder_.CreatePHI(toLLVM(e->type), incoming.size(), "matchtmp");
  for (auto &in : incoming)
    phi->addIncoming(in.first, in.second);
  return phi;
}

Value *CodeGen::genNumber(const NumberExpr *e) {
  if (e->isFloat)
    return ConstantFP::get(toLLVM(e->type), e->floatValue);
  return ConstantInt::get(toLLVM(e->type), e->intValue, e->type.isSigned);
}

AllocaInst *CodeGen::entryAlloca(llvm::Type *ty, const std::string &name) {
  Function *fn = builder_.GetInsertBlock()->getParent();
  BasicBlock &entry = fn->getEntryBlock();
  IRBuilder<> tmp(&entry, entry.begin());
  return tmp.CreateAlloca(ty, nullptr, name);
}

// idx が [0, len) の外なら kal_panic を呼ぶ。idx は符号に応じて i64 へ拡張し、
// 符号なし比較 uge で「負 (巨大な符号なし) も範囲超過も」一度に検出する。
void CodeGen::emitBoundsCheck(Value *idx, const kal::Type &idxType, Value *len) {
  llvm::Type *i64 = llvm::Type::getInt64Ty(ctx_);
  Value *idx64 = builder_.CreateIntCast(idx, i64, idxType.isSigned, "idx64");
  Value *oob = builder_.CreateICmpUGE(idx64, len, "oob");
  Function *fn = builder_.GetInsertBlock()->getParent();
  BasicBlock *panicBB = BasicBlock::Create(ctx_, "oob.panic", fn);
  BasicBlock *okBB = BasicBlock::Create(ctx_, "oob.ok", fn);
  builder_.CreateCondBr(oob, panicBB, okBB);
  builder_.SetInsertPoint(panicBB);
  builder_.CreateCall(module_->getFunction("kal_panic"), {});
  builder_.CreateUnreachable();
  builder_.SetInsertPoint(okBB);
}

Value *CodeGen::genVariable(const VariableExpr *e) {
  if (e->variantTag >= 0) // 引数なし enum バリアント
    return genVariant(e->type, e->variantTag, {});
  // 変数はメモリ常駐 (alloca)。読み出しは load。
  auto *slot = cast<AllocaInst>(namedValues_[e->name]);
  Value *val = builder_.CreateLoad(slot->getAllocatedType(), slot, e->name);
  // ムーブする使用なら、この変数のドロップフラグを下ろす (二重 drop 防止)。
  if (e->movesValue)
    for (auto &scope : dropScopes_)
      for (auto &d : scope)
        if (d.slot == slot)
          builder_.CreateStore(builder_.getInt1(false), d.flag);
  return val;
}

// 場所のアドレス。変数はその alloca、*p はポインタ値、フィールド/添字は GEP。
// 上記以外 (一時値) はメモリに退避してそのアドレスを返す。
Value *CodeGen::genAddr(const Expr *e) {
  switch (e->kind) {
  case Expr::Kind::Variable: {
    auto *v = static_cast<const VariableExpr *>(e);
    if (v->variantTag < 0) {
      auto it = namedValues_.find(v->name);
      if (it != namedValues_.end())
        return it->second;
    }
    break;
  }
  case Expr::Kind::Deref:
    return genExpr(static_cast<const DerefExpr *>(e)->operand.get());
  case Expr::Kind::Field: {
    auto *f = static_cast<const FieldExpr *>(e);
    Value *base;
    kal::Type structT;
    if (f->operand->type.isRef()) {
      base = genExpr(f->operand.get()); // 参照値 = struct へのポインタ
      structT = f->operand->type.pointee();
    } else {
      base = genAddr(f->operand.get());
      structT = f->operand->type;
    }
    StructType *st = getStructType(structT);
    return builder_.CreateStructGEP(st, base,
                                    static_cast<unsigned>(f->fieldIndex),
                                    "fieldptr");
  }
  case Expr::Kind::TupleIndex: {
    auto *t = static_cast<const TupleIndexExpr *>(e);
    Value *base = genAddr(t->operand.get());
    auto *tt = cast<StructType>(toLLVM(t->operand->type));
    return builder_.CreateStructGEP(tt, base, t->index, "telemptr");
  }
  case Expr::Kind::Index: {
    auto *ix = static_cast<const IndexExpr *>(e);
    Value *idx = genExpr(ix->index.get());
    llvm::Type *i64 = llvm::Type::getInt64Ty(ctx_);
    if (ix->base->type.isSlice() || ix->base->type.isVec() ||
        ix->base->type.isStringish()) {
      // スライス {ptr,len} / Vec {ptr,len,cap} / str・String {ptr,len,..}:
      // いずれも添字 0=ポインタ・1=長さ。先頭ポインタから要素型で GEP {idx}。
      Value *s = genExpr(ix->base.get());
      Value *data = builder_.CreateExtractValue(s, {0}, "dataptr");
      Value *len = builder_.CreateExtractValue(s, {1}, "datalen");
      emitBoundsCheck(idx, ix->index->type, len); // 境界チェック
      llvm::Type *elemTy =
          ix->base->type.isStringish()
              ? llvm::Type::getInt8Ty(ctx_) // str/String の要素は u8
              : toLLVM(ix->base->type.elemType());
      return builder_.CreateGEP(elemTy, data, {idx}, "elemptr");
    }
    // 配列 [N x T]: アドレスから GEP {0, idx}
    Value *base = genAddr(ix->base.get());
    Value *len = ConstantInt::get(i64, ix->base->type.arrayLen);
    emitBoundsCheck(idx, ix->index->type, len); // 境界チェック (N は定数)
    llvm::Type *at = toLLVM(ix->base->type);
    Value *zero = ConstantInt::get(i64, 0);
    return builder_.CreateGEP(at, base, {zero, idx}, "elemptr");
  }
  default:
    break;
  }
  // 一時値: メモリに退避
  Value *v = genExpr(e);
  if (blockDone())
    return nullptr; // 一時値の評価が発散
  AllocaInst *slot = entryAlloca(toLLVM(e->type), "tmp");
  builder_.CreateStore(v, slot);
  return slot;
}

Value *CodeGen::genBorrow(const BorrowExpr *e) {
  Value *addr = genAddr(e->operand.get());
  const kal::Type &ot = e->operand->type;
  if (ot.isArray()) {
    // 配列の借用 → スライス { 先頭ポインタ, 長さ }。
    // opaque pointer なので配列のアドレスがそのまま要素先頭を指す。
    llvm::Type *st = toLLVM(e->type); // { ptr, i64 }
    Value *agg = UndefValue::get(st);
    agg = builder_.CreateInsertValue(agg, addr, {0});
    agg = builder_.CreateInsertValue(
        agg, ConstantInt::get(llvm::Type::getInt64Ty(ctx_), ot.arrayLen), {1});
    return agg;
  }
  return addr; // 通常の参照 = アドレス
}

Value *CodeGen::genDeref(const DerefExpr *e) {
  Value *ptr = genExpr(e->operand.get());
  if (blockDone())
    return nullptr;
  Value *val = builder_.CreateLoad(toLLVM(e->type), ptr, "deref");
  // 箱の中身をムーブ取り出しした場合は、箱のヒープを解放する。
  if (e->movesOutOfBox)
    builder_.CreateCall(module_->getFunction("free"), {ptr});
  return val;
}

Value *CodeGen::genBinary(const BinaryExpr *e) {
  // 短絡論理: 右辺を条件付きで評価する
  if (e->op == Tok::AmpAmp || e->op == Tok::PipePipe) {
    bool isAnd = e->op == Tok::AmpAmp;
    Value *l = genExpr(e->lhs.get()); // i1
    if (blockDone())
      return nullptr; // lhs が発散
    Function *fn = builder_.GetInsertBlock()->getParent();
    BasicBlock *rhsBB = BasicBlock::Create(ctx_, "scrhs", fn);
    BasicBlock *contBB = BasicBlock::Create(ctx_, "sccont", fn);
    BasicBlock *lBB = builder_.GetInsertBlock();
    // &&: l が真なら rhs を、偽なら cont(=false)。|| はその逆。
    if (isAnd)
      builder_.CreateCondBr(l, rhsBB, contBB);
    else
      builder_.CreateCondBr(l, contBB, rhsBB);
    builder_.SetInsertPoint(rhsBB);
    Value *r = genExpr(e->rhs.get());
    bool rTerm = blockDone(); // rhs が発散したか
    BasicBlock *rEnd = builder_.GetInsertBlock();
    if (!rTerm)
      builder_.CreateBr(contBB);
    builder_.SetInsertPoint(contBB);
    PHINode *phi = builder_.CreatePHI(llvm::Type::getInt1Ty(ctx_), 2, "sctmp");
    // l からの到達時の値: && なら false、|| なら true
    phi->addIncoming(builder_.getInt1(!isAnd), lBB);
    if (!rTerm)
      phi->addIncoming(r, rEnd);
    return phi;
  }

  Value *l = genExpr(e->lhs.get());
  Value *r = genExpr(e->rhs.get());
  if (blockDone())
    return nullptr; // 被演算子が発散
  const kal::Type &ot = e->lhs->type; // 算術は両辺同型 (Sema 保証)

  // 文字列演算 (比較・連結)。str / String を混在できる。両辺は借用なので、
  // 被演算子が所有ヒープの一時値なら使用後に drop する (チェーン a+b+c の中間や
  // string("a")==string("b") の一時値がリークしないように)。
  bool strBin =
      (ot.isStringish() || e->rhs->type.isStringish()) &&
      (e->op == Tok::Plus || e->op == Tok::EqEq || e->op == Tok::BangEq ||
       e->op == Tok::Less || e->op == Tok::Greater || e->op == Tok::Le ||
       e->op == Tok::Ge);
  if (strBin) {
    llvm::Type *i32 = llvm::Type::getInt32Ty(ctx_);
    llvm::Type *i64 = llvm::Type::getInt64Ty(ctx_);
    llvm::Type *i8 = llvm::Type::getInt8Ty(ctx_);
    Value *p1 = builder_.CreateExtractValue(l, {0}, "p1");
    Value *n1 = builder_.CreateExtractValue(l, {1}, "n1");
    Value *p2 = builder_.CreateExtractValue(r, {0}, "p2");
    Value *n2 = builder_.CreateExtractValue(r, {1}, "n2");
    Value *res;
    if (e->op == Tok::Plus) {
      // 連結: malloc(n1+n2) に両辺をコピーして所有 String {buf, total, total} に
      Value *total = builder_.CreateAdd(n1, n2, "total");
      Value *buf =
          builder_.CreateCall(module_->getFunction("malloc"), {total}, "catbuf");
      builder_.CreateMemCpy(buf, MaybeAlign(1), p1, MaybeAlign(1), n1);
      Value *tail = builder_.CreateGEP(i8, buf, {n1}, "tail");
      builder_.CreateMemCpy(tail, MaybeAlign(1), p2, MaybeAlign(1), n2);
      StructType *vt = vecLLVMTy();
      Value *agg = UndefValue::get(vt);
      agg = builder_.CreateInsertValue(agg, buf, {0});
      agg = builder_.CreateInsertValue(agg, total, {1});
      agg = builder_.CreateInsertValue(agg, total, {2}); // cap = len
      res = agg;
    } else {
      // 比較 (バイト辞書順)
      Value *minlen = builder_.CreateSelect(builder_.CreateICmpULT(n1, n2), n1,
                                            n2, "minlen");
      Value *m = builder_.CreateCall(module_->getFunction("memcmp"),
                                     {p1, p2, minlen}, "memcmp");
      Constant *zero32 = ConstantInt::get(i32, 0);
      Constant *m1 = ConstantInt::get(i32, -1, /*signed=*/true);
      Constant *p1c = ConstantInt::get(i32, 1);
      Value *lenCmp = builder_.CreateSelect(
          builder_.CreateICmpULT(n1, n2), m1,
          builder_.CreateSelect(builder_.CreateICmpUGT(n1, n2), p1c, zero32),
          "lenCmp");
      Value *cmp = builder_.CreateSelect(
          builder_.CreateICmpSLT(m, zero32), m1,
          builder_.CreateSelect(builder_.CreateICmpSGT(m, zero32), p1c, lenCmp),
          "cmp");
      switch (e->op) {
      case Tok::EqEq: res = builder_.CreateICmpEQ(cmp, zero32, "streq"); break;
      case Tok::BangEq: res = builder_.CreateICmpNE(cmp, zero32, "strne"); break;
      case Tok::Less: res = builder_.CreateICmpSLT(cmp, zero32, "strlt"); break;
      case Tok::Le: res = builder_.CreateICmpSLE(cmp, zero32, "strle"); break;
      case Tok::Greater: res = builder_.CreateICmpSGT(cmp, zero32, "strgt"); break;
      default: res = builder_.CreateICmpSGE(cmp, zero32, "strge"); break;
      }
    }
    // 被演算子が所有 String の一時値 (場所式でない) なら解放する。
    dropDiscardedValue(e->lhs.get(), l);
    dropDiscardedValue(e->rhs.get(), r);
    return res;
  }

  bool isF = ot.isFloat();
  bool sgn = ot.isInt() ? ot.isSigned : true;

  switch (e->op) {
  case Tok::Plus:
    return isF ? builder_.CreateFAdd(l, r, "addtmp")
               : builder_.CreateAdd(l, r, "addtmp");
  case Tok::Minus:
    return isF ? builder_.CreateFSub(l, r, "subtmp")
               : builder_.CreateSub(l, r, "subtmp");
  case Tok::Star:
    return isF ? builder_.CreateFMul(l, r, "multmp")
               : builder_.CreateMul(l, r, "multmp");
  case Tok::Slash:
    if (isF)
      return builder_.CreateFDiv(l, r, "divtmp");
    return sgn ? builder_.CreateSDiv(l, r, "divtmp")
               : builder_.CreateUDiv(l, r, "divtmp");
  case Tok::Percent:
    if (isF)
      return builder_.CreateFRem(l, r, "remtmp");
    return sgn ? builder_.CreateSRem(l, r, "remtmp")
               : builder_.CreateURem(l, r, "remtmp");
  case Tok::Less:
    if (isF)
      return builder_.CreateFCmpOLT(l, r, "cmptmp");
    return sgn ? builder_.CreateICmpSLT(l, r, "cmptmp")
               : builder_.CreateICmpULT(l, r, "cmptmp");
  case Tok::Greater:
    if (isF)
      return builder_.CreateFCmpOGT(l, r, "cmptmp");
    return sgn ? builder_.CreateICmpSGT(l, r, "cmptmp")
               : builder_.CreateICmpUGT(l, r, "cmptmp");
  case Tok::Le:
    if (isF)
      return builder_.CreateFCmpOLE(l, r, "cmptmp");
    return sgn ? builder_.CreateICmpSLE(l, r, "cmptmp")
               : builder_.CreateICmpULE(l, r, "cmptmp");
  case Tok::Ge:
    if (isF)
      return builder_.CreateFCmpOGE(l, r, "cmptmp");
    return sgn ? builder_.CreateICmpSGE(l, r, "cmptmp")
               : builder_.CreateICmpUGE(l, r, "cmptmp");
  case Tok::EqEq:
    return isF ? builder_.CreateFCmpOEQ(l, r, "cmptmp")
               : builder_.CreateICmpEQ(l, r, "cmptmp");
  case Tok::BangEq:
    return isF ? builder_.CreateFCmpONE(l, r, "cmptmp")
               : builder_.CreateICmpNE(l, r, "cmptmp");
  default:
    return nullptr;
  }
}

Value *CodeGen::genUnary(const UnaryExpr *e) {
  Value *v = genExpr(e->operand.get());
  if (blockDone())
    return nullptr; // 被演算子が発散
  if (e->op == Tok::Bang)
    return builder_.CreateNot(v, "nottmp"); // bool (i1) 反転
  // 単項マイナス
  return e->operand->type.isFloat() ? builder_.CreateFNeg(v, "negtmp")
                                    : builder_.CreateNeg(v, "negtmp");
}

Value *CodeGen::genCall(const CallExpr *e) {
  // Vec 組み込みは引数の評価方法が特殊 (push は第1引数を場所として扱う)。
  if (e->isVecBuiltin)
    return genVecNew(e);
  if (e->isPushBuiltin)
    return genPush(e);
  if (e->isPopBuiltin)
    return genPop(e); // pop(v): v を場所として扱う
  if (e->isClearBuiltin)
    return genClear(e); // clear(v): v を場所として扱う
  if (e->isPushStrBuiltin)
    return genPushStr(e); // push_str(s, t): s を場所として扱う
  std::vector<Value *> args;
  for (auto &a : e->args)
    args.push_back(genExpr(a.get()));
  if (blockDone())
    return nullptr; // 引数の評価が発散
  // String→str に強制変換される引数を {ptr,len,cap} から str {ptr,len} へ縮める
  // (借用ビュー。元の String は呼び出し側が保持し続ける)。所有 String の一時値を
  // 渡した場合 (f(a+b) 等) は、呼び出し後に解放する (借用は呼び出し中のみ有効)。
  std::vector<std::pair<const Expr *, Value *>> coercedTemps;
  for (size_t i = 0; i < e->argCoercedToStr.size() && i < args.size(); ++i) {
    if (!e->argCoercedToStr[i] || !args[i])
      continue;
    coercedTemps.push_back({e->args[i].get(), args[i]}); // 元の String 値を保存
    Value *p = builder_.CreateExtractValue(args[i], {0}, "coerce.ptr");
    Value *n = builder_.CreateExtractValue(args[i], {1}, "coerce.len");
    auto *strTy = cast<StructType>(toLLVM(kal::Type::strTy()));
    Value *agg = UndefValue::get(strTy);
    agg = builder_.CreateInsertValue(agg, p, {0});
    agg = builder_.CreateInsertValue(agg, n, {1});
    args[i] = agg;
  }
  // 呼び出し後に coercion された一時 String を解放する (場所式は dropDiscardedValue
  // が借用としてスキップする)。
  auto dropCoercedTemps = [&]() {
    for (auto &ct : coercedTemps)
      dropDiscardedValue(ct.first, ct.second);
  };
  // 関連関数呼び出し Type::name(args): メソッドと同じく単態化して直接呼ぶ
  // (self なし)。型引数は Sema が推論した e->typeArgs を使う。
  if (!e->ownerType.empty()) {
    const FunctionDef *def = methodDefs_[e->ownerType].at(e->callee);
    std::vector<kal::Type> targs;
    for (auto &ta : e->typeArgs)
      targs.push_back(typeSubst_.empty() ? ta : substType(ta, typeSubst_));
    Function *callee = ensureInstance(def, targs);
    CallInst *call = builder_.CreateCall(callee, args);
    kal::Type rt = typeSubst_.empty() ? e->type : substType(e->type, typeSubst_);
    if (rt.isUnit())
      return nullptr;
    call->setName("assoctmp");
    return call;
  }
  if (e->variantTag >= 0) // enum バリアント構築
    return genVariant(e->type, e->variantTag, args);
  // 組み込み len(s): fat pointer {ptr, len} の len フィールドを取り出す
  // (スライス・Vec・str いずれも添字 1 が長さ)。引数は借用なので、所有ヒープの
  // 一時値 (len(a+b) / len(make_vec()) 等) なら使用後に解放する。
  if (e->isLenBuiltin) {
    Value *len = builder_.CreateExtractValue(args[0], {1}, "len");
    dropDiscardedValue(e->args[0].get(), args[0]);
    return len;
  }
  // 組み込み prints(s): str/String {ptr, len, ..} を分解して kal_prints を呼ぶ
  if (e->isPrintsBuiltin) {
    Value *data = builder_.CreateExtractValue(args[0], {0}, "strptr");
    Value *len = builder_.CreateExtractValue(args[0], {1}, "strlen");
    builder_.CreateCall(module_->getFunction("kal_prints"), {data, len});
    dropDiscardedValue(e->args[0].get(), args[0]); // 一時値 (prints(a+b) 等) を解放
    return nullptr; // unit
  }
  // 組み込み string(s): str のバイトをヒープにコピーして所有 String を作る
  if (e->isStringBuiltin) {
    llvm::Type *i64 = llvm::Type::getInt64Ty(ctx_);
    Value *src = builder_.CreateExtractValue(args[0], {0}, "srcptr");
    Value *len = builder_.CreateExtractValue(args[0], {1}, "srclen");
    Value *buf = builder_.CreateCall(module_->getFunction("malloc"), {len},
                                     "strbuf");
    builder_.CreateMemCpy(buf, MaybeAlign(1), src, MaybeAlign(1), len);
    StructType *vt = vecLLVMTy();
    Value *agg = UndefValue::get(vt);
    agg = builder_.CreateInsertValue(agg, buf, {0});
    agg = builder_.CreateInsertValue(agg, len, {1}); // len
    agg = builder_.CreateInsertValue(agg, len, {2}); // cap = len
    return agg;
  }
  // 組み込み box(e): ヒープに確保して中身を書き込み、ポインタ (Box) を返す
  if (e->isBoxBuiltin) {
    llvm::Type *elemTy = toLLVM(e->args[0]->type); // typeSubst_ で具体化済み
    uint64_t sz = dl_.getTypeAllocSize(elemTy).getFixedValue();
    Value *raw = builder_.CreateCall(
        module_->getFunction("malloc"),
        {ConstantInt::get(llvm::Type::getInt64Ty(ctx_), sz)}, "boxmem");
    builder_.CreateStore(args[0], raw);
    return raw;
  }
  // ジェネリック関数呼び出し: 型引数を (単態化中なら) 具体化して呼ぶ
  auto git = genericFuncDefs_.find(e->callee);
  if (git != genericFuncDefs_.end()) {
    std::vector<kal::Type> targs;
    for (auto &ta : e->typeArgs)
      targs.push_back(typeSubst_.empty() ? ta : substType(ta, typeSubst_));
    Function *callee = ensureInstance(git->second, targs);
    CallInst *call = builder_.CreateCall(callee, args);
    kal::Type rt = typeSubst_.empty() ? e->type : substType(e->type, typeSubst_);
    dropCoercedTemps();
    if (rt.isUnit())
      return nullptr;
    call->setName("calltmp");
    return call;
  }
  Function *callee = module_->getFunction(e->callee);
  CallInst *call = builder_.CreateCall(callee, args);
  dropCoercedTemps();
  if (e->type.isUnit())
    return nullptr;
  call->setName("calltmp");
  return call;
}

Value *CodeGen::genMethodCall(const MethodCallExpr *e) {
  // レシーバの実型 (単態化後) からオーナー型と型引数を導出する。
  // これによりトレイト境界経由の呼び出し (Param → 実型) も正しく解決される。
  kal::Type recvT = typeSubst_.empty() ? e->receiver->type
                                       : substType(e->receiver->type, typeSubst_);
  kal::Type baseT = recvT.isRef() ? recvT.pointee() : recvT;
  const FunctionDef *def = methodDefs_[baseT.name].at(e->method);
  std::vector<kal::Type> targs = baseT.elems; // 具体化済みの型引数

  // self 引数を構築: 値レシーバはムーブ、&self/&mut self はポインタを渡す。
  Value *selfArg;
  if (e->selfKind == 0)
    selfArg = genExpr(e->receiver.get()); // 値レシーバ
  else if (e->recvIsRef)
    selfArg = genExpr(e->receiver.get()); // 既に参照: そのポインタ値
  else
    selfArg = genAddr(e->receiver.get()); // 値の場所を借用
  std::vector<Value *> args;
  args.push_back(selfArg);
  for (auto &a : e->args)
    args.push_back(genExpr(a.get()));
  if (blockDone())
    return nullptr; // レシーバ/引数の評価が発散
  Function *callee = ensureInstance(def, targs);
  CallInst *call = builder_.CreateCall(callee, args);
  kal::Type rt = typeSubst_.empty() ? e->type : substType(e->type, typeSubst_);
  if (rt.isUnit())
    return nullptr;
  call->setName("methtmp");
  return call;
}

// ドロップグルー drop_<t>(ptr p): p が指す値 (型 t) を解放する。
void CodeGen::genDropBody(const kal::Type &t, llvm::Function *fn) {
  builder_.SetInsertPoint(BasicBlock::Create(ctx_, "entry", fn));
  Value *p = fn->getArg(0);
  llvm::Type *i64 = llvm::Type::getInt64Ty(ctx_);

  if (t.isBox()) {
    // 中身を drop してからヒープを free する
    Value *bp = builder_.CreateLoad(PointerType::getUnqual(ctx_), p, "boxptr");
    if (needsDrop(t.boxedType()))
      builder_.CreateCall(getDropFn(t.boxedType()), {bp});
    builder_.CreateCall(module_->getFunction("free"), {bp});
  } else if (t.isVec()) {
    // 生存要素 (0..len) を drop してからバッファを free する
    StructType *vt = vecLLVMTy();
    llvm::Type *ptrTy = PointerType::getUnqual(ctx_);
    Value *data = builder_.CreateLoad(
        ptrTy, builder_.CreateStructGEP(vt, p, 0, "dataptr"), "data");
    const kal::Type &et = t.elemType();
    if (needsDrop(et)) {
      Value *len = builder_.CreateLoad(
          i64, builder_.CreateStructGEP(vt, p, 1, "lenptr"), "len");
      llvm::Type *elemTy = toLLVM(et);
      AllocaInst *iSlot = builder_.CreateAlloca(i64, nullptr, "i");
      builder_.CreateStore(ConstantInt::get(i64, 0), iSlot);
      BasicBlock *cond = BasicBlock::Create(ctx_, "vdrop.cond", fn);
      BasicBlock *body = BasicBlock::Create(ctx_, "vdrop.body", fn);
      BasicBlock *end = BasicBlock::Create(ctx_, "vdrop.end", fn);
      builder_.CreateBr(cond);
      builder_.SetInsertPoint(cond);
      Value *i = builder_.CreateLoad(i64, iSlot, "i");
      builder_.CreateCondBr(builder_.CreateICmpULT(i, len, "more"), body, end);
      builder_.SetInsertPoint(body);
      Value *ep = builder_.CreateGEP(elemTy, data, {i}, "elemptr");
      builder_.CreateCall(getDropFn(et), {ep});
      builder_.CreateStore(
          builder_.CreateAdd(i, ConstantInt::get(i64, 1), "inext"), iSlot);
      builder_.CreateBr(cond);
      builder_.SetInsertPoint(end);
    }
    builder_.CreateCall(module_->getFunction("free"), {data}); // free(null) は安全
  } else if (t.isString()) {
    // String はバイト列 (drop 不要の u8)。バッファを free するだけ。
    StructType *vt = vecLLVMTy();
    Value *data = builder_.CreateLoad(
        PointerType::getUnqual(ctx_), builder_.CreateStructGEP(vt, p, 0, "sp"),
        "sdata");
    builder_.CreateCall(module_->getFunction("free"), {data});
  } else if (t.isTuple() || t.isArray()) {
    llvm::Type *aggTy = toLLVM(t);
    size_t n = t.isArray() ? t.arrayLen : t.elems.size();
    for (size_t i = 0; i < n; ++i) {
      const kal::Type &et = t.isArray() ? t.elems[0] : t.elems[i];
      if (!needsDrop(et))
        continue;
      Value *ep =
          t.isArray()
              ? builder_.CreateGEP(aggTy, p,
                                   {ConstantInt::get(i64, 0),
                                    ConstantInt::get(i64, i)}, "elemptr")
              : builder_.CreateStructGEP(cast<StructType>(aggTy), p,
                                         static_cast<unsigned>(i), "telemptr");
      builder_.CreateCall(getDropFn(et), {ep});
    }
  } else if (t.isStruct()) {
    const StructDef *sd = structDefs_[t.name];
    auto sub = instSubst(sd->typeParams, t.elems);
    StructType *st = getStructType(t);
    for (size_t i = 0; i < sd->fields.size(); ++i) {
      kal::Type ft = substType(sd->fields[i].type, sub);
      if (!needsDrop(ft))
        continue;
      Value *fp =
          builder_.CreateStructGEP(st, p, static_cast<unsigned>(i), "fieldptr");
      builder_.CreateCall(getDropFn(ft), {fp});
    }
  } else if (t.isEnum()) {
    const EnumDef *ed = enumDefs_[t.name];
    auto sub = instSubst(ed->typeParams, t.elems);
    StructType *et = getEnumType(t);
    Value *tag = builder_.CreateLoad(
        i64, builder_.CreateStructGEP(et, p, 0, "tagptr"), "tag");
    Value *payPtr = builder_.CreateStructGEP(et, p, 1, "payptr");
    BasicBlock *endBB = BasicBlock::Create(ctx_, "dropend", fn);
    // ペイロードに drop が要るバリアントごとに分岐して中身を drop
    std::vector<std::pair<int, BasicBlock *>> cases;
    for (size_t v = 0; v < ed->variants.size(); ++v) {
      bool any = false;
      for (auto &pt : ed->variants[v].payloadTypes)
        if (needsDrop(substType(pt, sub)))
          any = true;
      if (!any)
        continue;
      BasicBlock *bb = BasicBlock::Create(ctx_, "dropvar", fn);
      cases.push_back({static_cast<int>(v), bb});
      builder_.SetInsertPoint(bb);
      std::vector<llvm::Type *> fts;
      for (auto &pt : ed->variants[v].payloadTypes)
        fts.push_back(toLLVM(substType(pt, sub)));
      StructType *vs = StructType::get(ctx_, fts);
      for (size_t j = 0; j < ed->variants[v].payloadTypes.size(); ++j) {
        kal::Type pt = substType(ed->variants[v].payloadTypes[j], sub);
        if (!needsDrop(pt))
          continue;
        Value *fp = builder_.CreateStructGEP(vs, payPtr,
                                             static_cast<unsigned>(j), "pf");
        builder_.CreateCall(getDropFn(pt), {fp});
      }
      builder_.CreateBr(endBB);
    }
    // entry に switch を張る (どこにも該当しなければ end へ)
    Function *cur = fn;
    builder_.SetInsertPoint(&cur->getEntryBlock());
    SwitchInst *sw = builder_.CreateSwitch(tag, endBB, cases.size());
    for (auto &c : cases)
      sw->addCase(builder_.getInt64(c.first), c.second);
    builder_.SetInsertPoint(endBB);
  }

  builder_.CreateRetVoid();
  verifyFunction(*fn);
}

// ドロップ対象なら i1 フラグ (初期 true) を作って現スコープに登録する。
void CodeGen::registerLocal(llvm::Value *slot, const kal::Type &rawT) {
  if (dropScopes_.empty())
    return;
  kal::Type t = typeSubst_.empty() ? rawT : substType(rawT, typeSubst_);
  if (!needsDrop(t))
    return;
  Value *flag = entryAlloca(llvm::Type::getInt1Ty(ctx_), "live");
  builder_.CreateStore(builder_.getInt1(true), flag);
  dropScopes_.back().push_back({slot, flag, t});
}

// flag が立っていれば slot の値を drop する。
void CodeGen::emitDrop(llvm::Value *slot, llvm::Value *flag, const kal::Type &t) {
  Function *fn = builder_.GetInsertBlock()->getParent();
  BasicBlock *doBB = BasicBlock::Create(ctx_, "drop.do", fn);
  BasicBlock *contBB = BasicBlock::Create(ctx_, "drop.cont", fn);
  Value *live = builder_.CreateLoad(llvm::Type::getInt1Ty(ctx_), flag, "islive");
  builder_.CreateCondBr(live, doBB, contBB);
  builder_.SetInsertPoint(doBB);
  builder_.CreateCall(getDropFn(t), {slot});
  builder_.CreateStore(builder_.getInt1(false), flag); // 二重 drop 防止
  builder_.CreateBr(contBB);
  builder_.SetInsertPoint(contBB);
}

void CodeGen::popDropScope() {
  if (dropScopes_.empty())
    return;
  auto scope = std::move(dropScopes_.back());
  dropScopes_.pop_back();
  if (blockDone())
    return; // 既に発散 (return がスコープを drop 済み)
  for (auto it = scope.rbegin(); it != scope.rend(); ++it)
    emitDrop(it->slot, it->flag, it->type);
}

void CodeGen::dropAllScopesForExit() {
  // return 用: 内側から全スコープの生存ローカルを drop (pop はしない)
  for (auto sit = dropScopes_.rbegin(); sit != dropScopes_.rend(); ++sit)
    for (auto it = sit->rbegin(); it != sit->rend(); ++it)
      emitDrop(it->slot, it->flag, it->type);
}

Value *CodeGen::genReturn(const ReturnExpr *e) {
  if (e->value) {
    Value *v = genExpr(e->value.get());
    if (blockDone())
      return nullptr; // 値の評価自体が発散した
    if (currentRetSlot_ && v)
      builder_.CreateStore(v, currentRetSlot_);
  }
  dropAllScopesForExit(); // 全スコープの生存ローカルを drop してから抜ける
  // ブロックを終端する (afterret は作らない → blockDone() が正しく true を返す)
  builder_.CreateBr(currentRetBlock_);
  return nullptr;
}

// `e?`: prelude の Option/Result はタグ 0=成功(Some/Ok)・1=失敗(None/Err)。
// 成功なら中身を取り出して継続、失敗なら関数から早期リターンする。
Value *CodeGen::genTry(const TryExpr *e) {
  Value *v = genExpr(e->operand.get());
  kal::Type ot = typeSubst_.empty()
                     ? e->operand->type
                     : substType(e->operand->type, typeSubst_);
  StructType *et = getEnumType(ot);
  llvm::Type *i64 = llvm::Type::getInt64Ty(ctx_);

  AllocaInst *slot = builder_.CreateAlloca(et);
  slot->setAlignment(Align(8));
  builder_.CreateStore(v, slot);
  Value *tag = builder_.CreateLoad(
      i64, builder_.CreateStructGEP(et, slot, 0, "tagptr"), "tag");

  Function *fn = builder_.GetInsertBlock()->getParent();
  BasicBlock *okBB = BasicBlock::Create(ctx_, "try.ok", fn);
  BasicBlock *errBB = BasicBlock::Create(ctx_, "try.err", fn);
  Value *isOk = builder_.CreateICmpEQ(tag, ConstantInt::get(i64, 0), "isok");
  builder_.CreateCondBr(isOk, okBB, errBB);

  // 失敗パス: None / Err(payload) を作って早期リターン
  builder_.SetInsertPoint(errBB);
  if (e->kind == 0) { // Option → None
    Value *none = genVariant(currentRetTypeCG_, 1, {});
    if (currentRetSlot_)
      builder_.CreateStore(none, currentRetSlot_);
  } else { // Result → Err(e)
    llvm::Type *eTy = toLLVM(ot.elems[1]);
    StructType *vs = StructType::get(ctx_, ArrayRef<llvm::Type *>{eTy});
    Value *pv = builder_.CreateLoad(
        vs, builder_.CreateStructGEP(et, slot, 1, "errptr"), "errld");
    Value *errVal = builder_.CreateExtractValue(pv, {0u});
    Value *errEnum = genVariant(currentRetTypeCG_, 1, {errVal});
    if (currentRetSlot_)
      builder_.CreateStore(errEnum, currentRetSlot_);
  }
  // `?` の早期リターンも通常の return と同じく、生存中のローカルを drop する。
  dropAllScopesForExit();
  builder_.CreateBr(currentRetBlock_);

  // 成功パス: 中身を取り出して `?` 式の値とする
  builder_.SetInsertPoint(okBB);
  llvm::Type *innerTy = toLLVM(ot.elems[0]);
  StructType *okVs = StructType::get(ctx_, ArrayRef<llvm::Type *>{innerTy});
  Value *pv = builder_.CreateLoad(
      okVs, builder_.CreateStructGEP(et, slot, 1, "okptr"), "okld");
  return builder_.CreateExtractValue(pv, {0u}, "tryval");
}

// 文字列リテラル: バイト列を読み取り専用グローバルに置き、{ptr, len} を返す。
Value *CodeGen::genStringLit(const StringLitExpr *e) {
  llvm::Type *i64 = llvm::Type::getInt64Ty(ctx_);
  Constant *data = ConstantDataArray::getString(ctx_, e->value,
                                                /*AddNull=*/false);
  auto *gv = new GlobalVariable(*module_, data->getType(), /*isConstant=*/true,
                                GlobalValue::PrivateLinkage, data, "str");
  gv->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
  StructType *st = cast<StructType>(toLLVM(kal::Type::strTy())); // { ptr, i64 }
  Value *agg = UndefValue::get(st);
  agg = builder_.CreateInsertValue(agg, gv, {0}); // 不透明 ptr = 先頭バイト
  agg = builder_.CreateInsertValue(
      agg, ConstantInt::get(i64, e->value.size()), {1});
  return agg;
}

// 組み込み vec(): 空の Vec { null, 0, 0 } を返す (容量は最初の push で確保)。
Value *CodeGen::genVecNew(const CallExpr *e) {
  (void)e;
  StructType *vt = vecLLVMTy();
  llvm::Type *i64 = llvm::Type::getInt64Ty(ctx_);
  Constant *z = ConstantInt::get(i64, 0);
  return ConstantStruct::get(
      vt, {ConstantPointerNull::get(PointerType::getUnqual(ctx_)), z, z});
}

// 組み込み push(v, x): v は可変な場所。容量が足りなければ倍々で realloc し、
// 末尾に x を書き込んで長さを 1 増やす。値の位置としては unit。
Value *CodeGen::genPush(const CallExpr *e) {
  Value *vecPtr = genAddr(e->args[0].get()); // Vec{ptr,len,cap} の場所
  if (blockDone())
    return nullptr;
  Value *x = genExpr(e->args[1].get()); // push する値 (ムーブで入る)
  if (blockDone())
    return nullptr;
  llvm::Type *i64 = llvm::Type::getInt64Ty(ctx_);
  llvm::Type *ptrTy = PointerType::getUnqual(ctx_);
  StructType *vt = vecLLVMTy();
  kal::Type elemT = typeSubst_.empty()
                        ? e->args[1]->type
                        : substType(e->args[1]->type, typeSubst_);
  llvm::Type *elemTy = toLLVM(elemT);
  uint64_t esz = dl_.getTypeAllocSize(elemTy).getFixedValue();

  Value *dataPtr = builder_.CreateStructGEP(vt, vecPtr, 0, "dataptr");
  Value *lenPtr = builder_.CreateStructGEP(vt, vecPtr, 1, "lenptr");
  Value *capPtr = builder_.CreateStructGEP(vt, vecPtr, 2, "capptr");
  Value *len = builder_.CreateLoad(i64, lenPtr, "len");
  Value *cap = builder_.CreateLoad(i64, capPtr, "cap");

  Function *fn = builder_.GetInsertBlock()->getParent();
  BasicBlock *growBB = BasicBlock::Create(ctx_, "vec.grow", fn);
  BasicBlock *contBB = BasicBlock::Create(ctx_, "vec.cont", fn);
  builder_.CreateCondBr(builder_.CreateICmpEQ(len, cap, "full"), growBB, contBB);

  // 成長: 容量 0 なら 4、そうでなければ 2 倍にして realloc する。
  builder_.SetInsertPoint(growBB);
  Value *isZero = builder_.CreateICmpEQ(cap, ConstantInt::get(i64, 0), "cap0");
  Value *newCap = builder_.CreateSelect(
      isZero, ConstantInt::get(i64, 4),
      builder_.CreateMul(cap, ConstantInt::get(i64, 2), "cap2"), "newcap");
  Value *bytes =
      builder_.CreateMul(newCap, ConstantInt::get(i64, esz), "bytes");
  Value *oldData = builder_.CreateLoad(ptrTy, dataPtr, "olddata");
  Value *newData = builder_.CreateCall(module_->getFunction("realloc"),
                                       {oldData, bytes}, "newdata");
  builder_.CreateStore(newData, dataPtr);
  builder_.CreateStore(newCap, capPtr);
  builder_.CreateBr(contBB);

  // 追記: data[len] = x; len += 1
  builder_.SetInsertPoint(contBB);
  Value *data = builder_.CreateLoad(ptrTy, dataPtr, "data");
  Value *slot = builder_.CreateGEP(elemTy, data, {len}, "slot");
  builder_.CreateStore(x, slot);
  builder_.CreateStore(builder_.CreateAdd(len, ConstantInt::get(i64, 1), "len1"),
                       lenPtr);
  return nullptr; // unit
}

// 組み込み push_str(s, t): 可変 String s の末尾に str t のバイトを追記する。
// 容量が足りなければ「現容量×2」と「必要量」の大きい方へ realloc する。
Value *CodeGen::genPushStr(const CallExpr *e) {
  Value *sPtr = genAddr(e->args[0].get()); // String{ptr,len,cap} の場所
  if (blockDone())
    return nullptr;
  Value *t = genExpr(e->args[1].get()); // 追記する str {ptr, len}
  if (blockDone())
    return nullptr;
  llvm::Type *i64 = llvm::Type::getInt64Ty(ctx_);
  llvm::Type *ptrTy = PointerType::getUnqual(ctx_);
  StructType *vt = vecLLVMTy();
  Value *tData = builder_.CreateExtractValue(t, {0}, "tdata");
  Value *tLen = builder_.CreateExtractValue(t, {1}, "tlen");

  Value *dataPtr = builder_.CreateStructGEP(vt, sPtr, 0, "sdata");
  Value *lenPtr = builder_.CreateStructGEP(vt, sPtr, 1, "slen");
  Value *capPtr = builder_.CreateStructGEP(vt, sPtr, 2, "scap");
  Value *len = builder_.CreateLoad(i64, lenPtr, "len");
  Value *cap = builder_.CreateLoad(i64, capPtr, "cap");
  Value *need = builder_.CreateAdd(len, tLen, "need"); // 追記後の必要長

  llvm::Type *i8 = llvm::Type::getInt8Ty(ctx_);
  Function *fn = builder_.GetInsertBlock()->getParent();
  BasicBlock *growBB = BasicBlock::Create(ctx_, "str.grow", fn);
  BasicBlock *inplaceBB = BasicBlock::Create(ctx_, "str.inplace", fn);
  BasicBlock *doneBB = BasicBlock::Create(ctx_, "str.done", fn);
  builder_.CreateCondBr(builder_.CreateICmpUGT(need, cap, "tooSmall"), growBB,
                        inplaceBB);

  // 成長パス: max(cap*2, need) で新バッファを malloc し、既存内容と追記分を
  // コピーしてから旧バッファを free する。realloc を使わないのは、第 2 引数 t が
  // 旧バッファを指す自己追記 (push_str(s, s)) で、free 前に t を読むため
  // (realloc だと free 済みバッファを読む use-after-free になる)。
  builder_.SetInsertPoint(growBB);
  Value *dbl = builder_.CreateMul(cap, ConstantInt::get(i64, 2), "cap2");
  Value *bigger = builder_.CreateICmpUGT(need, dbl, "needBigger");
  Value *newCap = builder_.CreateSelect(bigger, need, dbl, "newcap");
  Value *oldData = builder_.CreateLoad(ptrTy, dataPtr, "olddata");
  Value *newBuf =
      builder_.CreateCall(module_->getFunction("malloc"), {newCap}, "newbuf");
  builder_.CreateMemCpy(newBuf, MaybeAlign(1), oldData, MaybeAlign(1), len);
  Value *gtail = builder_.CreateGEP(i8, newBuf, {len}, "gtail");
  builder_.CreateMemCpy(gtail, MaybeAlign(1), tData, MaybeAlign(1), tLen);
  builder_.CreateCall(module_->getFunction("free"), {oldData}); // 旧バッファ解放
  builder_.CreateStore(newBuf, dataPtr);
  builder_.CreateStore(newCap, capPtr);
  builder_.CreateStore(need, lenPtr);
  builder_.CreateBr(doneBB);

  // 容量内パス: 末尾にそのまま追記 (dst は現内容の後ろなので t と重ならない)。
  builder_.SetInsertPoint(inplaceBB);
  Value *data = builder_.CreateLoad(ptrTy, dataPtr, "data");
  Value *dst = builder_.CreateGEP(i8, data, {len}, "tail");
  builder_.CreateMemCpy(dst, MaybeAlign(1), tData, MaybeAlign(1), tLen);
  builder_.CreateStore(need, lenPtr);
  builder_.CreateBr(doneBB);

  builder_.SetInsertPoint(doneBB);
  // 第 2 引数が所有 String の一時値 (push_str(s, a+b) 等) なら追記後に解放する。
  dropDiscardedValue(e->args[1].get(), t);
  return nullptr; // unit
}

// 組み込み pop(v): 末尾要素を取り出して Option<T> で返す。
// 空なら None。非空なら len を 1 減らし (= 要素の所有権を呼び出し側へ移す。
// len 外になるので Vec の Drop はその要素に触れない)、Some(elem) を返す。
Value *CodeGen::genPop(const CallExpr *e) {
  Value *vecPtr = genAddr(e->args[0].get()); // Vec{ptr,len,cap} の場所
  if (blockDone())
    return nullptr;
  llvm::Type *i64 = llvm::Type::getInt64Ty(ctx_);
  llvm::Type *ptrTy = PointerType::getUnqual(ctx_);
  StructType *vt = vecLLVMTy();
  // 結果型 Option<T> (単態化中なら具体化済み)、要素型 T はその型引数。
  kal::Type resT =
      typeSubst_.empty() ? e->type : substType(e->type, typeSubst_);
  llvm::Type *elemTy = toLLVM(resT.elems[0]);

  Value *lenPtr = builder_.CreateStructGEP(vt, vecPtr, 1, "lenptr");
  Value *len = builder_.CreateLoad(i64, lenPtr, "len");

  Function *fn = builder_.GetInsertBlock()->getParent();
  BasicBlock *emptyBB = BasicBlock::Create(ctx_, "pop.empty", fn);
  BasicBlock *someBB = BasicBlock::Create(ctx_, "pop.some", fn);
  BasicBlock *contBB = BasicBlock::Create(ctx_, "pop.cont", fn);
  Value *isEmpty =
      builder_.CreateICmpEQ(len, ConstantInt::get(i64, 0), "empty");
  builder_.CreateCondBr(isEmpty, emptyBB, someBB);

  // 空: None
  builder_.SetInsertPoint(emptyBB);
  Value *none = genVariant(resT, 1, {});
  BasicBlock *emptyEnd = builder_.GetInsertBlock();
  builder_.CreateBr(contBB);

  // 非空: len-1 にして data[len-1] を読み、Some(elem)
  builder_.SetInsertPoint(someBB);
  Value *newlen = builder_.CreateSub(len, ConstantInt::get(i64, 1), "newlen");
  builder_.CreateStore(newlen, lenPtr);
  Value *data = builder_.CreateLoad(
      ptrTy, builder_.CreateStructGEP(vt, vecPtr, 0, "dataptr"), "data");
  Value *ep = builder_.CreateGEP(elemTy, data, {newlen}, "elemptr");
  Value *elem = builder_.CreateLoad(elemTy, ep, "popped");
  Value *some = genVariant(resT, 0, {elem});
  BasicBlock *someEnd = builder_.GetInsertBlock();
  builder_.CreateBr(contBB);

  builder_.SetInsertPoint(contBB);
  PHINode *phi = builder_.CreatePHI(none->getType(), 2, "popopt");
  phi->addIncoming(none, emptyEnd);
  phi->addIncoming(some, someEnd);
  return phi;
}

// 組み込み clear(v): 生存要素を drop してから長さを 0 にする (容量は保持)。
Value *CodeGen::genClear(const CallExpr *e) {
  Value *vecPtr = genAddr(e->args[0].get());
  if (blockDone())
    return nullptr;
  llvm::Type *i64 = llvm::Type::getInt64Ty(ctx_);
  llvm::Type *ptrTy = PointerType::getUnqual(ctx_);
  StructType *vt = vecLLVMTy();
  kal::Type vecT =
      typeSubst_.empty() ? e->args[0]->type
                         : substType(e->args[0]->type, typeSubst_);
  kal::Type elemT = vecT.elemType();

  Value *lenPtr = builder_.CreateStructGEP(vt, vecPtr, 1, "lenptr");
  if (needsDrop(elemT)) {
    Value *len = builder_.CreateLoad(i64, lenPtr, "len");
    Value *data = builder_.CreateLoad(
        ptrTy, builder_.CreateStructGEP(vt, vecPtr, 0, "dataptr"), "data");
    llvm::Type *elemTy = toLLVM(elemT);
    Function *fn = builder_.GetInsertBlock()->getParent();
    AllocaInst *iSlot = builder_.CreateAlloca(i64, nullptr, "i");
    builder_.CreateStore(ConstantInt::get(i64, 0), iSlot);
    BasicBlock *cond = BasicBlock::Create(ctx_, "clr.cond", fn);
    BasicBlock *body = BasicBlock::Create(ctx_, "clr.body", fn);
    BasicBlock *end = BasicBlock::Create(ctx_, "clr.end", fn);
    builder_.CreateBr(cond);
    builder_.SetInsertPoint(cond);
    Value *i = builder_.CreateLoad(i64, iSlot, "i");
    builder_.CreateCondBr(builder_.CreateICmpULT(i, len, "more"), body, end);
    builder_.SetInsertPoint(body);
    Value *ep = builder_.CreateGEP(elemTy, data, {i}, "elemptr");
    builder_.CreateCall(getDropFn(elemT), {ep});
    builder_.CreateStore(builder_.CreateAdd(i, ConstantInt::get(i64, 1)), iSlot);
    builder_.CreateBr(cond);
    builder_.SetInsertPoint(end);
  }
  builder_.CreateStore(ConstantInt::get(i64, 0), lenPtr); // 長さ 0 (容量は保持)
  return nullptr;                                          // unit
}

Value *CodeGen::genCast(const CastExpr *e) {
  Value *v = genExpr(e->operand.get());
  if (blockDone())
    return nullptr;
  const kal::Type &src = e->operand->type;
  const kal::Type &dst = e->targetType;
  if (src == dst)
    return v;
  llvm::Type *dstTy = toLLVM(dst);

  if (src.isInt() && dst.isInt())
    return builder_.CreateIntCast(v, dstTy, src.isSigned, "cast");
  if (src.isFloat() && dst.isFloat())
    return builder_.CreateFPCast(v, dstTy, "cast");
  if (src.isInt() && dst.isFloat())
    return src.isSigned ? builder_.CreateSIToFP(v, dstTy, "cast")
                        : builder_.CreateUIToFP(v, dstTy, "cast");
  if (src.isFloat() && dst.isInt())
    return dst.isSigned ? builder_.CreateFPToSI(v, dstTy, "cast")
                        : builder_.CreateFPToUI(v, dstTy, "cast");
  if (src.isBool() && dst.isInt())
    return builder_.CreateZExt(v, dstTy, "cast");
  if (src.isBool() && dst.isFloat())
    return builder_.CreateUIToFP(v, dstTy, "cast");
  if (src.isInt() && dst.isBool())
    return builder_.CreateICmpNE(v, ConstantInt::get(toLLVM(src), 0), "cast");
  return v;
}

Value *CodeGen::genIf(const IfExpr *e) {
  Value *condV = genExpr(e->cond.get()); // 既に i1 (bool)
  if (blockDone())
    return nullptr; // 条件式が発散
  Function *fn = builder_.GetInsertBlock()->getParent();

  // else 省略 = 文としての if (値は unit)
  if (!e->els) {
    BasicBlock *thenBB = BasicBlock::Create(ctx_, "then", fn);
    BasicBlock *mergeBB = BasicBlock::Create(ctx_, "ifcont");
    builder_.CreateCondBr(condV, thenBB, mergeBB);
    builder_.SetInsertPoint(thenBB);
    genExpr(e->then.get());
    if (!blockDone())
      builder_.CreateBr(mergeBB);
    fn->insert(fn->end(), mergeBB);
    builder_.SetInsertPoint(mergeBB);
    return nullptr;
  }

  BasicBlock *thenBB = BasicBlock::Create(ctx_, "then", fn);
  BasicBlock *elseBB = BasicBlock::Create(ctx_, "else");
  BasicBlock *mergeBB = BasicBlock::Create(ctx_, "ifcont");

  builder_.CreateCondBr(condV, thenBB, elseBB);

  builder_.SetInsertPoint(thenBB);
  Value *thenV = genExpr(e->then.get());
  bool thenTerm = blockDone(); // then 分岐が発散したか
  if (!thenTerm)
    builder_.CreateBr(mergeBB);
  thenBB = builder_.GetInsertBlock();

  fn->insert(fn->end(), elseBB);
  builder_.SetInsertPoint(elseBB);
  Value *elseV = genExpr(e->els.get());
  bool elseTerm = blockDone();
  if (!elseTerm)
    builder_.CreateBr(mergeBB);
  elseBB = builder_.GetInsertBlock();

  fn->insert(fn->end(), mergeBB);
  builder_.SetInsertPoint(mergeBB);

  if (thenTerm && elseTerm) {
    builder_.CreateUnreachable(); // 両分岐とも発散 → merge は到達不能
    return nullptr;
  }
  // unit、または片方が発散して型が未知なら値なし
  if (e->type.isUnit() || !e->type.isKnown())
    return nullptr;

  PHINode *phi = builder_.CreatePHI(toLLVM(e->type), 2, "iftmp");
  if (!thenTerm)
    phi->addIncoming(thenV, thenBB);
  if (!elseTerm)
    phi->addIncoming(elseV, elseBB);
  return phi;
}

Value *CodeGen::genFor(const ForExpr *e) {
  const kal::Type &vt = e->start->type; // ループ変数の型
  Value *startV = genExpr(e->start.get());
  if (blockDone())
    return nullptr; // 開始値が発散

  // ループ変数はメモリ常駐 (アドレスを取れる)
  AllocaInst *slot = entryAlloca(toLLVM(vt), e->var);
  builder_.CreateStore(startV, slot);
  Value *oldVal = namedValues_.count(e->var) ? namedValues_[e->var] : nullptr;
  namedValues_[e->var] = slot;

  Function *fn = builder_.GetInsertBlock()->getParent();
  BasicBlock *condBB = BasicBlock::Create(ctx_, "loopcond", fn);
  BasicBlock *bodyBB = BasicBlock::Create(ctx_, "loopbody", fn);
  BasicBlock *afterBB = BasicBlock::Create(ctx_, "afterloop", fn);

  builder_.CreateBr(condBB);

  builder_.SetInsertPoint(condBB);
  Value *condV = genExpr(e->cond.get()); // i1 (変数は load される)
  builder_.CreateCondBr(condV, bodyBB, afterBB);

  builder_.SetInsertPoint(bodyBB);
  Value *bodyV = genExpr(e->body.get()); // 値は捨てる
  dropDiscardedValue(e->body.get(), bodyV); // 本体の一時値が所有ヒープなら解放

  // 本体が発散 (return) していなければ、加算してループ先頭へ戻る
  if (!blockDone()) {
    Value *stepV;
    if (e->step)
      stepV = genExpr(e->step.get());
    else if (vt.isFloat())
      stepV = ConstantFP::get(toLLVM(vt), 1.0);
    else
      stepV = ConstantInt::get(toLLVM(vt), 1, vt.isSigned);

    Value *cur = builder_.CreateLoad(toLLVM(vt), slot, e->var);
    Value *nextVar = vt.isFloat() ? builder_.CreateFAdd(cur, stepV, "nextvar")
                                  : builder_.CreateAdd(cur, stepV, "nextvar");
    builder_.CreateStore(nextVar, slot);
    builder_.CreateBr(condBB);
  }

  builder_.SetInsertPoint(afterBB);

  if (oldVal)
    namedValues_[e->var] = oldVal;
  else
    namedValues_.erase(e->var);

  return nullptr; // for は unit
}

Function *CodeGen::declareProto(const Prototype &p) {
  std::vector<llvm::Type *> params;
  for (auto &pt : p.paramTypes)
    params.push_back(toLLVM(pt));
  FunctionType *ft = FunctionType::get(toLLVM(p.retType), params, false);
  Function *fn =
      Function::Create(ft, Function::ExternalLinkage, p.name, module_.get());
  unsigned i = 0;
  for (auto &arg : fn->args())
    arg.setName(p.args[i++]);
  return fn;
}

bool CodeGen::genFunction(const FunctionDef &f) {
  llvm::Function *fn = module_->getFunction(f.proto->name);
  if (!fn)
    return false;
  return genFunctionInto(f, fn, {}, f.proto->retType);
}

bool CodeGen::genFunctionInto(const FunctionDef &f, llvm::Function *fn,
                              const std::map<std::string, kal::Type> &subst,
                              const kal::Type &retType) {
  typeSubst_ = subst; // ジェネリック単態化中の型置換 (非総称は空)

  BasicBlock *bb = BasicBlock::Create(ctx_, "entry", fn);
  builder_.SetInsertPoint(bb);

  namedValues_.clear();
  dropScopes_.clear();
  dropScopes_.push_back({}); // 関数 (引数) スコープ
  // 引数をメモリにコピー (アドレスを取れるように)。型は宣言済みなので具体的。
  size_t ai = 0;
  for (auto &arg : fn->args()) {
    AllocaInst *slot = entryAlloca(arg.getType(), std::string(arg.getName()));
    builder_.CreateStore(&arg, slot);
    namedValues_[std::string(arg.getName())] = slot;
    if (ai < f.proto->paramTypes.size())
      registerLocal(slot, f.proto->paramTypes[ai]); // ドロップ対象なら登録
    ++ai;
  }

  // 早期リターン用: 戻り値の置き場と終端ブロックを用意する。
  currentRetTypeCG_ = retType;
  llvm::Type *retLLVM = retType.isUnit() ? nullptr : toLLVM(retType);
  currentRetSlot_ = retLLVM ? entryAlloca(retLLVM, "retval") : nullptr;
  currentRetBlock_ = BasicBlock::Create(ctx_, "return"); // 後で末尾に挿入

  Value *ret = genExpr(f.body.get());
  // 本体が末尾まで到達した (発散していない) なら戻り値を格納し、引数を drop して終端へ
  if (!blockDone()) {
    if (currentRetSlot_ && ret)
      builder_.CreateStore(ret, currentRetSlot_);
    popDropScope(); // 引数スコープを drop
    builder_.CreateBr(currentRetBlock_);
  } else {
    dropScopes_.pop_back(); // return が既に drop 済み
  }
  dropScopes_.clear();

  fn->insert(fn->end(), currentRetBlock_);
  builder_.SetInsertPoint(currentRetBlock_);
  if (retLLVM)
    builder_.CreateRet(builder_.CreateLoad(retLLVM, currentRetSlot_, "ret"));
  else
    builder_.CreateRetVoid();

  verifyFunction(*fn);
  typeSubst_.clear();
  currentRetSlot_ = nullptr;
  currentRetBlock_ = nullptr;
  return true;
}

// ジェネリック関数の具体化を宣言する (本体は後でキューから生成)。
llvm::Function *CodeGen::ensureInstance(const FunctionDef *def,
                                       const std::vector<kal::Type> &typeArgs) {
  std::string mangled = def->proto->name + "<";
  for (size_t i = 0; i < typeArgs.size(); ++i) {
    if (i)
      mangled += ", ";
    mangled += typeArgs[i].str();
  }
  mangled += ">";
  if (llvm::Function *existing = module_->getFunction(mangled))
    return existing; // 宣言済み

  std::map<std::string, kal::Type> subst;
  for (size_t i = 0;
       i < def->proto->typeParams.size() && i < typeArgs.size(); ++i)
    subst[def->proto->typeParams[i]] = typeArgs[i];

  // 具体化したプロトタイプを宣言 (型引数を代入した具体型で)
  Prototype inst;
  inst.name = mangled;
  inst.args = def->proto->args;
  for (auto &pt : def->proto->paramTypes)
    inst.paramTypes.push_back(substType(pt, subst));
  inst.retType = substType(def->proto->retType, subst);
  llvm::Function *fn = declareProto(inst);
  pendingInstances_.push_back({def, std::move(subst), mangled}); // 本体生成待ち
  return fn;
}

std::unique_ptr<Module> CodeGen::run(const Program &program, bool emitRuntime) {
  module_ = std::make_unique<Module>("kal", ctx_);
  module_->setDataLayout(dl_); // enum レイアウトのサイズ計算に必要

  // 型定義を登録 (toLLVM が名前から型を引けるように)
  for (auto &sd : program.structs)
    structDefs_[sd->name] = sd.get();
  for (auto &ed : program.enums)
    enumDefs_[ed->name] = ed.get();
  // ジェネリック関数を登録 (本体は呼び出し箇所ごとに単態化する)
  for (auto &f : program.functions)
    if (!f->proto->typeParams.empty())
      genericFuncDefs_[f->proto->name] = f.get();
  // メソッドを登録 (呼び出し箇所ごとに単態化する)
  for (auto &ib : program.impls)
    for (auto &m : ib->methods) {
      std::string bare = m->proto->name.substr(ib->typeName.size() + 1);
      methodDefs_[ib->typeName][bare] = m.get();
    }

  // (1) 全プロトタイプを宣言 (前方参照・相互再帰に対応)。ジェネリックは除く。
  for (auto &ex : program.externs)
    declareProto(*ex);
  for (auto &f : program.functions)
    if (f->proto->typeParams.empty() && !module_->getFunction(f->proto->name))
      declareProto(*f->proto);

  // 組み込み: printi(i64)->i64, printd(f64)->f64, putchard(i64)->i64
  llvm::Type *i64 = llvm::Type::getInt64Ty(ctx_);
  llvm::Type *f64 = llvm::Type::getDoubleTy(ctx_);
  auto ensure = [&](const char *name, llvm::Type *ret,
                    ArrayRef<llvm::Type *> params) {
    if (!module_->getFunction(name))
      Function::Create(FunctionType::get(ret, params, false),
                       Function::ExternalLinkage, name, module_.get());
  };
  llvm::Type *voidTy = llvm::Type::getVoidTy(ctx_);
  ensure("printi", voidTy, {i64});
  ensure("printd", voidTy, {f64});
  ensure("putchard", voidTy, {i64});
  ensure("kal_panic", voidTy, {}); // 境界違反などで呼ぶ (メッセージ表示して終了)
  ensure("malloc", PointerType::getUnqual(ctx_), {i64}); // box(e) のヒープ確保
  ensure("free", voidTy, {PointerType::getUnqual(ctx_)}); // Drop のヒープ解放
  ensure("realloc", PointerType::getUnqual(ctx_),
         {PointerType::getUnqual(ctx_), i64}); // Vec の成長
  ensure("kal_prints", voidTy,
         {PointerType::getUnqual(ctx_), i64}); // str の出力
  ensure("memcmp", llvm::Type::getInt32Ty(ctx_),
         {PointerType::getUnqual(ctx_), PointerType::getUnqual(ctx_), i64});

  // (2) 関数本体 (非総称のみ。ジェネリックは呼び出しから単態化される)
  for (auto &f : program.functions)
    if (f->proto->typeParams.empty())
      if (!genFunction(*f))
        return nullptr;

  // (3) トップレベル式を集めて __main を生成 (型に応じて自動表示)
  FunctionType *mainTy = FunctionType::get(llvm::Type::getVoidTy(ctx_), false);
  llvm::Function *mainFn = Function::Create(mainTy, Function::ExternalLinkage,
                                            "__main", module_.get());
  builder_.SetInsertPoint(BasicBlock::Create(ctx_, "entry", mainFn));
  namedValues_.clear();

  llvm::Function *printiFn = module_->getFunction("printi");
  llvm::Function *printdFn = module_->getFunction("printd");

  for (auto &e : program.topExprs) {
    Value *v = genExpr(e.get());
    const kal::Type &t = e->type;
    if (t.isUnit())
      continue; // unit は表示しない
    if (t.isFloat()) {
      Value *d = t.bits == 32 ? builder_.CreateFPExt(v, f64, "pf") : v;
      builder_.CreateCall(printdFn, {d});
    } else if (t.isInt()) {
      Value *i = builder_.CreateIntCast(v, i64, t.isSigned, "pi");
      builder_.CreateCall(printiFn, {i});
    } else if (t.isBool()) {
      Value *i = builder_.CreateZExt(v, i64, "pb");
      builder_.CreateCall(printiFn, {i});
    } else {
      dropDiscardedValue(e.get(), v); // 所有ヒープの一時値なら解放
    }
  }
  builder_.CreateRetVoid();
  verifyFunction(*mainFn);

  // (4) ジェネリック関数の具体化を本体生成する。生成中にさらに別の具体化が
  // 要求されることがあるためキューが空になるまで繰り返す (相互/自己再帰対応)。
  // 多相再帰 (型が単調増加して止まらない) を防ぐため具体化数に上限を設ける。
  const size_t kInstanceLimit = 128;
  size_t instCount = 0;
  while (!pendingInstances_.empty()) {
    if (++instCount > kInstanceLimit) {
      diag_.error(pendingInstances_.back().def->proto->nameSpan, "E0199",
                  "ジェネリックの具体化が多すぎます (多相再帰の可能性があります)");
      break;
    }
    PendingInstance inst = std::move(pendingInstances_.back());
    pendingInstances_.pop_back();
    llvm::Function *fn = module_->getFunction(inst.mangled);
    kal::Type retType = substType(inst.def->proto->retType, inst.subst);
    genFunctionInto(*inst.def, fn, inst.subst, retType);
  }

  // (5) ドロップグルー関数の本体を生成 (再帰型に対応してキューが空になるまで)。
  typeSubst_.clear();
  while (!pendingDropFns_.empty()) {
    auto pd = std::move(pendingDropFns_.back());
    pendingDropFns_.pop_back();
    genDropBody(pd.first, pd.second);
  }

  // C の main を生成 (AOT/JIT 共通。JIT は __main を直接呼ぶので未使用)
  emitCMain();

  // AOT 用: ランタイム関数の本体を生成して自己完結させる
  if (emitRuntime)
    emitRuntimeDefs();

  if (diag_.numErrors() > 0)
    return nullptr;
  return std::move(module_);
}

void CodeGen::emitCMain() {
  llvm::Type *i32 = llvm::Type::getInt32Ty(ctx_);
  FunctionType *mainTy = FunctionType::get(i32, false);
  Function *mainFn =
      Function::Create(mainTy, Function::ExternalLinkage, "main", module_.get());
  builder_.SetInsertPoint(BasicBlock::Create(ctx_, "entry", mainFn));
  builder_.CreateCall(module_->getFunction("__main"), {});
  builder_.CreateRet(ConstantInt::get(i32, 0));
  verifyFunction(*mainFn);
}

void CodeGen::emitRuntimeDefs() {
  llvm::Type *i32 = llvm::Type::getInt32Ty(ctx_);
  llvm::Type *i64 = llvm::Type::getInt64Ty(ctx_);
  llvm::Type *f64 = llvm::Type::getDoubleTy(ctx_);
  llvm::Type *ptr = PointerType::getUnqual(ctx_);

  // libc: i32 printf(ptr, ...) と i32 putchar(i32)
  auto printfFn = module_->getOrInsertFunction(
      "printf", FunctionType::get(i32, {ptr}, /*vararg=*/true));
  auto putcharFn =
      module_->getOrInsertFunction("putchar", FunctionType::get(i32, {i32}, false));

  auto body = [&](const char *name) -> Function * {
    Function *fn = module_->getFunction(name);
    builder_.SetInsertPoint(BasicBlock::Create(ctx_, "entry", fn));
    return fn;
  };

  // printi(i64 x): printf("%lld\n", x)
  {
    Function *fn = body("printi");
    Value *fmt = builder_.CreateGlobalString("%lld\n", "fmt_i");
    builder_.CreateCall(printfFn, {fmt, fn->getArg(0)});
    builder_.CreateRetVoid();
    verifyFunction(*fn);
  }
  // printd(double x): printf("%g\n", x)
  {
    Function *fn = body("printd");
    Value *fmt = builder_.CreateGlobalString("%g\n", "fmt_g");
    builder_.CreateCall(printfFn, {fmt, fn->getArg(0)});
    builder_.CreateRetVoid();
    verifyFunction(*fn);
    (void)f64;
  }
  // putchard(i64 x): putchar((i32)x)
  {
    Function *fn = body("putchard");
    Value *c = builder_.CreateTrunc(fn->getArg(0), i32, "c");
    builder_.CreateCall(putcharFn, {c});
    builder_.CreateRetVoid();
    verifyFunction(*fn);
  }
  // kal_prints(ptr s, i64 n): putchar で s[0..n) を 1 バイトずつ出力する。
  // putchar はバッファ付き stdout を使う (printi/putchard と同じ) ため出力順が
  // 正しく、`%.*s` と違い埋め込み '\0' でも途切れない (JIT の fwrite と一致)。
  {
    Function *fn = body("kal_prints");
    Value *s = fn->getArg(0);
    Value *n = fn->getArg(1);
    BasicBlock *cond = BasicBlock::Create(ctx_, "ps.cond", fn);
    BasicBlock *bodyBB = BasicBlock::Create(ctx_, "ps.body", fn);
    BasicBlock *end = BasicBlock::Create(ctx_, "ps.end", fn);
    AllocaInst *iSlot = builder_.CreateAlloca(i64, nullptr, "i");
    builder_.CreateStore(ConstantInt::get(i64, 0), iSlot);
    builder_.CreateBr(cond);
    builder_.SetInsertPoint(cond);
    Value *i = builder_.CreateLoad(i64, iSlot, "i");
    builder_.CreateCondBr(builder_.CreateICmpULT(i, n, "more"), bodyBB, end);
    builder_.SetInsertPoint(bodyBB);
    Value *bp = builder_.CreateGEP(llvm::Type::getInt8Ty(ctx_), s, {i}, "bp");
    Value *b = builder_.CreateLoad(llvm::Type::getInt8Ty(ctx_), bp, "b");
    builder_.CreateCall(putcharFn, {builder_.CreateZExt(b, i32, "bc")});
    builder_.CreateStore(builder_.CreateAdd(i, ConstantInt::get(i64, 1)), iSlot);
    builder_.CreateBr(cond);
    builder_.SetInsertPoint(end);
    builder_.CreateRetVoid();
    verifyFunction(*fn);
  }
  // kal_panic(): メッセージを表示して exit(1)
  {
    llvm::Type *voidTy = llvm::Type::getVoidTy(ctx_);
    auto exitFn = module_->getOrInsertFunction(
        "exit", FunctionType::get(voidTy, {i32}, false));
    Function *fn = body("kal_panic");
    Value *msg = builder_.CreateGlobalString("panic: index out of bounds\n",
                                             "panic_msg");
    builder_.CreateCall(printfFn, {msg});
    builder_.CreateCall(exitFn, {ConstantInt::get(i32, 1)});
    builder_.CreateUnreachable();
    verifyFunction(*fn);
  }
}
