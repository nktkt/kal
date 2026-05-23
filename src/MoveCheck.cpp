//===- MoveCheck.cpp -------------------------------------------------------===//
#include "kal/MoveCheck.h"

#include "kal/Diagnostic.h"

using namespace kal;

MoveCheck::MoveCheck(DiagnosticEngine &diag) : diag_(diag) {}

bool MoveCheck::isCopy(const Type &t) const {
  switch (t.kind) {
  case Type::Kind::Int:
  case Type::Kind::Float:
  case Type::Kind::Bool:
  case Type::Kind::Unit:
  case Type::Kind::Str:     // str は静的データへの fat ポインタ (コピー)
  case Type::Kind::Unknown: // エラー型は誤検出を避けてコピー扱い
    return true;
  case Type::Kind::Ref:
  case Type::Kind::Slice:
    return !t.refMut; // &T / &[T] はコピー、&mut は ムーブ
  case Type::Kind::Array:
    return isCopy(t.elems[0]); // 要素が Copy なら配列も Copy
  default:
    return false; // Struct / Enum / Tuple
  }
}

void MoveCheck::moveVar(const std::string &name, Span span) {
  auto it = moved_.find(name);
  if (it != moved_.end()) {
    diag_.error(span, "E0180", "ムーブ済みの値を使用しています");
    return;
  }
  moved_[name] = span;
}

void MoveCheck::requireLive(const Expr *e) {
  switch (e->kind) {
  case Expr::Kind::Variable: {
    auto *v = static_cast<const VariableExpr *>(e);
    if (v->variantTag < 0 && moved_.count(v->name))
      diag_.error(e->span, "E0182", "ムーブ済みの値を参照しています");
    return;
  }
  case Expr::Kind::Field:
    requireLive(static_cast<const FieldExpr *>(e)->operand.get());
    return;
  case Expr::Kind::TupleIndex:
    requireLive(static_cast<const TupleIndexExpr *>(e)->operand.get());
    return;
  case Expr::Kind::Index: {
    auto *ix = static_cast<const IndexExpr *>(e);
    use(ix->index.get()); // 添字値は評価される
    requireLive(ix->base.get());
    return;
  }
  case Expr::Kind::Deref:
    requireLive(static_cast<const DerefExpr *>(e)->operand.get());
    return;
  case Expr::Kind::Borrow:
    requireLive(static_cast<const BorrowExpr *>(e)->operand.get());
    return;
  default:
    use(e); // 一時値など
    return;
  }
}

