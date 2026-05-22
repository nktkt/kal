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

llvm::Type *CodeGen::toLLVM(const kal::Type &t) {
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
    return getStructType(t.name);
  case kal::Type::Kind::Enum:
    return getEnumType(t.name);
  case kal::Type::Kind::Ref:
    return PointerType::getUnqual(ctx_);
  case kal::Type::Kind::Tuple: {
    std::vector<llvm::Type *> elems;
    for (auto &e : t.elems)
      elems.push_back(toLLVM(e));
    return StructType::get(ctx_, elems);
  }
  case kal::Type::Kind::Unknown:
    return nullptr;
  }
  return nullptr;
}

StructType *CodeGen::getStructType(const std::string &name) {
  auto it = structTypes_.find(name);
  if (it != structTypes_.end())
    return it->second;
  const StructDef *sd = structDefs_[name];
  std::vector<llvm::Type *> fieldTys;
  for (auto &f : sd->fields)
    fieldTys.push_back(toLLVM(f.type));
  StructType *st = StructType::create(ctx_, fieldTys, name);
  structTypes_[name] = st;
  return st;
}

// enum を { i64 tag, [S x i8] payload } で表す (S = 最大バリアントのサイズ)。
// tag を i64 にすることで payload が 8 バイト境界に整列する。
StructType *CodeGen::getEnumType(const std::string &name) {
  auto it = enumTypes_.find(name);
  if (it != enumTypes_.end())
    return it->second;
  const EnumDef *ed = enumDefs_[name];
  uint64_t maxSize = 0;
  for (auto &v : ed->variants) {
    std::vector<llvm::Type *> fts;
    for (auto &pt : v.payloadTypes)
      fts.push_back(toLLVM(pt));
    StructType *vs = StructType::get(ctx_, fts);
    maxSize = std::max(maxSize, dl_.getTypeAllocSize(vs).getFixedValue());
  }
  llvm::Type *tag = llvm::Type::getInt64Ty(ctx_);
  llvm::Type *payload = ArrayType::get(llvm::Type::getInt8Ty(ctx_), maxSize);
  StructType *et = StructType::create(ctx_, {tag, payload}, name);
  enumTypes_[name] = et;
  return et;
}

Value *CodeGen::genVariant(const std::string &enumName, int tag,
                           ArrayRef<Value *> payload) {
  StructType *et = getEnumType(enumName);
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
  }
  return nullptr;
}

Value *CodeGen::genStructLit(const StructLitExpr *e) {
  const StructDef *sd = structDefs_[e->structName];
  StructType *st = getStructType(e->structName);
  Value *agg = UndefValue::get(st);
  // 宣言順に、対応する初期化式を探して詰める
  for (size_t i = 0; i < sd->fields.size(); ++i) {
    Value *fv = nullptr;
    for (size_t j = 0; j < e->fieldNames.size(); ++j)
      if (e->fieldNames[j] == sd->fields[i].name) {
        fv = genExpr(e->fieldValues[j].get());
        break;
      }
    agg = builder_.CreateInsertValue(agg, fv, {static_cast<unsigned>(i)});
  }
  return agg;
}

Value *CodeGen::genField(const FieldExpr *e) {
  Value *v = genExpr(e->operand.get());
  return builder_.CreateExtractValue(v, {static_cast<unsigned>(e->fieldIndex)},
                                     "field");
}

Value *CodeGen::genTupleLit(const TupleLitExpr *e) {
  llvm::Type *tt = toLLVM(e->type);
  Value *agg = UndefValue::get(tt);
  for (size_t i = 0; i < e->elems.size(); ++i)
    agg = builder_.CreateInsertValue(agg, genExpr(e->elems[i].get()),
                                     {static_cast<unsigned>(i)});
  return agg;
}

Value *CodeGen::genTupleIndex(const TupleIndexExpr *e) {
  Value *v = genExpr(e->operand.get());
  return builder_.CreateExtractValue(v, {e->index}, "telem");
}

