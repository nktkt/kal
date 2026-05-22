//===- CodeGen.cpp ---------------------------------------------------------===//
#include "kal/CodeGen.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Verifier.h"

using namespace kal;
using namespace llvm;

CodeGen::CodeGen(LLVMContext &ctx, DiagnosticEngine &diag)
    : ctx_(ctx), diag_(diag), builder_(ctx) {}

Type *CodeGen::doubleTy() { return Type::getDoubleTy(ctx_); }

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
  }
  return nullptr;
}

Value *CodeGen::genNumber(const NumberExpr *e) {
  return ConstantFP::get(ctx_, APFloat(e->value));
}

Value *CodeGen::genVariable(const VariableExpr *e) {
  auto it = namedValues_.find(e->name);
  if (it == namedValues_.end()) {
    diag_.error(e->span, "E0100", "未定義の変数です");
    return nullptr;
  }
  return it->second;
}

Value *CodeGen::genBinary(const BinaryExpr *e) {
  Value *l = genExpr(e->lhs.get());
  Value *r = genExpr(e->rhs.get());
  if (!l || !r)
    return nullptr;

  switch (e->op) {
  case Tok::Plus:
    return builder_.CreateFAdd(l, r, "addtmp");
  case Tok::Minus:
    return builder_.CreateFSub(l, r, "subtmp");
  case Tok::Star:
    return builder_.CreateFMul(l, r, "multmp");
  case Tok::Slash:
    return builder_.CreateFDiv(l, r, "divtmp");
  case Tok::Less:
    l = builder_.CreateFCmpULT(l, r, "cmptmp");
    return builder_.CreateUIToFP(l, doubleTy(), "booltmp");
  case Tok::Greater:
    l = builder_.CreateFCmpUGT(l, r, "cmptmp");
    return builder_.CreateUIToFP(l, doubleTy(), "booltmp");
  default:
    diag_.error(e->opSpan, "E0101", "未知の二項演算子です");
    return nullptr;
  }
}

Value *CodeGen::genCall(const CallExpr *e) {
  Function *callee = module_->getFunction(e->callee);
  if (!callee) {
    diag_.error(e->calleeSpan, "E0102", "未定義の関数を呼び出しています");
    return nullptr;
  }
  if (callee->arg_size() != e->args.size()) {
    diag_.error(e->span, "E0103", "引数の数が一致しません");
    return nullptr;
  }
  std::vector<Value *> args;
  for (auto &a : e->args) {
    Value *v = genExpr(a.get());
    if (!v)
      return nullptr;
    args.push_back(v);
  }
  return builder_.CreateCall(callee, args, "calltmp");
}

Value *CodeGen::genIf(const IfExpr *e) {
  Value *condV = genExpr(e->cond.get());
  if (!condV)
    return nullptr;
  condV = builder_.CreateFCmpONE(condV, ConstantFP::get(ctx_, APFloat(0.0)),
                                 "ifcond");

  Function *fn = builder_.GetInsertBlock()->getParent();
  BasicBlock *thenBB = BasicBlock::Create(ctx_, "then", fn);
  BasicBlock *elseBB = BasicBlock::Create(ctx_, "else");
  BasicBlock *mergeBB = BasicBlock::Create(ctx_, "ifcont");

  builder_.CreateCondBr(condV, thenBB, elseBB);

  builder_.SetInsertPoint(thenBB);
  Value *thenV = genExpr(e->then.get());
  if (!thenV)
    return nullptr;
  builder_.CreateBr(mergeBB);
  thenBB = builder_.GetInsertBlock();

  fn->insert(fn->end(), elseBB);
  builder_.SetInsertPoint(elseBB);
  Value *elseV = genExpr(e->els.get());
  if (!elseV)
    return nullptr;
  builder_.CreateBr(mergeBB);
  elseBB = builder_.GetInsertBlock();

  fn->insert(fn->end(), mergeBB);
  builder_.SetInsertPoint(mergeBB);
  PHINode *phi = builder_.CreatePHI(doubleTy(), 2, "iftmp");
  phi->addIncoming(thenV, thenBB);
  phi->addIncoming(elseV, elseBB);
  return phi;
}