void MoveCheck::use(const Expr *e) {
  switch (e->kind) {
  case Expr::Kind::Number:
  case Expr::Kind::BoolLit:
  case Expr::Kind::StringLit:
    return;
  case Expr::Kind::Variable: {
    auto *v = static_cast<const VariableExpr *>(e);
    if (v->variantTag >= 0)
      return; // 引数なしバリアント構築
    if (isCopy(e->type)) {
      if (moved_.count(v->name))
        diag_.error(e->span, "E0180", "ムーブ済みの値を使用しています");
    } else {
      moveVar(v->name, e->span);
      const_cast<VariableExpr *>(v)->movesValue = true; // ドロップフラグ降下用
    }
    return;
  }
  case Expr::Kind::Binary: {
    auto *b = static_cast<const BinaryExpr *>(e);
    use(b->lhs.get());
    use(b->rhs.get());
    return;
  }
  case Expr::Kind::MethodCall: {
    auto *m = static_cast<const MethodCallExpr *>(e);
    if (m->selfKind == 0)
      use(m->receiver.get()); // 値レシーバ: ムーブ
    else
      requireLive(m->receiver.get()); // &self / &mut self: 借用
    for (auto &a : m->args)
      use(a.get());
    return;
  }
  case Expr::Kind::Return: {
    auto *r = static_cast<const ReturnExpr *>(e);
    if (r->value)
      use(r->value.get());
    return;
  }
  case Expr::Kind::Try:
    use(static_cast<const TryExpr *>(e)->operand.get()); // operand を消費
    return;
  case Expr::Kind::Call: {
    auto *c = static_cast<const CallExpr *>(e);
    if (c->isLenBuiltin) {
      for (auto &a : c->args)
        requireLive(a.get()); // len は借用: 引数をムーブしない
      return;
    }
    if (c->isPushBuiltin && c->args.size() == 2) {
      requireLive(c->args[0].get()); // push(v, x): v は可変借用 (ムーブしない)
      use(c->args[1].get());         // x は v にムーブで入る
      return;
    }
    for (auto &a : c->args)
      use(a.get());
    return;
  }
  case Expr::Kind::Cast:
    use(static_cast<const CastExpr *>(e)->operand.get());
    return;
  case Expr::Kind::Unary:
    use(static_cast<const UnaryExpr *>(e)->operand.get());
    return;
  case Expr::Kind::StructLit: {
    auto *s = static_cast<const StructLitExpr *>(e);
    for (auto &fv : s->fieldValues)
      use(fv.get());
    return;
  }
  case Expr::Kind::TupleLit: {
    auto *t = static_cast<const TupleLitExpr *>(e);
    for (auto &el : t->elems)
      use(el.get());
    return;
  }
  case Expr::Kind::ArrayLit: {
    auto *a = static_cast<const ArrayLitExpr *>(e);
    for (auto &el : a->elems)
      use(el.get());
    return;
  }
  case Expr::Kind::Index: {
    auto *ix = static_cast<const IndexExpr *>(e);
    use(ix->index.get()); // 添字値は評価される
    if (ix->base->type.isSlice() || ix->base->type.isVec()) {
      // スライス/Vec の要素はコンテナが所有するため、添字で値を取り出すと
      // ムーブアウトになる。非 Copy 要素は取り出せない (借用するか Copy のみ)。
      requireLive(ix->base.get());
      if (!isCopy(e->type))
        diag_.error(e->span, "E0184",
                    "スライス・Vec の非 Copy 要素はムーブで取り出せません");
    } else if (isCopy(e->type)) {
      requireLive(ix->base.get()); // Copy 要素の読みはムーブしない
    } else {
      use(ix->base.get()); // 非 Copy 要素 → 配列全体をムーブ (保守的)
    }
    return;
  }
  case Expr::Kind::Field: {
    auto *f = static_cast<const FieldExpr *>(e);
    if (isCopy(e->type))
      requireLive(f->operand.get()); // Copy フィールドの読みはムーブしない
    else
      use(f->operand.get()); // 非 Copy フィールド → ベースをムーブ (保守的)
    return;
  }
  case Expr::Kind::TupleIndex: {
    auto *t = static_cast<const TupleIndexExpr *>(e);
    if (isCopy(e->type))
      requireLive(t->operand.get());
    else
      use(t->operand.get());
    return;
  }
  case Expr::Kind::Borrow:
    requireLive(static_cast<const BorrowExpr *>(e)->operand.get());
    return;
  case Expr::Kind::Deref: {
    auto *d = static_cast<const DerefExpr *>(e);
    if (d->operand->type.isBox() && !isCopy(e->type)) {
      // 箱の中身を非 Copy でムーブ取り出し = 箱ごと消費する (Rust の *box と同じ)。
      // これで同じ中身の二重取り出し (二重解放) を防ぐ。CodeGen は箱を free する。
      use(d->operand.get());
      const_cast<DerefExpr *>(d)->movesOutOfBox = true;
    } else {
      requireLive(d->operand.get());
      if (!d->operand->type.isBox() && !isCopy(e->type))
        diag_.error(e->span, "E0181", "参照からムーブすることはできません");
    }
    return;
  }
  case Expr::Kind::If: {
    auto *i = static_cast<const IfExpr *>(e);
    use(i->cond.get());
    if (!i->els) { // 文としての if: then の (条件付き) ムーブのみ
      use(i->then.get());
      return;
    }
    auto saved = moved_;
    use(i->then.get());
    auto afterThen = moved_;
    moved_ = saved;
    use(i->els.get());
    // 合流: どちらかの分岐でムーブされたら以降ムーブ扱い
    for (auto &kv : afterThen)
      moved_.insert(kv);
    return;
  }
  case Expr::Kind::Match: {
    auto *m = static_cast<const MatchExpr *>(e);
    if (m->scrutinee->type.isRef())
      requireLive(m->scrutinee.get()); // &Enum 経由: ムーブしない
    else
      use(m->scrutinee.get());
    auto saved = moved_;
    std::map<std::string, Span> result;
    bool first = true;
    for (auto &arm : m->arms) {
      moved_ = saved;
      use(arm.body.get());
      for (auto &b : arm.bindings) // アーム束縛はスコープ外へ
        if (b != "_")
          moved_.erase(b);
      if (first) {
        result = moved_;
        first = false;
      } else {
        for (auto &kv : moved_)
          result.insert(kv);
      }
    }
    if (!first)
      moved_ = result;
    return;
  }
  case Expr::Kind::Block: {
    auto *blk = static_cast<const BlockExpr *>(e);
    // ブロックスコープ: 導入した束縛は退出時に復元
    std::vector<std::pair<std::string, std::pair<bool, Span>>> saved;
    for (auto &st : blk->stmts) {
      if (st.kind == Stmt::Kind::Let) {
        use(st.expr.get());
        bool had = moved_.count(st.name) != 0;
        saved.push_back({st.name, {had, had ? moved_[st.name] : Span{}}});
        moved_.erase(st.name); // 新しい束縛は未ムーブ
      } else {
        use(st.expr.get());
      }
    }
    if (blk->tail)
      use(blk->tail.get());
    for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
      moved_.erase(it->first);
      if (it->second.first)
        moved_[it->first] = it->second.second;
    }
    return;
  }
  case Expr::Kind::Assign: {
    auto *a = static_cast<const AssignExpr *>(e);
    use(a->value.get());
    const Expr *tgt = a->target.get();
    if (tgt->kind == Expr::Kind::Variable &&
        static_cast<const VariableExpr *>(tgt)->variantTag < 0) {
      moved_.erase(static_cast<const VariableExpr *>(tgt)->name); // 再初期化
    } else {
      requireLive(tgt); // *p / s.x: ベースは生存している必要
    }
    return;
  }
  case Expr::Kind::For: {
    auto *f = static_cast<const ForExpr *>(e);
    use(f->start.get());
    auto before = moved_;
    use(f->cond.get());
    use(f->body.get());
    // 本体で新たにムーブされた外側変数 = 次の反復で再利用される恐れ
    for (auto &kv : moved_)
      if (!before.count(kv.first))
        diag_.error(kv.second, "E0183",
                    "ループ本体で値がムーブされています "
                    "(次の反復で再利用される可能性があります)");
    return;
  }
  }
}

bool MoveCheck::run(const Program &program) {
  for (auto &f : program.functions) {
    moved_.clear();
    use(f->body.get());
  }
  for (auto &ib : program.impls)
    for (auto &m : ib->methods) {
      moved_.clear();
      use(m->body.get());
    }
  for (auto &e : program.topExprs) {
    moved_.clear();
    use(e.get());
  }
  return diag_.numErrors() == 0;
}
