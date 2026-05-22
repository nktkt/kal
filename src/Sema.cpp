//===- Sema.cpp - type checking + literal inference -----------------------===//
#include "kal/Sema.h"

#include "kal/Diagnostic.h"

#include <functional>

using namespace kal;

Sema::Sema(DiagnosticEngine &diag) : diag_(diag) {}

const StructDef *Sema::findStruct(const std::string &name) const {
  auto it = structs_.find(name);
  return it == structs_.end() ? nullptr : it->second;
}

const EnumDef *Sema::findEnum(const std::string &name) const {
  auto it = enums_.find(name);
  return it == enums_.end() ? nullptr : it->second;
}

Type Sema::resolve(Type t) const {
  if (t.kind == Type::Kind::Struct && findEnum(t.name))
    t.kind = Type::Kind::Enum; // パーサが Struct と誤判定した enum 名を直す
  else if (t.kind == Type::Kind::Tuple || t.kind == Type::Kind::Ref)
    for (auto &e : t.elems)
      e = resolve(e);
  return t;
}

bool Sema::run(Program &program) {
  // (1) 型名 (struct / enum) を登録
  for (auto &sd : program.structs) {
    if (structs_.count(sd->name))
      diag_.error(sd->nameSpan, "E0045", "構造体が重複定義されています");
    structs_[sd->name] = sd.get();
  }
  for (auto &ed : program.enums) {
    if (enums_.count(ed->name) || structs_.count(ed->name))
      diag_.error(ed->nameSpan, "E0085", "型名が重複しています");
    enums_[ed->name] = ed.get();
  }

  // (2) 注釈された型を解決 (enum 名を Struct→Enum に直す)
  for (auto &sd : program.structs)
    for (auto &f : sd->fields)
      f.type = resolve(f.type);
  for (auto &ed : program.enums)
    for (auto &v : ed->variants)
      for (auto &pt : v.payloadTypes)
        pt = resolve(pt);
  for (auto &ex : program.externs) {
    for (auto &pt : ex->paramTypes)
      pt = resolve(pt);
    ex->retType = resolve(ex->retType);
  }
  for (auto &f : program.functions) {
    for (auto &pt : f->proto->paramTypes)
      pt = resolve(pt);
    f->proto->retType = resolve(f->proto->retType);
  }

  // (3) 型存在チェック (Ref/Tuple は再帰的に)
  std::function<bool(const Type &)> knownType = [&](const Type &t) -> bool {
    if (t.isStruct())
      return findStruct(t.name) != nullptr;
    if (t.isEnum())
      return findEnum(t.name) != nullptr;
    if (t.isRef())
      return knownType(t.elems[0]);
    if (t.isTuple()) {
      for (auto &e : t.elems)
        if (!knownType(e))
          return false;
      return true;
    }
    return true;
  };
  for (auto &sd : program.structs)
    for (auto &f : sd->fields)
      if (!knownType(f.type))
        diag_.error(f.span, "E0046", "未定義の型です: " + f.type.name);
  for (auto &ed : program.enums)
    for (auto &v : ed->variants)
      for (auto &pt : v.payloadTypes)
        if (!knownType(pt))
          diag_.error(v.span, "E0046", "未定義の型です: " + pt.name);

  // (4) バリアントを登録 (名前はグローバルに一意・解決済みペイロード型で)
  for (auto &ed : program.enums)
    for (size_t i = 0; i < ed->variants.size(); ++i) {
      const auto &v = ed->variants[i];
      if (variants_.count(v.name))
        diag_.error(v.span, "E0086", "バリアント名が重複しています: " + v.name);
      variants_[v.name] = {ed->name, static_cast<int>(i), v.payloadTypes};
    }

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
    locals_[f.proto->args[i]] = {f.proto->paramTypes[i], /*isMut=*/false};

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
  case Expr::Kind::StructLit:
    t = checkStructLit(static_cast<StructLitExpr *>(e));
    break;
  case Expr::Kind::Field:
    t = checkField(static_cast<FieldExpr *>(e));
    break;
  case Expr::Kind::TupleLit:
    t = checkTupleLit(static_cast<TupleLitExpr *>(e), expected);
    break;
  case Expr::Kind::TupleIndex:
    t = checkTupleIndex(static_cast<TupleIndexExpr *>(e));
    break;
  case Expr::Kind::Match:
    t = checkMatch(static_cast<MatchExpr *>(e), expected);
    break;
  case Expr::Kind::Borrow:
    t = checkBorrow(static_cast<BorrowExpr *>(e), expected);
    break;
  case Expr::Kind::Deref:
    t = checkDeref(static_cast<DerefExpr *>(e));
    break;
  case Expr::Kind::Block:
    t = checkBlock(static_cast<BlockExpr *>(e), expected);
    break;
  case Expr::Kind::Assign:
    t = checkAssign(static_cast<AssignExpr *>(e));
    break;
  case Expr::Kind::Unary:
    t = checkUnary(static_cast<UnaryExpr *>(e), expected);
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
  if (it != locals_.end())
    return it->second.type;
  // ローカルになければ、引数なし enum バリアントかもしれない
  auto vit = variants_.find(e->name);
  if (vit != variants_.end()) {
    if (!vit->second.payloadTypes.empty()) {
      diag_.error(e->span, "E0105",
                  "バリアント " + e->name + " には引数が必要です");
      return Type::unknown();
    }
    e->variantTag = vit->second.tag;
    e->variantEnum = vit->second.enumName;
    return Type::enumTy(vit->second.enumName);
  }
  diag_.error(e->span, "E0100", "未定義の変数です");
  return Type::unknown();
}

