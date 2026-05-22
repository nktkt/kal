//===- CodeGen.cpp - lowers the typed AST to LLVM IR ----------------------===//
//
// Sema が各 Expr に型を注釈済みなので、ここでは型に従って整数/浮動小数点の
// 命令を選ぶだけ。型エラーは起きない前提 (起きたら内部バグ)。
//
//===----------------------------------------------------------------------===//
#include "kal/CodeGen.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Verifier.h"

using namespace kal;
using namespace llvm;

CodeGen::CodeGen(LLVMContext &ctx, DiagnosticEngine &diag)
    : ctx_(ctx), diag_(diag), builder_(ctx) {}

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
  case kal::Type::Kind::Unknown:
    return nullptr;
  }
  return nullptr;
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
  }
  return nullptr;
}

Value *CodeGen::genNumber(const NumberExpr *e) {
  if (e->isFloat)
    return ConstantFP::get(toLLVM(e->type), e->floatValue);
  return ConstantInt::get(toLLVM(e->type), e->intValue, e->type.isSigned);
}

Value *CodeGen::genVariable(const VariableExpr *e) {
  return namedValues_[e->name];
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
  Function *callee = module_->getFunction(e->callee);
  std::vector<Value *> args;
  for (auto &a : e->args)
    args.push_back(genExpr(a.get()));
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

  Function *fn = builder_.GetInsertBlock()->getParent();
  BasicBlock *preheaderBB = builder_.GetInsertBlock();
  BasicBlock *condBB = BasicBlock::Create(ctx_, "loopcond", fn);
  BasicBlock *bodyBB = BasicBlock::Create(ctx_, "loopbody", fn);
  BasicBlock *afterBB = BasicBlock::Create(ctx_, "afterloop", fn);

  builder_.CreateBr(condBB);

  builder_.SetInsertPoint(condBB);
  PHINode *var = builder_.CreatePHI(toLLVM(vt), 2, e->var);
  var->addIncoming(startV, preheaderBB);

  Value *oldVal = namedValues_.count(e->var) ? namedValues_[e->var] : nullptr;
  namedValues_[e->var] = var;

  Value *condV = genExpr(e->cond.get()); // i1
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

  Value *nextVar = vt.isFloat() ? builder_.CreateFAdd(var, stepV, "nextvar")
                                : builder_.CreateAdd(var, stepV, "nextvar");
  BasicBlock *bodyEndBB = builder_.GetInsertBlock();
  builder_.CreateBr(condBB);
  var->addIncoming(nextVar, bodyEndBB);

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
  for (auto &arg : fn->args())
    namedValues_[std::string(arg.getName())] = &arg;

  Value *ret = genExpr(f.body.get());
  if (f.proto->retType.isUnit())
    builder_.CreateRetVoid();
  else
    builder_.CreateRet(ret);
  verifyFunction(*fn);
  return true;
}

std::unique_ptr<Module> CodeGen::run(const Program &program) {
  module_ = std::make_unique<Module>("kal", ctx_);

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

  if (diag_.numErrors() > 0)
    return nullptr;
  return std::move(module_);
}
