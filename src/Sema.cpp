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
  // 組み込み Box<T>
  if (t.kind == Type::Kind::Struct && t.name == "Box" && t.elems.size() == 1)
    return Type::boxTy(resolve(t.elems[0]));
  // 検査中のジェネリック関数の型引数名は Param へ (本体の let/as 注釈用)
  if (t.kind == Type::Kind::Struct && activeTypeParams_.count(t.name))
    return Type::paramTy(t.name);
  if (t.kind == Type::Kind::Struct && findEnum(t.name))
    t.kind = Type::Kind::Enum; // パーサが Struct と誤判定した enum 名を直す
  // 複合型の要素・型引数を再帰的に解決 (Tuple/Ref/Array/Slice/Struct/Enum)
  for (auto &e : t.elems)
    e = resolve(e);
  return t;
}

// 型名のうち、ジェネリック型引数 (params) に一致するものを Param へ変換する。
Type Sema::bindParams(Type t, const std::set<std::string> &params) const {
  if (t.kind == Type::Kind::Struct && params.count(t.name))
    return Type::paramTy(t.name);
  for (auto &e : t.elems)
    e = bindParams(e, params);
  return t;
}

// subst に従って Param を具体型へ置換する。
Type Sema::substType(const Type &t,
                     const std::map<std::string, Type> &subst) const {
  if (t.isParam()) {
    auto it = subst.find(t.name);
    return it != subst.end() ? it->second : t;
  }
  Type r = t;
  for (auto &e : r.elems)
    e = substType(e, subst);
  return r;
}

// decl (Param を含む) と actual (具体型) を突き合わせて型引数を解く。
void Sema::unifyParam(const Type &decl, const Type &actual,
                      std::map<std::string, Type> &subst) const {
  if (decl.isParam()) {
    // 推論変数 (非アクティブ Param) を含む actual は束縛しない。
    // (外側の未解決型引数が漏れた期待型のケース。引数側の推論に委ねる)
    if (actual.isKnown() && !hasNonActiveParam(actual) && !subst.count(decl.name))
      subst[decl.name] = actual;
    return;
  }
  if (decl.kind == actual.kind && decl.elems.size() == actual.elems.size())
    for (size_t i = 0; i < decl.elems.size(); ++i)
      unifyParam(decl.elems[i], actual.elems[i], subst);
}

bool Sema::hasParam(const Type &t) {
  if (t.isParam())
    return true;
  for (auto &e : t.elems)
    if (hasParam(e))
      return true;
  return false;
}

// 式が「必ず発散する」(return 等で先へ進まない) か。末尾 return 文の判定用。
static bool definitelyDiverges(const Expr *e) {
  switch (e->kind) {
  case Expr::Kind::Return:
    return true;
  case Expr::Kind::Block: {
    auto *b = static_cast<const BlockExpr *>(e);
    if (b->tail)
      return definitelyDiverges(b->tail.get());
    return !b->stmts.empty() &&
           b->stmts.back().kind == Stmt::Kind::Expr &&
           definitelyDiverges(b->stmts.back().expr.get());
  }
  case Expr::Kind::If: {
    auto *i = static_cast<const IfExpr *>(e);
    return i->els && definitelyDiverges(i->then.get()) &&
           definitelyDiverges(i->els.get());
  }
  case Expr::Kind::Match: {
    auto *m = static_cast<const MatchExpr *>(e);
    if (m->arms.empty())
      return false;
    for (auto &arm : m->arms)
      if (!definitelyDiverges(arm.body.get()))
        return false;
    return true;
  }
  default:
    return false;
  }
}

bool Sema::hasNonActiveParam(const Type &t) const {
  if (t.isParam())
    return activeTypeParams_.count(t.name) == 0; // 検査中の型引数でなければ推論変数
  for (auto &e : t.elems)
    if (hasNonActiveParam(e))
      return true;
  return false;
}

// 型 t が「値として」含む struct/enum 名を集める (無限サイズ検出用)。
// 参照・スライスはポインタなので連鎖を断ち切る。型引数は保守的に値とみなす。
static void collectValueDeps(const Type &t, std::set<std::string> &out) {
  switch (t.kind) {
  case Type::Kind::Struct:
  case Type::Kind::Enum:
    out.insert(t.name);
    for (auto &a : t.elems) // 型引数も保守的に辿る
      collectValueDeps(a, out);
    return;
  case Type::Kind::Tuple:
  case Type::Kind::Array:
    for (auto &e : t.elems)
      collectValueDeps(e, out);
    return;
  default: // Ref/Slice はポインタ (有限)、Param/スカラーは葉
    return;
  }
}