Type Sema::checkBinary(BinaryExpr *e, std::optional<Type> expected) {
  Tok op = e->op;
  bool isOrdCmp = op == Tok::Less || op == Tok::Greater || op == Tok::Le ||
                  op == Tok::Ge;
  bool isEq = op == Tok::EqEq || op == Tok::BangEq;
  bool isLogical = op == Tok::AmpAmp || op == Tok::PipePipe;
  bool isCompare = isOrdCmp || isEq;

  // 論理 && / || : 両辺 bool、結果 bool
  if (isLogical) {
    Type lt = check(e->lhs.get(), Type::boolean());
    Type rt = check(e->rhs.get(), Type::boolean());
    if (lt.isKnown() && !lt.isBool())
      diag_.error(e->lhs->span, "E0122",
                  "論理演算には bool が必要です (実際 " + lt.str() + ")");
    if (rt.isKnown() && !rt.isBool())
      diag_.error(e->rhs->span, "E0122",
                  "論理演算には bool が必要です (実際 " + rt.str() + ")");
    return Type::boolean();
  }

  // 比較なら期待型は両辺に伝えない (結果は bool のため)。
  std::optional<Type> lhsHint = isCompare ? std::nullopt : expected;
  Type lt = check(e->lhs.get(), lhsHint);
  std::optional<Type> rhsHint =
      lt.isKnown() ? std::optional<Type>(lt) : std::nullopt;
  Type rt = check(e->rhs.get(), rhsHint);

  // == / != は数値か bool に使える。順序比較・算術は数値のみ。
  if (lt.isKnown()) {
    bool ok = isEq ? (lt.isNumeric() || lt.isBool()) : lt.isNumeric();
    if (!ok) {
      diag_.error(e->lhs->span, "E0120",
                  (isEq ? "== / != には数値か bool が必要です (実際 "
                        : "数値型が必要です (実際 ") +
                      lt.str() + ")");
      return isCompare ? Type::boolean() : Type::unknown();
    }
  }
  if (lt.isKnown() && rt.isKnown() && lt != rt) {
    diag_.error(e->span, "E0121",
                "二項演算の両辺の型が一致しません (" + lt.str() + " と " +
                    rt.str() + ")");
    return isCompare ? Type::boolean() : Type::unknown();
  }
  return isCompare ? Type::boolean() : lt;
}

Type Sema::checkUnary(UnaryExpr *e, std::optional<Type> expected) {
  if (e->op == Tok::Bang) { // 論理否定
    Type t = check(e->operand.get(), Type::boolean());
    if (t.isKnown() && !t.isBool())
      diag_.error(e->operand->span, "E0123",
                  "! には bool が必要です (実際 " + t.str() + ")");
    return Type::boolean();
  }
  // 単項マイナス
  Type t = check(e->operand.get(), expected);
  if (t.isKnown() && !t.isNumeric()) {
    diag_.error(e->operand->span, "E0124",
                "単項 - には数値型が必要です (実際 " + t.str() + ")");
    return Type::unknown();
  }
  return t;
}

