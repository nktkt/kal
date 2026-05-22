//===- Sema.cpp - type checking + literal inference -----------------------===//
#include "kal/Sema.h"

#include "kal/Diagnostic.h"

using namespace kal;

Sema::Sema(DiagnosticEngine &diag) : diag_(diag) {}

bool Sema::run(Program &program) {
  // 組み込み関数のシグネチャ (すべて unit を返す → トップレベルで表示されない)
  funcs_["printi"] = {{Type::intTy(64, true)}, Type::unit()};
  funcs_["printd"] = {{Type::floatTy(64)}, Type::unit()};
  funcs_["putchard"] = {{Type::intTy(64, true)}, Type::unit()};

  // ユーザー定義のシグネチャを先に集める (前方参照・相互再帰に対応)
  for (auto &ex : program.externs)
    funcs_[ex->name] = {ex->paramTypes, ex->retType};
  for (auto &f : program.functions)
    funcs_[f->proto->name] = {f->proto->paramTypes, f->proto->retType};

  // 各関数本体を検査
  for (auto &f : program.functions)
    checkFunction(*f);

  // トップレベル式 (型は自由 / 自動表示のために注釈する)
  for (auto &e : program.topExprs)
    check(e.get(), std::nullopt);

  return diag_.numErrors() == 0;
}

void Sema::checkFunction(FunctionDef &f) {
  locals_.clear();
  for (size_t i = 0; i < f.proto->args.size(); ++i)
    locals_[f.proto->args[i]] = f.proto->paramTypes[i];

  Type bodyT = check(f.body.get(), f.proto->retType);
  if (bodyT.isKnown() && bodyT != f.proto->retType) {
    diag_.error(f.body->span, "E0110",
                "戻り値の型が一致しません (期待 " + f.proto->retType.str() +
                    ", 実際 " + bodyT.str() + ")");
  }
}

Type Sema::check(Expr *e, std::optional<Type> expected) {
  Type t;
  switch (e->kind) {
  case Expr::Kind::Number:
    t = checkNumber(static_cast<NumberExpr *>(e), expected);
    break;
  case Expr::Kind::Variable:
    t = checkVariable(static_cast<VariableExpr *>(e));
    break;
  case Expr::Kind::Binary:
    t = checkBinary(static_cast<BinaryExpr *>(e), expected);
    break;
  case Expr::Kind::Call:
    t = checkCall(static_cast<CallExpr *>(e));
    break;
  case Expr::Kind::If:
    t = checkIf(static_cast<IfExpr *>(e), expected);
    break;
  case Expr::Kind::For:
    t = checkFor(static_cast<ForExpr *>(e));
    break;
  case Expr::Kind::Cast:
    t = checkCast(static_cast<CastExpr *>(e));
    break;
  }
  e->type = t;
  return t;
}

Type Sema::checkNumber(NumberExpr *e, std::optional<Type> expected) {
  if (e->isFloat) {
    // 小数リテラル: 期待が浮動小数点ならそれに合わせる。既定 f64。
    if (expected && expected->isFloat())
      return *expected;
    return Type::floatTy(64);
  }
  // 整数リテラル: 期待が整数ならそれに合わせる。既定 i32。
  if (expected && expected->isInt())
    return *expected;
  return Type::intTy(32, true);
}

Type Sema::checkVariable(VariableExpr *e) {
  auto it = locals_.find(e->name);
  if (it == locals_.end()) {
    diag_.error(e->span, "E0100", "未定義の変数です");
    return Type::unknown();
  }
  return it->second;
}

Type Sema::checkBinary(BinaryExpr *e, std::optional<Type> expected) {
  bool isCompare = (e->op == Tok::Less || e->op == Tok::Greater);

  // 比較なら期待型は両辺に伝えない (結果は bool のため)。
  std::optional<Type> lhsHint = isCompare ? std::nullopt : expected;
  Type lt = check(e->lhs.get(), lhsHint);
  // 右辺は左辺の型に合わせる (リテラルの型を確定させる)。
  std::optional<Type> rhsHint =
      lt.isKnown() ? std::optional<Type>(lt) : std::nullopt;
  Type rt = check(e->rhs.get(), rhsHint);

  if (lt.isKnown() && !lt.isNumeric()) {
    diag_.error(e->lhs->span, "E0120", "数値型が必要です (実際 " + lt.str() + ")");
    return Type::unknown();
  }
  if (lt.isKnown() && rt.isKnown() && lt != rt) {
    diag_.error(e->span, "E0121",
                "二項演算の両辺の型が一致しません (" + lt.str() + " と " +
                    rt.str() + ")");
    return isCompare ? Type::boolean() : Type::unknown();
  }
  return isCompare ? Type::boolean() : lt;
}