Value *CodeGen::genBlock(const BlockExpr *e) {
  std::vector<std::pair<std::string, Value *>> saved; // name, oldSlot(or null)
  for (auto &st : e->stmts) {
    if (st.kind == Stmt::Kind::Let) {
      Value *v = genExpr(st.expr.get());
      AllocaInst *slot = entryAlloca(toLLVM(st.expr->type), st.name);
      builder_.CreateStore(v, slot);
      saved.push_back({st.name, namedValues_.count(st.name)
                                    ? namedValues_[st.name]
                                    : nullptr});
      namedValues_[st.name] = slot;
    } else {
      genExpr(st.expr.get()); // 式文: 値は捨てる
    }
  }
  Value *result = e->tail ? genExpr(e->tail.get()) : nullptr;
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
  builder_.CreateStore(v, addr);
  return nullptr; // 代入式は unit
}

Value *CodeGen::genMatch(const MatchExpr *e) {
  Value *scrut = genExpr(e->scrutinee.get());
  StructType *et = cast<StructType>(scrut->getType());

  // scrutinee をメモリに置き、tag と payload を読み出す
  AllocaInst *slot = builder_.CreateAlloca(et);
  slot->setAlignment(Align(8));
  builder_.CreateStore(scrut, slot);
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

  for (size_t a = 0; a < e->arms.size(); ++a) {
    const MatchArm &arm = e->arms[a];
    builder_.SetInsertPoint(armBBs[a]);

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
      }
    }

    Value *bv = genExpr(arm.body.get());

    for (size_t i = 0; i < boundNames.size(); ++i) {
      if (oldVals[i])
        namedValues_[boundNames[i]] = oldVals[i];
      else
        namedValues_.erase(boundNames[i]);
    }

    BasicBlock *endBB = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);
    if (!isUnit)
      incoming.push_back({bv, endBB});
  }

  if (unreachBB) {
    builder_.SetInsertPoint(unreachBB);
    builder_.CreateUnreachable();
  }

  fn->insert(fn->end(), mergeBB);
  builder_.SetInsertPoint(mergeBB);
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

Value *CodeGen::genVariable(const VariableExpr *e) {
  if (e->variantTag >= 0) // 引数なし enum バリアント
    return genVariant(e->variantEnum, e->variantTag, {});
  // 変数はメモリ常駐 (alloca)。読み出しは load。
  auto *slot = cast<AllocaInst>(namedValues_[e->name]);
  return builder_.CreateLoad(slot->getAllocatedType(), slot, e->name);
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
    Value *base = genAddr(f->operand.get());
    StructType *st = getStructType(f->operand->type.name);
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
  default:
    break;
  }
  // 一時値: メモリに退避
  Value *v = genExpr(e);
  AllocaInst *slot = entryAlloca(toLLVM(e->type), "tmp");
  builder_.CreateStore(v, slot);
  return slot;
}

Value *CodeGen::genBorrow(const BorrowExpr *e) {
  return genAddr(e->operand.get()); // 参照値 = アドレス
}

Value *CodeGen::genDeref(const DerefExpr *e) {
  Value *ptr = genExpr(e->operand.get());
  return builder_.CreateLoad(toLLVM(e->type), ptr, "deref");
}

Value *CodeGen::genBinary(const BinaryExpr *e) {
  Value *l = genExpr(e->lhs.get());
  Value *r = genExpr(e->rhs.get());
  const kal::Type &ot = e->lhs->type; // 両辺は同じ型 (Sema 保証)
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
  case Tok::Less:
    if (isF)
      return builder_.CreateFCmpULT(l, r, "cmptmp");
    return sgn ? builder_.CreateICmpSLT(l, r, "cmptmp")
               : builder_.CreateICmpULT(l, r, "cmptmp");
  case Tok::Greater:
    if (isF)
      return builder_.CreateFCmpUGT(l, r, "cmptmp");
    return sgn ? builder_.CreateICmpSGT(l, r, "cmptmp")
               : builder_.CreateICmpUGT(l, r, "cmptmp");
  default:
    return nullptr;
  }
}