Type Sema::checkCall(CallExpr *e) {
  auto it = funcs_.find(e->callee);
  if (it == funcs_.end()) {
    // 関数でなければ enum バリアント構築かもしれない
    auto vit = variants_.find(e->callee);
    if (vit != variants_.end()) {
      const VariantInfo &vi = vit->second;
      if (vi.payloadTypes.size() != e->args.size())
        diag_.error(e->span, "E0106",
                    "バリアント " + e->callee + " のペイロード数が一致しません (期待 " +
                        std::to_string(vi.payloadTypes.size()) + ", 実際 " +
                        std::to_string(e->args.size()) + ")");
      for (size_t i = 0; i < e->args.size() && i < vi.payloadTypes.size(); ++i) {
        Type at = check(e->args[i].get(), vi.payloadTypes[i]);
        if (at.isKnown() && at != vi.payloadTypes[i])
          diag_.error(e->args[i]->span, "E0107",
                      "ペイロードの型が一致しません (期待 " +
                          vi.payloadTypes[i].str() + ", 実際 " + at.str() + ")");
      }
      e->variantTag = vi.tag;
      e->variantEnum = vi.enumName;
      return Type::enumTy(vi.enumName);
    }
    diag_.error(e->calleeSpan, "E0102", "未定義の関数を呼び出しています");
    for (auto &a : e->args)
      check(a.get(), std::nullopt);
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

  // ループ変数をスコープに導入 (同名は退避)。ループ変数は不変。
  bool hadOld = locals_.count(e->var) != 0;
  Local old = hadOld ? locals_[e->var] : Local();
  locals_[e->var] = {st, /*isMut=*/false};

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
  e->targetType = resolve(e->targetType);
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

Type Sema::checkStructLit(StructLitExpr *e) {
  const StructDef *sd = findStruct(e->structName);
  if (!sd) {
    diag_.error(e->nameSpan, "E0060", "未定義の構造体です: " + e->structName);
    for (auto &fv : e->fieldValues)
      check(fv.get(), std::nullopt);
    return Type::unknown();
  }
  if (e->fieldValues.size() != sd->fields.size())
    diag_.error(e->span, "E0061",
                "フィールド数が一致しません (期待 " +
                    std::to_string(sd->fields.size()) + ", 実際 " +
                    std::to_string(e->fieldValues.size()) + ")");
  for (size_t i = 0; i < e->fieldValues.size(); ++i) {
    const StructField *decl = nullptr;
    for (auto &f : sd->fields)
      if (f.name == e->fieldNames[i]) {
        decl = &f;
        break;
      }
    if (!decl) {
      diag_.error(e->fieldValues[i]->span, "E0062",
                  "そのようなフィールドはありません: " + e->fieldNames[i]);
      check(e->fieldValues[i].get(), std::nullopt);
      continue;
    }
    Type ft = check(e->fieldValues[i].get(), decl->type);
    if (ft.isKnown() && ft != decl->type)
      diag_.error(e->fieldValues[i]->span, "E0063",
                  "フィールド '" + decl->name + "' の型が一致しません (期待 " +
                      decl->type.str() + ", 実際 " + ft.str() + ")");
  }
  return Type::structTy(e->structName);
}

Type Sema::checkField(FieldExpr *e) {
  Type ot = check(e->operand.get(), std::nullopt);
  if (!ot.isStruct()) {
    if (ot.isKnown())
      diag_.error(e->operand->span, "E0064",
                  "フィールドアクセスには構造体が必要です (実際 " + ot.str() +
                      ")");
    return Type::unknown();
  }
  const StructDef *sd = findStruct(ot.name);
  if (!sd)
    return Type::unknown();
  for (size_t i = 0; i < sd->fields.size(); ++i)
    if (sd->fields[i].name == e->field) {
      e->fieldIndex = static_cast<int>(i);
      return sd->fields[i].type;
    }
  diag_.error(e->fieldSpan, "E0065",
              "構造体 " + ot.name + " にフィールド '" + e->field +
                  "' はありません");
  return Type::unknown();
}

Type Sema::checkTupleLit(TupleLitExpr *e, std::optional<Type> expected) {
  std::vector<Type> elems;
  for (size_t i = 0; i < e->elems.size(); ++i) {
    std::optional<Type> hint;
    if (expected && expected->isTuple() && i < expected->elems.size())
      hint = expected->elems[i];
    elems.push_back(check(e->elems[i].get(), hint));
  }
  return Type::tupleTy(std::move(elems));
}

Type Sema::checkTupleIndex(TupleIndexExpr *e) {
  Type ot = check(e->operand.get(), std::nullopt);
  if (!ot.isTuple()) {
    if (ot.isKnown())
      diag_.error(e->operand->span, "E0066",
                  "タプルではないものに添字アクセスしています (実際 " +
                      ot.str() + ")");
    return Type::unknown();
  }
  if (e->index >= ot.elems.size()) {
    diag_.error(e->indexSpan, "E0067",
                "タプルの添字が範囲外です (要素数 " +
                    std::to_string(ot.elems.size()) + ")");
    return Type::unknown();
  }
  return ot.elems[e->index];
}

// 代入先 / &mut で借用できる「可変な場所」か判定する。
bool Sema::isMutablePlace(const Expr *e) {
  switch (e->kind) {
  case Expr::Kind::Variable: {
    auto *v = static_cast<const VariableExpr *>(e);
    auto it = locals_.find(v->name);
    return it != locals_.end() && it->second.isMut;
  }
  case Expr::Kind::Deref: {
    // *p が書けるのは p が &mut のとき
    auto *d = static_cast<const DerefExpr *>(e);
    return d->operand->type.isRef() && d->operand->type.refMut;
  }
  case Expr::Kind::Field:
    return isMutablePlace(static_cast<const FieldExpr *>(e)->operand.get());
  case Expr::Kind::TupleIndex:
    return isMutablePlace(static_cast<const TupleIndexExpr *>(e)->operand.get());
  default:
    return false;
  }
}

Type Sema::checkBlock(BlockExpr *e, std::optional<Type> expected) {
  // ブロックは新しいスコープ。導入した束縛を末尾で復元する。
  std::vector<std::pair<std::string, bool>> introduced; // name, hadOld
  std::vector<Local> savedLocals;

  for (auto &st : e->stmts) {
    if (st.kind == Stmt::Kind::Let) {
      std::optional<Type> hint;
      if (st.hasAnnotation) {
        st.annotatedType = resolve(st.annotatedType);
        hint = st.annotatedType;
      }
      Type vt = check(st.expr.get(), hint);
      if (st.hasAnnotation) {
        if (vt.isKnown() && vt != st.annotatedType)
          diag_.error(st.expr->span, "E0053",
                      "let の値の型が注釈と一致しません (期待 " +
                          st.annotatedType.str() + ", 実際 " + vt.str() + ")");
        vt = st.annotatedType;
      }
      introduced.push_back({st.name, locals_.count(st.name) != 0});
      savedLocals.push_back(introduced.back().second ? locals_[st.name]
                                                     : Local());
      locals_[st.name] = {vt, st.isMut};
    } else {
      check(st.expr.get(), std::nullopt); // 式文: 値は捨てる
    }
  }

  Type result = e->tail ? check(e->tail.get(), expected) : Type::unit();

  // スコープ復元 (逆順)
  for (size_t i = introduced.size(); i-- > 0;) {
    if (introduced[i].second)
      locals_[introduced[i].first] = savedLocals[i];
    else
      locals_.erase(introduced[i].first);
  }
  return result;
}

Type Sema::checkAssign(AssignExpr *e) {
  Type tt = check(e->target.get(), std::nullopt);
  Type vt = check(e->value.get(), tt.isKnown() ? std::optional<Type>(tt)
                                               : std::nullopt);
  if (!isMutablePlace(e->target.get()))
    diag_.error(e->target->span, "E0170",
                "可変でない場所には代入できません (let mut / &mut が必要)");
  if (tt.isKnown() && vt.isKnown() && tt != vt)
    diag_.error(e->span, "E0171",
                "代入の型が一致しません (左辺 " + tt.str() + ", 右辺 " + vt.str() +
                    ")");
  return Type::unit();
}

Type Sema::checkBorrow(BorrowExpr *e, std::optional<Type> expected) {
  // 期待が参照型なら、その指す型をヒントとして伝える
  std::optional<Type> hint;
  if (expected && expected->isRef())
    hint = expected->pointee();
  Type t = check(e->operand.get(), hint);
  if (e->isMut && !isMutablePlace(e->operand.get()))
    diag_.error(e->span, "E0172",
                "&mut で借用するには可変な場所が必要です (let mut が必要)");
  return Type::refTy(t, e->isMut);
}

Type Sema::checkDeref(DerefExpr *e) {
  Type t = check(e->operand.get(), std::nullopt);
  if (!t.isRef()) {
    if (t.isKnown())
      diag_.error(e->span, "E0160",
                  "参照ではないものを参照外ししています (実際 " + t.str() + ")");
    return Type::unknown();
  }
  return t.pointee();
}

Type Sema::checkMatch(MatchExpr *e, std::optional<Type> expected) {
  Type st = check(e->scrutinee.get(), std::nullopt);
  if (!st.isEnum()) {
    if (st.isKnown())
      diag_.error(e->scrutinee->span, "E0090",
                  "match の対象は enum である必要があります (実際 " + st.str() +
                      ")");
    for (auto &arm : e->arms)
      check(arm.body.get(), expected);
    return Type::unknown();
  }
  const EnumDef *ed = findEnum(st.name);
  if (!ed)
    return Type::unknown();

  Type result = Type::unknown();
  bool haveResult = false;
  bool hasWildcard = false;
  std::vector<bool> covered(ed->variants.size(), false);

  for (auto &arm : e->arms) {
    std::vector<std::pair<std::string, Type>> binds; // 束縛名 → 型

    if (arm.isWildcard) {
      hasWildcard = true;
    } else {
      int idx = -1;
      for (size_t i = 0; i < ed->variants.size(); ++i)
        if (ed->variants[i].name == arm.variant) {
          idx = static_cast<int>(i);
          break;
        }
      if (idx < 0) {
        diag_.error(arm.variantSpan, "E0091",
                    "enum " + ed->name + " にバリアント '" + arm.variant +
                        "' はありません");
        check(arm.body.get(), expected);
        continue;
      }
      const EnumVariant &v = ed->variants[idx];
      arm.tag = idx;
      arm.payloadTypes = v.payloadTypes;
      if (covered[idx])
        diag_.warning(arm.variantSpan, "E0092",
                      "このバリアントは既に網羅されています");
      covered[idx] = true;
      if (arm.bindings.size() != v.payloadTypes.size())
        diag_.error(arm.variantSpan, "E0093",
                    "束縛の数がペイロードと一致しません (期待 " +
                        std::to_string(v.payloadTypes.size()) + ", 実際 " +
                        std::to_string(arm.bindings.size()) + ")");
      for (size_t i = 0; i < arm.bindings.size() && i < v.payloadTypes.size();
           ++i)
        if (arm.bindings[i] != "_")
          binds.push_back({arm.bindings[i], v.payloadTypes[i]});
    }

    // 束縛をスコープに入れて本体を検査 (束縛は不変)
    std::vector<bool> hadOld;
    std::vector<Local> oldLocals;
    for (auto &b : binds) {
      hadOld.push_back(locals_.count(b.first) != 0);
      oldLocals.push_back(hadOld.back() ? locals_[b.first] : Local());
      locals_[b.first] = {b.second, /*isMut=*/false};
    }
    Type bt =
        check(arm.body.get(), haveResult ? std::optional<Type>(result) : expected);
    for (size_t i = 0; i < binds.size(); ++i) {
      if (hadOld[i])
        locals_[binds[i].first] = oldLocals[i];
      else
        locals_.erase(binds[i].first);
    }

    if (!haveResult) {
      result = bt;
      haveResult = true;
    } else if (bt.isKnown() && result.isKnown() && bt != result) {
      diag_.error(arm.body->span, "E0094",
                  "match の各アームの型が一致しません (" + result.str() + " と " +
                      bt.str() + ")");
    }
  }

  // 網羅性チェック
  if (!hasWildcard) {
    std::string missing;
    for (size_t i = 0; i < ed->variants.size(); ++i)
      if (!covered[i]) {
        if (!missing.empty())
          missing += ", ";
        missing += ed->variants[i].name;
      }
    if (!missing.empty())
      diag_.error(e->span, "E0095",
                  "match が網羅的ではありません (未処理: " + missing + ")");
  }
  return result;
}