Type Sema::checkCall(CallExpr *e) {
  auto it = funcs_.find(e->callee);
  if (it == funcs_.end()) {
    diag_.error(e->calleeSpan, "E0102", "未定義の関数を呼び出しています");
    return Type::unknown();
  }
  const FuncSig &sig = it->second;
  if (sig.params.size() != e->args.size()) {
    diag_.error(e->span, "E0103", "引数の数が一致しません (期待 " +
                                      std::to_string(sig.params.size()) +
                                      ", 実際 " +
                                      std::to_string(e->args.size()) + ")");
    return sig.ret;
  }
  for (size_t i = 0; i < e->args.size(); ++i) {
    Type at = check(e->args[i].get(), sig.params[i]);
    if (at.isKnown() && at != sig.params[i]) {
      diag_.error(e->args[i]->span, "E0104",
                  "引数の型が一致しません (期待 " + sig.params[i].str() +
                      ", 実際 " + at.str() + ")");
    }
  }
  return sig.ret;
}

Type Sema::checkIf(IfExpr *e, std::optional<Type> expected) {
  Type ct = check(e->cond.get(), Type::boolean());
  if (ct.isKnown() && !ct.isBool())
    diag_.error(e->cond->span, "E0130",
                "if の条件は bool 型である必要があります (実際 " + ct.str() + ")");

  Type tt = check(e->then.get(), expected);
  std::optional<Type> elseHint =
      tt.isKnown() ? std::optional<Type>(tt) : expected;
  Type et = check(e->els.get(), elseHint);

  if (tt.isKnown() && et.isKnown() && tt != et) {
    diag_.error(e->span, "E0131",
                "if の then と else の型が一致しません (" + tt.str() + " と " +
                    et.str() + ")");
    return tt;
  }
  return tt.isKnown() ? tt : et;
}

Type Sema::checkFor(ForExpr *e) {
  Type st = check(e->start.get(), std::nullopt);
  if (st.isKnown() && !st.isNumeric())
    diag_.error(e->start->span, "E0140", "ループの開始値は数値型が必要です");

  // ループ変数をスコープに導入 (同名は退避)
  bool hadOld = locals_.count(e->var) != 0;
  Type old = hadOld ? locals_[e->var] : Type();
  locals_[e->var] = st;

  Type ct = check(e->cond.get(), Type::boolean());
  if (ct.isKnown() && !ct.isBool())
    diag_.error(e->cond->span, "E0141",
                "ループの継続条件は bool 型が必要です (実際 " + ct.str() + ")");

  if (e->step) {
    Type stepT = check(e->step.get(), st);
    if (stepT.isKnown() && st.isKnown() && stepT != st)
      diag_.error(e->step->span, "E0142",
                  "ステップの型が開始値と一致しません (" + st.str() + " と " +
                      stepT.str() + ")");
  }

  check(e->body.get(), std::nullopt); // 本体の型は捨てる

  if (hadOld)
    locals_[e->var] = old;
  else
    locals_.erase(e->var);

  return Type::unit();
}

Type Sema::checkCast(CastExpr *e) {
  Type src = check(e->operand.get(), std::nullopt);
  Type dst = e->targetType;
  // 数値・bool 間のキャストのみ許可。
  bool srcOk = src.isNumeric() || src.isBool();
  bool dstOk = dst.isNumeric() || dst.isBool();
  if (src.isKnown() && (!srcOk || !dstOk))
    diag_.error(e->span, "E0150",
                "この型へはキャストできません (" + src.str() + " as " +
                    dst.str() + ")");
  return dst;
}