bool Sema::run(Program &program) {
  // (1) 型名 (struct / enum) を登録
  for (auto &sd : program.structs) {
    if (sd->name == "Box")
      diag_.error(sd->nameSpan, "E0049", "Box は組み込み型です");
    if (structs_.count(sd->name))
      diag_.error(sd->nameSpan, "E0045", "構造体が重複定義されています");
    structs_[sd->name] = sd.get();
  }
  for (auto &ed : program.enums) {
    if (ed->name == "Box")
      diag_.error(ed->nameSpan, "E0049", "Box は組み込み型です");
    if (enums_.count(ed->name) || structs_.count(ed->name))
      diag_.error(ed->nameSpan, "E0085", "型名が重複しています");
    enums_[ed->name] = ed.get();
  }

  // トレイトを登録
  for (auto &td : program.traits) {
    if (traits_.count(td->name) || structs_.count(td->name) ||
        enums_.count(td->name))
      diag_.error(td->nameSpan, "E0220", "トレイト名が重複しています: " + td->name);
    traits_[td->name] = td.get();
    std::set<std::string> seenM; // メソッド名の重複を弾く
    for (auto &m : td->methods)
      if (!seenM.insert(m->name).second)
        diag_.error(m->nameSpan, "E0206",
                    "トレイトメソッドが重複しています: " + m->name);
  }

  // 型引数名の検査 (組み込み型名との衝突 / 同一リスト内の重複)
  auto checkTypeParams = [&](const std::vector<std::string> &tps, Span span) {
    std::set<std::string> seen;
    for (auto &p : tps) {
      Type dummy;
      if (typeFromName(p, dummy))
        diag_.error(span, "E0047",
                    "型引数名が組み込み型名と衝突しています: " + p);
      if (!seen.insert(p).second)
        diag_.error(span, "E0048", "型引数名が重複しています: " + p);
    }
  };
  for (auto &sd : program.structs)
    checkTypeParams(sd->typeParams, sd->nameSpan);
  for (auto &ed : program.enums)
    checkTypeParams(ed->typeParams, ed->nameSpan);
  for (auto &f : program.functions)
    checkTypeParams(f->proto->typeParams, f->proto->nameSpan);
  for (auto &ib : program.impls)
    checkTypeParams(ib->typeParams, ib->typeSpan);

  // (2) 注釈された型を解決 (enum 名を Struct→Enum に直す)
  for (auto &sd : program.structs) {
    std::set<std::string> params(sd->typeParams.begin(), sd->typeParams.end());
    for (auto &f : sd->fields)
      f.type = resolve(bindParams(f.type, params)); // 型引数名を Param へ
  }
  for (auto &ed : program.enums) {
    std::set<std::string> params(ed->typeParams.begin(), ed->typeParams.end());
    for (auto &v : ed->variants)
      for (auto &pt : v.payloadTypes)
        // 先に型引数名を Param へ束縛してから resolve する。
        // (順序が逆だと resolve が型引数名を同名の enum に誤って解決してしまう)
        pt = resolve(bindParams(pt, params));
  }
  for (auto &ex : program.externs) {
    for (auto &pt : ex->paramTypes)
      pt = resolve(pt);
    ex->retType = resolve(ex->retType);
  }
  for (auto &f : program.functions) {
    std::set<std::string> params(f->proto->typeParams.begin(),
                                 f->proto->typeParams.end());
    for (auto &pt : f->proto->paramTypes)
      pt = resolve(bindParams(pt, params)); // 型引数名を Param へ
    f->proto->retType = resolve(bindParams(f->proto->retType, params));
  }
  for (auto &ib : program.impls) {
    std::set<std::string> params(ib->typeParams.begin(), ib->typeParams.end());
    for (auto &m : ib->methods) {
      for (auto &pt : m->proto->paramTypes)
        pt = resolve(bindParams(pt, params)); // self 型・引数型の Param 化
      m->proto->retType = resolve(bindParams(m->proto->retType, params));
    }
  }
  // トレイトメソッドの非 self 引数・戻り値を解決する (self はプレースホルダなので除く)
  for (auto &td : program.traits)
    for (auto &m : td->methods) {
      for (size_t i = 1; i < m->paramTypes.size(); ++i)
        m->paramTypes[i] = resolve(m->paramTypes[i]);
      m->retType = resolve(m->retType);
    }

  // (3) 型存在チェック (Ref/Tuple は再帰的に)
  std::function<bool(const Type &)> knownType = [&](const Type &t) -> bool {
    if (t.isParam())
      return true; // 型変数は常に妥当
    if (t.isStruct()) {
      const StructDef *sd = findStruct(t.name);
      if (!sd || t.elems.size() != sd->typeParams.size())
        return false; // struct 不在、または型引数の数が合わない
      for (auto &a : t.elems)
        if (!knownType(a))
          return false;
      return true;
    }
    if (t.isEnum()) {
      const EnumDef *ed = findEnum(t.name);
      if (!ed || t.elems.size() != ed->typeParams.size())
        return false; // enum 不在、または型引数の数が合わない
      for (auto &a : t.elems)
        if (!knownType(a))
          return false;
      return true;
    }
    if (t.isRef() || t.isArray() || t.isSlice() || t.isBox())
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
        diag_.error(f.span, "E0046", "未定義または不正な型です: " + f.type.str());
  for (auto &ed : program.enums)
    for (auto &v : ed->variants)
      for (auto &pt : v.payloadTypes)
        if (!knownType(pt))
          diag_.error(v.span, "E0046", "未定義または不正な型です: " + pt.str());
  for (auto &ex : program.externs) {
    if (!ex->typeParams.empty()) {
      diag_.error(ex->span, "E0098", "extern 関数はジェネリックにできません");
      continue; // 型引数を含むシグネチャは検査しない
    }
    for (auto &pt : ex->paramTypes)
      if (!knownType(pt))
        diag_.error(ex->span, "E0046", "未定義または不正な型です: " + pt.str());
    if (!knownType(ex->retType))
      diag_.error(ex->span, "E0046",
                  "未定義または不正な型です: " + ex->retType.str());
  }
  for (auto &f : program.functions) {
    for (auto &pt : f->proto->paramTypes)
      if (!knownType(pt))
        diag_.error(f->proto->span, "E0046",
                    "未定義または不正な型です: " + pt.str());
    if (!knownType(f->proto->retType))
      diag_.error(f->proto->span, "E0046",
                  "未定義または不正な型です: " + f->proto->retType.str());
  }
  for (auto &ib : program.impls) {
    // impl の対象型は struct / enum で、型引数の数が一致する必要がある
    const StructDef *isd = findStruct(ib->typeName);
    const EnumDef *ied = findEnum(ib->typeName);
    size_t arity = isd ? isd->typeParams.size()
                       : ied ? ied->typeParams.size() : 0;
    if (!isd && !ied)
      diag_.error(ib->typeSpan, "E0205",
                  "impl の対象型が未定義です: " + ib->typeName);
    else if (arity != ib->typeParams.size())
      diag_.error(ib->typeSpan, "E0205",
                  "impl の型引数の数が型 " + ib->typeName + " と一致しません");
    for (auto &m : ib->methods) {
      for (auto &pt : m->proto->paramTypes)
        if (!knownType(pt))
          diag_.error(m->proto->span, "E0046",
                      "未定義または不正な型です: " + pt.str());
      if (!knownType(m->proto->retType))
        diag_.error(m->proto->span, "E0046",
                    "未定義または不正な型です: " + m->proto->retType.str());
    }
  }
  // トレイトメソッドの型 (非 self) を検査
  for (auto &td : program.traits)
    for (auto &m : td->methods) {
      for (size_t i = 1; i < m->paramTypes.size(); ++i)
        if (!knownType(m->paramTypes[i]))
          diag_.error(m->span, "E0046",
                      "未定義または不正な型です: " + m->paramTypes[i].str());
      if (!knownType(m->retType))
        diag_.error(m->span, "E0046",
                    "未定義または不正な型です: " + m->retType.str());
    }
  // 型引数の境界が存在するトレイトを指すか
  for (auto &f : program.functions)
    for (auto &b : f->proto->bounds)
      if (!traits_.count(b.second))
        diag_.error(f->proto->nameSpan, "E0229",
                    "未定義のトレイトです: " + b.second);
  // トレイト実装の適合性チェック
  for (auto &ib : program.impls) {
    if (ib->traitName.empty())
      continue;
    auto trIt = traits_.find(ib->traitName);
    if (trIt == traits_.end()) {
      diag_.error(ib->traitSpan, "E0225",
                  "未定義のトレイトです: " + ib->traitName);
      continue;
    }
    const TraitDef *td = trIt->second;
    traitImpls_.insert({ib->traitName, ib->typeName}); // 実装関係を記録
    auto sk = [](const Type &s) { return !s.isRef() ? 0 : (s.refMut ? 2 : 1); };
    auto bare = [&](const FunctionDef *m) {
      return m->proto->name.substr(ib->typeName.size() + 1);
    };
    for (auto &tm : td->methods) { // 各トレイトメソッドの実装と一致を確認
      const FunctionDef *im = nullptr;
      for (auto &m : ib->methods)
        if (bare(m.get()) == tm->name) {
          im = m.get();
          break;
        }
      if (!im) {
        diag_.error(ib->traitSpan, "E0226",
                    "トレイト " + ib->traitName + " のメソッド '" + tm->name +
                        "' が未実装です");
        continue;
      }
      bool ok = im->proto->paramTypes.size() == tm->paramTypes.size() &&
                sk(im->proto->paramTypes[0]) == sk(tm->paramTypes[0]) &&
                im->proto->retType == tm->retType;
      for (size_t i = 1; ok && i < tm->paramTypes.size(); ++i)
        ok = im->proto->paramTypes[i] == tm->paramTypes[i];
      if (!ok)
        diag_.error(im->proto->nameSpan, "E0227",
                    "メソッド '" + tm->name + "' のシグネチャがトレイト " +
                        ib->traitName + " と一致しません");
    }
    for (auto &m : ib->methods) { // トレイトにないメソッドを実装していないか
      std::string bn = bare(m.get());
      bool inTrait = false;
      for (auto &tm : td->methods)
        if (tm->name == bn)
          inTrait = true;
      if (!inTrait)
        diag_.error(m->proto->nameSpan, "E0228",
                    "トレイト " + ib->traitName + " にメソッド '" + bn +
                        "' はありません");
    }
  }

  // (3.5) 無限サイズの値型 (間接化なしの再帰) を拒否する。
  // 値として自分自身に到達できる struct/enum はサイズが確定せず、
  // 単態化時に無限再帰する。参照・スライスを挟めば有限になる。
  {
    std::map<std::string, std::set<std::string>> deps;
    for (auto &sd : program.structs) {
      auto &d = deps[sd->name];
      for (auto &f : sd->fields)
        collectValueDeps(f.type, d);
    }
    for (auto &ed : program.enums) {
      auto &d = deps[ed->name];
      for (auto &v : ed->variants)
        for (auto &pt : v.payloadTypes)
          collectValueDeps(pt, d);
    }
    std::function<bool(const std::string &, const std::string &,
                       std::set<std::string> &)>
        reaches = [&](const std::string &from, const std::string &target,
                      std::set<std::string> &seen) -> bool {
      auto it = deps.find(from);
      if (it == deps.end())
        return false;
      for (auto &nxt : it->second) {
        if (nxt == target)
          return true;
        if (seen.insert(nxt).second && reaches(nxt, target, seen))
          return true;
      }
      return false;
    };
    auto checkFinite = [&](const std::string &name, Span span) {
      std::set<std::string> seen;
      if (reaches(name, name, seen))
        diag_.error(span, "E0089",
                    "再帰的な型は無限サイズです: " + name +
                        " (参照・スライスによる間接化が必要です)");
    };
    for (auto &sd : program.structs)
      checkFinite(sd->name, sd->nameSpan);
    for (auto &ed : program.enums)
      checkFinite(ed->name, ed->nameSpan);
  }

  // (4) バリアントを登録 (名前はグローバルに一意・所属 enum の型引数つき)
  for (auto &ed : program.enums)
    for (size_t i = 0; i < ed->variants.size(); ++i) {
      const auto &v = ed->variants[i];
      if (variants_.count(v.name))
        diag_.error(v.span, "E0086", "バリアント名が重複しています: " + v.name);
      variants_[v.name] = {ed->name, static_cast<int>(i), v.payloadTypes,
                           ed->typeParams};
    }

  // 組み込み関数のシグネチャ (すべて unit を返す → トップレベルで表示されない)
  funcs_["printi"] = {{Type::intTy(64, true)}, Type::unit()};
  funcs_["printd"] = {{Type::floatTy(64)}, Type::unit()};
  funcs_["putchard"] = {{Type::intTy(64, true)}, Type::unit()};

  // ユーザー定義のシグネチャを先に集める (前方参照・相互再帰に対応)。
  // (ジェネリック extern は上で E0098 済み。funcs_ には入れない)
  for (auto &ex : program.externs)
    if (ex->typeParams.empty())
      funcs_[ex->name] = {ex->paramTypes, ex->retType};
  for (auto &f : program.functions) {
    if (f->proto->typeParams.empty())
      funcs_[f->proto->name] = {f->proto->paramTypes, f->proto->retType};
    else
      genericFuncs_[f->proto->name] = f->proto.get(); // ジェネリックは別管理
  }

  // メソッドを (型名, メソッド名) で登録
  for (auto &ib : program.impls)
    for (auto &m : ib->methods) {
      std::string bare = m->proto->name.substr(ib->typeName.size() + 1);
      if (methods_[ib->typeName].count(bare))
        diag_.error(m->proto->nameSpan, "E0206",
                    "メソッドが重複しています: " + ib->typeName + "." + bare);
      methods_[ib->typeName][bare] = m.get();
    }

  // 各関数本体を検査
  for (auto &f : program.functions)
    checkFunction(*f);
  // 各メソッド本体を検査
  for (auto &ib : program.impls)
    for (auto &m : ib->methods)
      checkFunction(*m);

  // トップレベル式 (型は自由 / 自動表示のために注釈する)
  for (auto &e : program.topExprs)
    check(e.get(), std::nullopt);

  return diag_.numErrors() == 0;
}

void Sema::checkFunction(FunctionDef &f) {
  locals_.clear();
  // ジェネリック関数なら型引数をスコープに入れる (本体の型注釈で Param 化)
  activeTypeParams_ = {f.proto->typeParams.begin(), f.proto->typeParams.end()};
  paramBounds_.clear();
  for (auto &b : f.proto->bounds)
    paramBounds_[b.first].push_back(b.second); // 型引数の境界トレイト
  currentRetType_ = f.proto->retType;          // return / ? の検証用
  for (size_t i = 0; i < f.proto->args.size(); ++i)
    locals_[f.proto->args[i]] = {f.proto->paramTypes[i], /*isMut=*/false};

  Type bodyT = check(f.body.get(), f.proto->retType);
  if (bodyT.isKnown() && bodyT != f.proto->retType) {
    diag_.error(f.body->span, "E0110",
                "戻り値の型が一致しません (期待 " + f.proto->retType.str() +
                    ", 実際 " + bodyT.str() + ")");
  }
  activeTypeParams_.clear();
  paramBounds_.clear();
  currentRetType_.reset();
}

Type Sema::check(Expr *e, std::optional<Type> expected) {
  Type t;
  switch (e->kind) {
  case Expr::Kind::Number:
    t = checkNumber(static_cast<NumberExpr *>(e), expected);
    break;
  case Expr::Kind::Variable:
    t = checkVariable(static_cast<VariableExpr *>(e), expected);
    break;
  case Expr::Kind::Binary:
    t = checkBinary(static_cast<BinaryExpr *>(e), expected);
    break;
  case Expr::Kind::Call:
    t = checkCall(static_cast<CallExpr *>(e), expected);
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
    t = checkStructLit(static_cast<StructLitExpr *>(e), expected);
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
  case Expr::Kind::ArrayLit:
    t = checkArrayLit(static_cast<ArrayLitExpr *>(e), expected);
    break;
  case Expr::Kind::Index:
    t = checkIndex(static_cast<IndexExpr *>(e));
    break;
  case Expr::Kind::BoolLit:
    t = Type::boolean();
    break;
  case Expr::Kind::MethodCall:
    t = checkMethodCall(static_cast<MethodCallExpr *>(e));
    break;
  case Expr::Kind::Return:
    t = checkReturn(static_cast<ReturnExpr *>(e));
    break;
  case Expr::Kind::Try:
    t = checkTry(static_cast<TryExpr *>(e));
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

Type Sema::checkVariable(VariableExpr *e, std::optional<Type> expected) {
  auto it = locals_.find(e->name);
  if (it != locals_.end())
    return it->second.type;
  // ローカルになければ、引数なし enum バリアントかもしれない
  auto vit = variants_.find(e->name);
  if (vit != variants_.end()) {
    const VariantInfo &vi = vit->second;
    if (!vi.payloadTypes.empty()) {
      diag_.error(e->span, "E0105",
                  "バリアント " + e->name + " には引数が必要です");
      return Type::unknown();
    }
    e->variantTag = vi.tag;
    e->variantEnum = vi.enumName;
    if (vi.typeParams.empty())
      return Type::enumTy(vi.enumName);
    // ジェネリック (None 等): 引数がないので型引数は期待型からのみ決まる
    std::map<std::string, Type> subst;
    if (expected && expected->isEnum() && expected->name == vi.enumName &&
        expected->elems.size() == vi.typeParams.size())
      for (size_t i = 0; i < vi.typeParams.size(); ++i)
        subst[vi.typeParams[i]] = expected->elems[i];
    std::vector<Type> args;
    for (auto &p : vi.typeParams) {
      auto sit = subst.find(p);
      if (sit == subst.end()) {
        diag_.error(e->span, "E0097",
                    "型引数を推論できません: " + vi.enumName + " の " + p +
                        " (型注釈が必要です)");
        return Type::unknown();
      }
      args.push_back(sit->second);
    }
    return Type::enumTy(vi.enumName, std::move(args));
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

Type Sema::checkCall(CallExpr *e, std::optional<Type> expected) {
  // 組み込み len(s: &[T]) -> i64 (ユーザーが len を定義していなければ)
  if (e->callee == "len" && !funcs_.count("len")) {
    e->isLenBuiltin = true;
    if (e->args.size() != 1) {
      diag_.error(e->span, "E0108",
                  "len には引数が 1 つ必要です (実際 " +
                      std::to_string(e->args.size()) + ")");
      for (auto &a : e->args)
        check(a.get(), std::nullopt);
      return Type::intTy(64, true);
    }
    Type at = check(e->args[0].get(), std::nullopt);
    if (at.isKnown() && !at.isSlice())
      diag_.error(e->args[0]->span, "E0109",
                  "len の引数はスライスである必要があります (実際 " + at.str() +
                      ")");
    return Type::intTy(64, true);
  }

  // 組み込み box(e) -> Box<T> (ユーザーが box を定義していなければ)
  if (e->callee == "box" && !funcs_.count("box")) {
    e->isBoxBuiltin = true;
    if (e->args.size() != 1) {
      diag_.error(e->span, "E0245",
                  "box には引数が 1 つ必要です (実際 " +
                      std::to_string(e->args.size()) + ")");
      for (auto &a : e->args)
        check(a.get(), std::nullopt);
      return Type::unknown();
    }
    std::optional<Type> hint;
    if (expected && expected->isBox())
      hint = expected->boxedType(); // 期待が Box<T> なら中身に T を伝える
    Type at = check(e->args[0].get(), hint);
    if (!at.isKnown())
      return Type::unknown();
    return Type::boxTy(at);
  }

  // ジェネリック関数呼び出し: 期待型 + 引数から型引数を推論する (turbofish 不要)。
  auto git = genericFuncs_.find(e->callee);
  if (git != genericFuncs_.end()) {
    const Prototype *proto = git->second;
    if (proto->paramTypes.size() != e->args.size()) {
      diag_.error(e->span, "E0103",
                  "引数の数が一致しません (期待 " +
                      std::to_string(proto->paramTypes.size()) + ", 実際 " +
                      std::to_string(e->args.size()) + ")");
      for (auto &a : e->args)
        check(a.get(), std::nullopt);
      return Type::unknown();
    }
    std::map<std::string, Type> subst;
    if (expected)
      unifyParam(proto->retType, *expected, subst); // 期待型から推論
    for (size_t i = 0; i < e->args.size(); ++i) {
      // 部分的に解決済みのヒントを渡す (具体部分は伝播し、未解決 Param は
      // リテラル側で無視される) → 引数から残りの型引数を推論
      Type at = check(e->args[i].get(),
                      std::optional<Type>(substType(proto->paramTypes[i], subst)));
      unifyParam(proto->paramTypes[i], at, subst);
    }
    std::vector<Type> targs;
    for (auto &p : proto->typeParams) {
      auto sit = subst.find(p);
      if (sit == subst.end()) {
        diag_.error(e->span, "E0097",
                    "型引数を推論できません: " + e->callee + " の " + p +
                        " (型注釈が必要です)");
        return Type::unknown();
      }
      targs.push_back(sit->second);
    }
    // トレイト境界を満たすか確認 (解決した型引数がトレイトを実装しているか)
    for (auto &b : proto->bounds) {
      Type st = substType(Type::paramTy(b.first), subst);
      bool ok;
      if (st.isParam()) { // 検査中の型引数なら、その境界に含まれていれば満たす
        ok = false;
        auto pb = paramBounds_.find(st.name);
        if (pb != paramBounds_.end())
          for (auto &t : pb->second)
            if (t == b.second)
              ok = true;
      } else {
        ok = traitImpls_.count({b.second, st.name}) > 0;
      }
      if (!ok)
        diag_.error(e->span, "E0230",
                    "型 " + st.str() + " はトレイト " + b.second +
                        " を実装していません");
    }
    // 解決後の引数型で再検証
    for (size_t i = 0; i < e->args.size(); ++i) {
      Type want = substType(proto->paramTypes[i], subst);
      if (e->args[i]->type.isKnown() && e->args[i]->type != want)
        diag_.error(e->args[i]->span, "E0104",
                    "引数の型が一致しません (期待 " + want.str() + ", 実際 " +
                        e->args[i]->type.str() + ")");
    }
    e->typeArgs = std::move(targs);
    return substType(proto->retType, subst);
  }

  auto it = funcs_.find(e->callee);
  if (it == funcs_.end()) {
    // 関数でなければ enum バリアント構築かもしれない
    auto vit = variants_.find(e->callee);
    if (vit != variants_.end()) {
      const VariantInfo &vi = vit->second;
      e->variantTag = vi.tag;
      e->variantEnum = vi.enumName;
      if (vi.payloadTypes.size() != e->args.size()) {
        diag_.error(e->span, "E0106",
                    "バリアント " + e->callee + " のペイロード数が一致しません (期待 " +
                        std::to_string(vi.payloadTypes.size()) + ", 実際 " +
                        std::to_string(e->args.size()) + ")");
        for (auto &a : e->args)
          check(a.get(), std::nullopt);
        return vi.typeParams.empty() ? Type::enumTy(vi.enumName)
                                     : Type::unknown();
      }
      // 非総称: 従来どおりペイロード型で検査
      if (vi.typeParams.empty()) {
        for (size_t i = 0; i < e->args.size(); ++i) {
          Type at = check(e->args[i].get(), vi.payloadTypes[i]);
          if (at.isKnown() && at != vi.payloadTypes[i])
            diag_.error(e->args[i]->span, "E0107",
                        "ペイロードの型が一致しません (期待 " +
                            vi.payloadTypes[i].str() + ", 実際 " + at.str() +
                            ")");
        }
        return Type::enumTy(vi.enumName);
      }
      // ジェネリック: 期待型 + 引数から型引数を推論する。
      // 推論変数 (非アクティブ Param) を含む期待要素は引数で決める。
      std::map<std::string, Type> subst;
      if (expected && expected->isEnum() && expected->name == vi.enumName &&
          expected->elems.size() == vi.typeParams.size())
        for (size_t i = 0; i < vi.typeParams.size(); ++i)
          if (!hasNonActiveParam(expected->elems[i]))
            subst[vi.typeParams[i]] = expected->elems[i];
      for (size_t i = 0; i < e->args.size(); ++i) {
        Type hint = substType(vi.payloadTypes[i], subst);
        Type at = check(e->args[i].get(),
                        hasParam(hint) ? std::nullopt : std::optional<Type>(hint));
        unifyParam(vi.payloadTypes[i], at, subst);
      }
      std::vector<Type> args;
      for (auto &p : vi.typeParams) {
        auto sit = subst.find(p);
        if (sit == subst.end()) {
          diag_.error(e->span, "E0097",
                      "型引数を推論できません: " + vi.enumName + " の " + p +
                          " (型注釈が必要です)");
          return Type::unknown();
        }
        args.push_back(sit->second);
      }
      // 解決後のペイロード型で各引数を再検証
      for (size_t i = 0; i < e->args.size(); ++i) {
        Type want = substType(vi.payloadTypes[i], subst);
        if (e->args[i]->type.isKnown() && e->args[i]->type != want)
          diag_.error(e->args[i]->span, "E0107",
                      "ペイロードの型が一致しません (期待 " + want.str() +
                          ", 実際 " + e->args[i]->type.str() + ")");
      }
      return Type::enumTy(vi.enumName, std::move(args));
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

  // else 省略 = 文としての if。then の値は捨て、全体は unit。
  if (!e->els) {
    check(e->then.get(), std::nullopt);
    return Type::unit();
  }

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

Type Sema::checkStructLit(StructLitExpr *e, std::optional<Type> expected) {
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

  // 同じフィールドの重複指定を弾く。重複を許すと個数一致のまま別フィールドが
  // 欠落し、CodeGen で未初期化 (nullptr) になりクラッシュする。
  for (size_t i = 0; i < e->fieldNames.size(); ++i)
    for (size_t j = i + 1; j < e->fieldNames.size(); ++j)
      if (e->fieldNames[i] == e->fieldNames[j])
        diag_.error(e->fieldValues[j]->span, "E0068",
                    "フィールド '" + e->fieldNames[j] +
                        "' が重複して指定されています");

  auto findDecl = [&](const std::string &n) -> const StructField * {
    for (auto &f : sd->fields)
      if (f.name == n)
        return &f;
    return nullptr;
  };

  // 非総称: 従来どおりフィールド型で検査
  if (sd->typeParams.empty()) {
    for (size_t i = 0; i < e->fieldValues.size(); ++i) {
      const StructField *decl = findDecl(e->fieldNames[i]);
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

  // ジェネリック: 期待型 + フィールド値から型引数を推論する。
  // 推論変数 (非アクティブ Param) を含む期待要素はフィールドで決める。
  std::map<std::string, Type> subst;
  if (expected && expected->isStruct() && expected->name == e->structName &&
      expected->elems.size() == sd->typeParams.size())
    for (size_t i = 0; i < sd->typeParams.size(); ++i)
      if (!hasNonActiveParam(expected->elems[i]))
        subst[sd->typeParams[i]] = expected->elems[i];
  for (size_t i = 0; i < e->fieldValues.size(); ++i) {
    const StructField *decl = findDecl(e->fieldNames[i]);
    if (!decl) {
      diag_.error(e->fieldValues[i]->span, "E0062",
                  "そのようなフィールドはありません: " + e->fieldNames[i]);
      check(e->fieldValues[i].get(), std::nullopt);
      continue;
    }
    Type hint = substType(decl->type, subst);
    Type ft = check(e->fieldValues[i].get(),
                    hasParam(hint) ? std::nullopt : std::optional<Type>(hint));
    unifyParam(decl->type, ft, subst);
  }
  std::vector<Type> args;
  for (auto &p : sd->typeParams) {
    auto sit = subst.find(p);
    if (sit == subst.end()) {
      diag_.error(e->span, "E0097",
                  "型引数を推論できません: " + e->structName + " の " + p +
                      " (型注釈が必要です)");
      return Type::unknown();
    }
    args.push_back(sit->second);
  }
  // 解決後のフィールド型で各値を再検証
  for (size_t i = 0; i < e->fieldValues.size(); ++i) {
    const StructField *decl = findDecl(e->fieldNames[i]);
    if (!decl)
      continue;
    Type want = substType(decl->type, subst);
    if (e->fieldValues[i]->type.isKnown() && e->fieldValues[i]->type != want)
      diag_.error(e->fieldValues[i]->span, "E0063",
                  "フィールド '" + decl->name + "' の型が一致しません (期待 " +
                      want.str() + ", 実際 " + e->fieldValues[i]->type.str() +
                      ")");
  }
  Type t = Type::structTy(e->structName);
  t.elems = std::move(args);
  return t;
}

Type Sema::checkField(FieldExpr *e) {
  Type ot = check(e->operand.get(), std::nullopt);
  // &Struct は自動で deref する (&self メソッド本体の self.field など)
  Type st = ot.isRef() ? ot.pointee() : ot;
  if (!st.isStruct()) {
    if (st.isKnown())
      diag_.error(e->operand->span, "E0064",
                  "フィールドアクセスには構造体が必要です (実際 " + ot.str() +
                      ")");
    return Type::unknown();
  }
  const StructDef *sd = findStruct(st.name);
  if (!sd)
    return Type::unknown();
  // ジェネリックなら型引数でフィールド型を具体化する
  std::map<std::string, Type> subst;
  if (!sd->typeParams.empty() && st.elems.size() == sd->typeParams.size())
    for (size_t i = 0; i < sd->typeParams.size(); ++i)
      subst[sd->typeParams[i]] = st.elems[i];
  for (size_t i = 0; i < sd->fields.size(); ++i)
    if (sd->fields[i].name == e->field) {
      e->fieldIndex = static_cast<int>(i);
      return substType(sd->fields[i].type, subst);
    }
  diag_.error(e->fieldSpan, "E0065",
              "構造体 " + st.name + " にフィールド '" + e->field +
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
    auto *d = static_cast<const DerefExpr *>(e);
    // *b (Box) は box が可変な場所なら書ける。*p (&mut) は p が &mut のとき。
    if (d->operand->type.isBox())
      return isMutablePlace(d->operand.get());
    return d->operand->type.isRef() && d->operand->type.refMut;
  }
  case Expr::Kind::Field: {
    auto *f = static_cast<const FieldExpr *>(e);
    // &Struct 経由なら &mut のときのみ可。値なら基底が可変かで決まる。
    if (f->operand->type.isRef())
      return f->operand->type.refMut;
    return isMutablePlace(f->operand.get());
  }
  case Expr::Kind::TupleIndex:
    return isMutablePlace(static_cast<const TupleIndexExpr *>(e)->operand.get());
  case Expr::Kind::Index: {
    auto *ix = static_cast<const IndexExpr *>(e);
    // スライス越しの書き込みは &mut [T] のときのみ可 (束縛の mut とは独立)。
    if (ix->base->type.isSlice())
      return ix->base->type.refMut;
    return isMutablePlace(ix->base.get()); // 配列: ベースが可変なら可変
  }
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

  Type result;
  if (e->tail)
    result = check(e->tail.get(), expected);
  else if (!e->stmts.empty() && e->stmts.back().kind == Stmt::Kind::Expr &&
           definitelyDiverges(e->stmts.back().expr.get()))
    result = Type::unknown(); // 末尾の文が発散 → ブロック全体も発散扱い
  else
    result = Type::unit();

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

Type Sema::checkArrayLit(ArrayLitExpr *e, std::optional<Type> expected) {
  std::optional<Type> elemHint;
  if (expected && expected->isArray())
    elemHint = expected->elemType();

  if (e->elems.empty()) {
    if (elemHint)
      return Type::arrayTy(*elemHint, 0);
    diag_.error(e->span, "E0190",
                "空の配列リテラルからは要素型を推論できません (型注釈が必要です)");
    return Type::unknown();
  }

  Type elemT = check(e->elems[0].get(), elemHint);
  for (size_t i = 1; i < e->elems.size(); ++i) {
    std::optional<Type> hint =
        elemT.isKnown() ? std::optional<Type>(elemT) : elemHint;
    Type t = check(e->elems[i].get(), hint);
    if (t.isKnown() && elemT.isKnown() && t != elemT)
      diag_.error(e->elems[i]->span, "E0191",
                  "配列要素の型が一致しません (" + elemT.str() + " と " + t.str() +
                      ")");
  }
  return Type::arrayTy(elemT, static_cast<unsigned>(e->elems.size()));
}

Type Sema::checkIndex(IndexExpr *e) {
  Type bt = check(e->base.get(), std::nullopt);
  Type it = check(e->index.get(), Type::intTy(64, true));
  if (it.isKnown() && !it.isInt())
    diag_.error(e->index->span, "E0192",
                "添字には整数型が必要です (実際 " + it.str() + ")");
  if (!bt.isArray() && !bt.isSlice()) {
    if (bt.isKnown())
      diag_.error(e->base->span, "E0193",
                  "配列・スライスではないものに添字アクセスしています (実際 " +
                      bt.str() + ")");
    return Type::unknown();
  }
  return bt.elemType();
}

Type Sema::checkReturn(ReturnExpr *e) {
  if (!currentRetType_) {
    diag_.error(e->span, "E0240", "return は関数本体の中でのみ使えます");
    if (e->value)
      check(e->value.get(), std::nullopt);
    return Type::unknown();
  }
  Type rt = *currentRetType_;
  if (e->value) {
    Type vt = check(e->value.get(), rt);
    if (vt.isKnown() && vt != rt)
      diag_.error(e->value->span, "E0241",
                  "return の型が戻り値型と一致しません (期待 " + rt.str() +
                      ", 実際 " + vt.str() + ")");
  } else if (!rt.isUnit()) {
    diag_.error(e->span, "E0241",
                "値のない return ですが戻り値型は " + rt.str() + " です");
  }
  return Type::unknown(); // 発散する
}

Type Sema::checkTry(TryExpr *e) {
  Type ot = check(e->operand.get(), std::nullopt);
  if (!currentRetType_) {
    diag_.error(e->span, "E0242", "? は関数本体の中でのみ使えます");
    return Type::unknown();
  }
  Type rt = *currentRetType_;
  if (ot.isEnum() && ot.name == "Option" && ot.elems.size() == 1) {
    e->kind = 0;
    if (!(rt.isEnum() && rt.name == "Option"))
      diag_.error(e->span, "E0243",
                  "? (Option) は戻り値型が Option の関数でのみ使えます (実際 " +
                      rt.str() + ")");
    return ot.elems[0];
  }
  if (ot.isEnum() && ot.name == "Result" && ot.elems.size() == 2) {
    e->kind = 1;
    if (!(rt.isEnum() && rt.name == "Result" && rt.elems.size() == 2 &&
          rt.elems[1] == ot.elems[1]))
      diag_.error(e->span, "E0243",
                  "? (Result) は同じエラー型 E を返す Result 関数でのみ使えます");
    return ot.elems[0];
  }
  if (ot.isKnown())
    diag_.error(e->operand->span, "E0244",
                "? は Option か Result にのみ使えます (実際 " + ot.str() + ")");
  return Type::unknown();
}

Type Sema::checkMethodCall(MethodCallExpr *e) {
  Type rt = check(e->receiver.get(), std::nullopt);
  // レシーバが参照なら自動で deref して、その指す型のメソッドを探す
  e->recvIsRef = rt.isRef();
  Type baseT = rt.isRef() ? rt.pointee() : rt;

  // 型引数レシーバ: 境界トレイトのメソッドとして解決する (単態化で実型に解決)
  if (baseT.isParam()) {
    const Prototype *tm = nullptr;
    auto bit = paramBounds_.find(baseT.name);
    if (bit != paramBounds_.end())
      for (auto &tn : bit->second) {
        auto trIt = traits_.find(tn);
        if (trIt == traits_.end())
          continue;
        for (auto &m : trIt->second->methods)
          if (m->name == e->method) {
            tm = m.get();
            break;
          }
        if (tm)
          break;
      }
    if (!tm) {
      diag_.error(e->methodSpan, "E0211",
                  "型引数 " + baseT.name + " の境界にメソッド '" + e->method +
                      "' はありません");
      for (auto &a : e->args)
        check(a.get(), std::nullopt);
      return Type::unknown();
    }
    e->ownerType = baseT.name; // CodeGen は単態化後の実型から再導出する
    e->selfKind = !tm->paramTypes[0].isRef()
                      ? 0
                      : (tm->paramTypes[0].refMut ? 2 : 1);
    // レシーバの適合性 (具体型分岐と同じ規則を境界経由でも適用する)
    if (e->selfKind == 0 && rt.isRef())
      diag_.error(e->receiver->span, "E0212",
                  "値レシーバのメソッドには値が必要です (参照が渡されています)");
    if (e->selfKind == 2) { // &mut self
      if (rt.isRef()) {
        if (!rt.refMut)
          diag_.error(e->receiver->span, "E0213",
                      "&mut self メソッドには可変な参照/場所が必要です");
      } else if (!isMutablePlace(e->receiver.get())) {
        diag_.error(e->receiver->span, "E0213",
                    "&mut self メソッドには可変な場所が必要です (let mut が必要)");
      }
    }
    size_t want = tm->paramTypes.size() - 1;
    if (want != e->args.size()) {
      diag_.error(e->span, "E0103",
                  "引数の数が一致しません (期待 " + std::to_string(want) +
                      ", 実際 " + std::to_string(e->args.size()) + ")");
      for (auto &a : e->args)
        check(a.get(), std::nullopt);
      return tm->retType;
    }
    for (size_t i = 0; i < e->args.size(); ++i) {
      Type pt = tm->paramTypes[i + 1];
      Type at = check(e->args[i].get(), pt);
      if (at.isKnown() && at != pt)
        diag_.error(e->args[i]->span, "E0104",
                    "引数の型が一致しません (期待 " + pt.str() + ", 実際 " +
                        at.str() + ")");
    }
    return tm->retType;
  }

  if (!baseT.isStruct() && !baseT.isEnum()) {
    if (baseT.isKnown())
      diag_.error(e->receiver->span, "E0210",
                  "メソッド呼び出しには struct / enum が必要です (実際 " +
                      rt.str() + ")");
    return Type::unknown();
  }
  auto tit = methods_.find(baseT.name);
  const FunctionDef *m =
      tit != methods_.end() && tit->second.count(e->method)
          ? tit->second.at(e->method)
          : nullptr;
  if (!m) {
    diag_.error(e->methodSpan, "E0211",
                "型 " + baseT.name + " にメソッド '" + e->method +
                    "' はありません");
    for (auto &a : e->args)
      check(a.get(), std::nullopt);
    return Type::unknown();
  }
  const Prototype *proto = m->proto.get();
  e->ownerType = baseT.name;

  // impl の型引数 = レシーバ型の型引数 (位置対応)
  std::map<std::string, Type> subst;
  for (size_t i = 0; i < proto->typeParams.size() && i < baseT.elems.size(); ++i)
    subst[proto->typeParams[i]] = baseT.elems[i];
  e->typeArgs.clear();
  for (auto &p : proto->typeParams)
    e->typeArgs.push_back(subst.count(p) ? subst[p] : Type::unknown());

  // self の種類 (paramTypes[0] から)
  const Type &selfT = proto->paramTypes[0];
  e->selfKind = !selfT.isRef() ? 0 : (selfT.refMut ? 2 : 1);

  // レシーバの適合性
  if (e->selfKind == 0 && rt.isRef())
    diag_.error(e->receiver->span, "E0212",
                "値レシーバのメソッドには値が必要です (参照が渡されています)");
  if (e->selfKind == 2) { // &mut self
    if (rt.isRef()) {
      if (!rt.refMut)
        diag_.error(e->receiver->span, "E0213",
                    "&mut self メソッドには可変な参照/場所が必要です");
    } else if (!isMutablePlace(e->receiver.get())) {
      diag_.error(e->receiver->span, "E0213",
                  "&mut self メソッドには可変な場所が必要です (let mut が必要)");
    }
  }

  // 引数 (self を除く paramTypes[1..]) を検査
  size_t want = proto->paramTypes.size() - 1;
  if (want != e->args.size()) {
    diag_.error(e->span, "E0103",
                "引数の数が一致しません (期待 " + std::to_string(want) +
                    ", 実際 " + std::to_string(e->args.size()) + ")");
    for (auto &a : e->args)
      check(a.get(), std::nullopt);
    return substType(proto->retType, subst);
  }
  for (size_t i = 0; i < e->args.size(); ++i) {
    Type pt = substType(proto->paramTypes[i + 1], subst);
    Type at = check(e->args[i].get(), pt);
    if (at.isKnown() && at != pt)
      diag_.error(e->args[i]->span, "E0104",
                  "引数の型が一致しません (期待 " + pt.str() + ", 実際 " +
                      at.str() + ")");
  }
  return substType(proto->retType, subst);
}

Type Sema::checkBorrow(BorrowExpr *e, std::optional<Type> expected) {
  // 期待が参照型なら、その指す型をヒントとして伝える。
  // 期待がスライス &[T] なら、借用元の配列リテラルへ要素型を伝える
  // (長さは配列リテラル側が決めるので 0 で良い)。
  std::optional<Type> hint;
  if (expected && expected->isRef())
    hint = expected->pointee();
  else if (expected && expected->isSlice())
    hint = Type::arrayTy(expected->elemType(), 0);
  Type t = check(e->operand.get(), hint);
  if (e->isMut && !isMutablePlace(e->operand.get()))
    diag_.error(e->span, "E0172",
                "&mut で借用するには可変な場所が必要です (let mut が必要)");
  // 配列の借用はスライス &[T] になる (fat pointer)。それ以外は通常の参照。
  if (t.isArray())
    return Type::sliceTy(t.elemType(), e->isMut);
  return Type::refTy(t, e->isMut);
}

Type Sema::checkDeref(DerefExpr *e) {
  Type t = check(e->operand.get(), std::nullopt);
  if (t.isBox()) // *b: Box<T> の中身を取り出す
    return t.boxedType();
  if (!t.isRef()) {
    if (t.isKnown())
      diag_.error(e->span, "E0160",
                  "参照や Box ではないものを参照外ししています (実際 " + t.str() +
                      ")");
    return Type::unknown();
  }
  return t.pointee();
}

Type Sema::checkMatch(MatchExpr *e, std::optional<Type> expected) {
  Type rt = check(e->scrutinee.get(), std::nullopt);
  // &Enum は自動で deref する (&self メソッド本体の match self など)
  Type st = rt.isRef() ? rt.pointee() : rt;
  if (!st.isEnum()) {
    if (rt.isKnown())
      diag_.error(e->scrutinee->span, "E0090",
                  "match の対象は enum である必要があります (実際 " + rt.str() +
                      ")");
    for (auto &arm : e->arms)
      check(arm.body.get(), expected);
    return Type::unknown();
  }
  const EnumDef *ed = findEnum(st.name);
  if (!ed)
    return Type::unknown();

  // ジェネリック enum なら、対象の型引数でペイロード型を具体化する
  std::map<std::string, Type> subst;
  if (!ed->typeParams.empty() && st.elems.size() == ed->typeParams.size())
    for (size_t i = 0; i < ed->typeParams.size(); ++i)
      subst[ed->typeParams[i]] = st.elems[i];

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
      // 型引数を適用した具体ペイロード型 (非総称なら v.payloadTypes と同じ)
      std::vector<Type> pts;
      for (auto &pt : v.payloadTypes)
        pts.push_back(substType(pt, subst));
      arm.payloadTypes = pts;
      if (covered[idx])
        diag_.warning(arm.variantSpan, "E0092",
                      "このバリアントは既に網羅されています");
      covered[idx] = true;
      if (arm.bindings.size() != pts.size())
        diag_.error(arm.variantSpan, "E0093",
                    "束縛の数がペイロードと一致しません (期待 " +
                        std::to_string(pts.size()) + ", 実際 " +
                        std::to_string(arm.bindings.size()) + ")");
      for (size_t i = 0; i < arm.bindings.size() && i < pts.size(); ++i)
        if (arm.bindings[i] != "_")
          binds.push_back({arm.bindings[i], pts[i]});
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