Value *CodeGen::genFor(const ForExpr *e) {
  // 前判定ループ:  preheader → [cond] →(真) body → cond ...  →(偽) after
  Value *startV = genExpr(e->start.get());
  if (!startV)
    return nullptr;

  Function *fn = builder_.GetInsertBlock()->getParent();
  BasicBlock *preheaderBB = builder_.GetInsertBlock();
  BasicBlock *condBB = BasicBlock::Create(ctx_, "loopcond", fn);
  BasicBlock *bodyBB = BasicBlock::Create(ctx_, "loopbody", fn);
  BasicBlock *afterBB = BasicBlock::Create(ctx_, "afterloop", fn);

  builder_.CreateBr(condBB);

  builder_.SetInsertPoint(condBB);
  PHINode *var = builder_.CreatePHI(doubleTy(), 2, e->var);
  var->addIncoming(startV, preheaderBB);

  Value *oldVal = namedValues_.count(e->var) ? namedValues_[e->var] : nullptr;
  namedValues_[e->var] = var;

  Value *condV = genExpr(e->cond.get());
  if (!condV)
    return nullptr;
  condV = builder_.CreateFCmpONE(condV, ConstantFP::get(ctx_, APFloat(0.0)),
                                 "loopcheck");
  builder_.CreateCondBr(condV, bodyBB, afterBB);

  builder_.SetInsertPoint(bodyBB);
  if (!genExpr(e->body.get()))
    return nullptr;

  Value *stepV = e->step ? genExpr(e->step.get())
                         : ConstantFP::get(ctx_, APFloat(1.0));
  if (!stepV)
    return nullptr;
  Value *nextVar = builder_.CreateFAdd(var, stepV, "nextvar");
  BasicBlock *bodyEndBB = builder_.GetInsertBlock();
  builder_.CreateBr(condBB);
  var->addIncoming(nextVar, bodyEndBB);

  builder_.SetInsertPoint(afterBB);

  if (oldVal)
    namedValues_[e->var] = oldVal;
  else
    namedValues_.erase(e->var);

  return ConstantFP::get(ctx_, APFloat(0.0));
}

Function *CodeGen::declareProto(const Prototype &p) {
  std::vector<Type *> doubles(p.args.size(), doubleTy());
  FunctionType *ft = FunctionType::get(doubleTy(), doubles, false);
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
  if (!ret) {
    fn->eraseFromParent();
    return false;
  }
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

  // 組み込み printd / putchard
  FunctionType *ft = FunctionType::get(doubleTy(), {doubleTy()}, false);
  if (!module_->getFunction("printd"))
    Function::Create(ft, Function::ExternalLinkage, "printd", module_.get());
  if (!module_->getFunction("putchard"))
    Function::Create(ft, Function::ExternalLinkage, "putchard", module_.get());

  // (2) 関数本体
  for (auto &f : program.functions)
    if (!genFunction(*f))
      return nullptr;

  // (3) トップレベル式を集めて __main を生成
  FunctionType *mainTy = FunctionType::get(Type::getVoidTy(ctx_), false);
  llvm::Function *mainFn =
      Function::Create(mainTy, Function::ExternalLinkage, "__main", module_.get());
  BasicBlock *bb = BasicBlock::Create(ctx_, "entry", mainFn);
  builder_.SetInsertPoint(bb);
  namedValues_.clear();

  llvm::Function *printdFn = module_->getFunction("printd");
  for (auto &e : program.topExprs) {
    Value *v = genExpr(e.get());
    if (!v)
      return nullptr;
    builder_.CreateCall(printdFn, {v});
  }
  builder_.CreateRetVoid();
  verifyFunction(*mainFn);

  if (diag_.numErrors() > 0)
    return nullptr;
  return std::move(module_);
}