Value *CodeGen::genCall(const CallExpr *e) {
  std::vector<Value *> args;
  for (auto &a : e->args)
    args.push_back(genExpr(a.get()));
  if (e->variantTag >= 0) // enum バリアント構築
    return genVariant(e->variantEnum, e->variantTag, args);
  Function *callee = module_->getFunction(e->callee);
  CallInst *call = builder_.CreateCall(callee, args);
  if (e->type.isUnit())
    return nullptr;
  call->setName("calltmp");
  return call;
}

Value *CodeGen::genCast(const CastExpr *e) {
  Value *v = genExpr(e->operand.get());
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

  Function *fn = builder_.GetInsertBlock()->getParent();
  BasicBlock *thenBB = BasicBlock::Create(ctx_, "then", fn);
  BasicBlock *elseBB = BasicBlock::Create(ctx_, "else");
  BasicBlock *mergeBB = BasicBlock::Create(ctx_, "ifcont");

  builder_.CreateCondBr(condV, thenBB, elseBB);

  builder_.SetInsertPoint(thenBB);
  Value *thenV = genExpr(e->then.get());
  builder_.CreateBr(mergeBB);
  thenBB = builder_.GetInsertBlock();

  fn->insert(fn->end(), elseBB);
  builder_.SetInsertPoint(elseBB);
  Value *elseV = genExpr(e->els.get());
  builder_.CreateBr(mergeBB);
  elseBB = builder_.GetInsertBlock();

  fn->insert(fn->end(), mergeBB);
  builder_.SetInsertPoint(mergeBB);

  if (e->type.isUnit())
    return nullptr; // 両分岐 unit: 値なし

  PHINode *phi = builder_.CreatePHI(toLLVM(e->type), 2, "iftmp");
  phi->addIncoming(thenV, thenBB);
  phi->addIncoming(elseV, elseBB);
  return phi;
}

Value *CodeGen::genFor(const ForExpr *e) {
  const kal::Type &vt = e->start->type; // ループ変数の型
  Value *startV = genExpr(e->start.get());

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
  genExpr(e->body.get()); // 値は捨てる

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

  BasicBlock *bb = BasicBlock::Create(ctx_, "entry", fn);
  builder_.SetInsertPoint(bb);

  namedValues_.clear();
  // 引数をメモリにコピー (アドレスを取れるように)
  for (auto &arg : fn->args()) {
    AllocaInst *slot = entryAlloca(arg.getType(), std::string(arg.getName()));
    builder_.CreateStore(&arg, slot);
    namedValues_[std::string(arg.getName())] = slot;
  }

  Value *ret = genExpr(f.body.get());
  if (f.proto->retType.isUnit())
    builder_.CreateRetVoid();
  else
    builder_.CreateRet(ret);
  verifyFunction(*fn);
  return true;
}

std::unique_ptr<Module> CodeGen::run(const Program &program, bool emitRuntime) {
  module_ = std::make_unique<Module>("kal", ctx_);
  module_->setDataLayout(dl_); // enum レイアウトのサイズ計算に必要

  // 型定義を登録 (toLLVM が名前から型を引けるように)
  for (auto &sd : program.structs)
    structDefs_[sd->name] = sd.get();
  for (auto &ed : program.enums)
    enumDefs_[ed->name] = ed.get();

  // (1) 全プロトタイプを宣言 (前方参照・相互再帰に対応)
  for (auto &ex : program.externs)
    declareProto(*ex);
  for (auto &f : program.functions)
    if (!module_->getFunction(f->proto->name))
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

  // (2) 関数本体
  for (auto &f : program.functions)
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
    }
  }
  builder_.CreateRetVoid();
  verifyFunction(*mainFn);

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
}
