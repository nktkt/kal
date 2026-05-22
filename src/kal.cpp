//===- kal.cpp - Kal v0.1: a tiny expression language on LLVM -------------===//
//
// Kal は「電卓・式言語」(Kaleidoscope 系) の最小実装です。
//   - 値はすべて倍精度浮動小数点 (double)
//   - def / extern / if-then-else / for / 再帰 / 二項演算子
//   - ソースを LLVM IR に変換し、ORC JIT でその場実行
//
// パイプライン:  ソース → [Lexer] → トークン → [Parser] → AST
//                       → [CodeGen] → LLVM IR → [ORC JIT] → 実行
//
// 使い方:
//   kalc program.kal        # ファイルを実行
//   kalc < program.kal      # 標準入力から実行
//   kalc --emit-ir prog.kal # 実行せず LLVM IR を表示
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/APFloat.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"

#include <cctype>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace llvm;

//===----------------------------------------------------------------------===//
// 1. Lexer — 文字列をトークン列に分解する
//===----------------------------------------------------------------------===//

enum Token {
  tok_eof = -1,
  // キーワード
  tok_def = -2,
  tok_extern = -3,
  tok_if = -4,
  tok_then = -5,
  tok_else = -6,
  tok_for = -7,
  tok_in = -8,
  // 主要トークン
  tok_identifier = -9,
  tok_number = -10,
};

// 入力ソース全体と現在位置 (グローバルに保持してシンプルに)
static std::string g_src;
static size_t g_pos = 0;

static std::string g_identifierStr; // tok_identifier のとき有効
static double g_numVal;             // tok_number のとき有効

static int readChar() { return g_pos < g_src.size() ? g_src[g_pos++] : EOF; }
static int peekChar() { return g_pos < g_src.size() ? g_src[g_pos] : EOF; }

/// 次のトークンを 1 つ取り出す。
static int gettok() {
  static int lastChar = ' ';

  // 空白を読み飛ばす
  while (isspace(lastChar))
    lastChar = readChar();

  // 識別子 / キーワード: [a-zA-Z][a-zA-Z0-9_]*
  if (isalpha(lastChar)) {
    g_identifierStr = (char)lastChar;
    while (isalnum((lastChar = readChar())) || lastChar == '_')
      g_identifierStr += (char)lastChar;

    if (g_identifierStr == "def")    return tok_def;
    if (g_identifierStr == "extern") return tok_extern;
    if (g_identifierStr == "if")     return tok_if;
    if (g_identifierStr == "then")   return tok_then;
    if (g_identifierStr == "else")   return tok_else;
    if (g_identifierStr == "for")    return tok_for;
    if (g_identifierStr == "in")     return tok_in;
    return tok_identifier;
  }

  // 数値: 整数・小数 (例: 1, 3.14, .5)
  if (isdigit(lastChar) || lastChar == '.') {
    std::string num;
    do {
      num += (char)lastChar;
      lastChar = readChar();
    } while (isdigit(lastChar) || lastChar == '.');
    g_numVal = strtod(num.c_str(), nullptr);
    return tok_number;
  }

  // コメント: '#' から行末まで
  if (lastChar == '#') {
    do
      lastChar = readChar();
    while (lastChar != EOF && lastChar != '\n' && lastChar != '\r');
    if (lastChar != EOF)
      return gettok();
  }

  if (lastChar == EOF)
    return tok_eof;

  // それ以外は文字そのものをトークンとして返す ('+', '(', ';' など)
  int thisChar = lastChar;
  lastChar = readChar();
  return thisChar;
}

//===----------------------------------------------------------------------===//
// 2. AST — 抽象構文木のノード定義
//===----------------------------------------------------------------------===//

namespace {

/// 式ノードの基底
class ExprAST {
public:
  virtual ~ExprAST() = default;
  virtual Value *codegen() = 0;
};

/// 数値リテラル: "1.0"
class NumberExprAST : public ExprAST {
  double val;
public:
  NumberExprAST(double val) : val(val) {}
  Value *codegen() override;
};

/// 変数参照: "x"
class VariableExprAST : public ExprAST {
  std::string name;
public:
  VariableExprAST(std::string name) : name(std::move(name)) {}
  Value *codegen() override;
};

/// 二項演算: "lhs op rhs"
class BinaryExprAST : public ExprAST {
  char op;
  std::unique_ptr<ExprAST> lhs, rhs;
public:
  BinaryExprAST(char op, std::unique_ptr<ExprAST> lhs,
                std::unique_ptr<ExprAST> rhs)
      : op(op), lhs(std::move(lhs)), rhs(std::move(rhs)) {}
  Value *codegen() override;
};

/// 関数呼び出し: "callee(args...)"
class CallExprAST : public ExprAST {
  std::string callee;
  std::vector<std::unique_ptr<ExprAST>> args;
public:
  CallExprAST(std::string callee, std::vector<std::unique_ptr<ExprAST>> args)
      : callee(std::move(callee)), args(std::move(args)) {}
  Value *codegen() override;
};

/// if/then/else 式 (値を返す)
class IfExprAST : public ExprAST {
  std::unique_ptr<ExprAST> cond, then, els;
public:
  IfExprAST(std::unique_ptr<ExprAST> cond, std::unique_ptr<ExprAST> then,
            std::unique_ptr<ExprAST> els)
      : cond(std::move(cond)), then(std::move(then)), els(std::move(els)) {}
  Value *codegen() override;
};

/// for ループ: "for i = start, cond, step in body"
class ForExprAST : public ExprAST {
  std::string varName;
  std::unique_ptr<ExprAST> start, cond, step, body;
public:
  ForExprAST(std::string varName, std::unique_ptr<ExprAST> start,
             std::unique_ptr<ExprAST> cond, std::unique_ptr<ExprAST> step,
             std::unique_ptr<ExprAST> body)
      : varName(std::move(varName)), start(std::move(start)),
        cond(std::move(cond)), step(std::move(step)), body(std::move(body)) {}
  Value *codegen() override;
};

/// 関数プロトタイプ: 名前と引数名 (引数はすべて double)
class PrototypeAST {
  std::string name;
  std::vector<std::string> args;
public:
  PrototypeAST(std::string name, std::vector<std::string> args)
      : name(std::move(name)), args(std::move(args)) {}
  const std::string &getName() const { return name; }
  Function *codegen();
};

/// 関数定義: プロトタイプ + 本体
class FunctionAST {
  std::unique_ptr<PrototypeAST> proto;
  std::unique_ptr<ExprAST> body;
public:
  FunctionAST(std::unique_ptr<PrototypeAST> proto,
              std::unique_ptr<ExprAST> body)
      : proto(std::move(proto)), body(std::move(body)) {}
  Function *declare(); // プロトタイプだけ先に宣言する
  Function *codegen(); // 本体を生成する
};

} // end anonymous namespace

//===----------------------------------------------------------------------===//
// 3. Parser — トークン列を AST に変換 (再帰下降 + 演算子優先順位)
//===----------------------------------------------------------------------===//

static int g_curTok;
static int getNextToken() { return g_curTok = gettok(); }

/// 二項演算子の優先順位 (大きいほど強く結合)
static std::map<char, int> g_binopPrec = {
    {'<', 10}, {'>', 10}, {'+', 20}, {'-', 20}, {'*', 40}, {'/', 40}};

static int getTokPrecedence() {
  if (!isascii(g_curTok))
    return -1;
  auto it = g_binopPrec.find((char)g_curTok);
  return it == g_binopPrec.end() ? -1 : it->second;
}

static std::unique_ptr<ExprAST> logError(const char *str) {
  fprintf(stderr, "構文エラー: %s\n", str);
  return nullptr;
}
static std::unique_ptr<PrototypeAST> logErrorP(const char *str) {
  logError(str);
  return nullptr;
}

static std::unique_ptr<ExprAST> parseExpression();

/// numberexpr ::= number
static std::unique_ptr<ExprAST> parseNumberExpr() {
  auto result = std::make_unique<NumberExprAST>(g_numVal);
  getNextToken(); // 数値を消費
  return std::move(result);
}

/// parenexpr ::= '(' expression ')'
static std::unique_ptr<ExprAST> parseParenExpr() {
  getNextToken(); // '('
  auto v = parseExpression();
  if (!v)
    return nullptr;
  if (g_curTok != ')')
    return logError("')' が必要です");
  getNextToken(); // ')'
  return v;
}

/// identifierexpr ::= identifier | identifier '(' args ')'
static std::unique_ptr<ExprAST> parseIdentifierExpr() {
  std::string idName = g_identifierStr;
  getNextToken(); // 識別子を消費

  if (g_curTok != '(') // 単なる変数参照
    return std::make_unique<VariableExprAST>(idName);

  // 関数呼び出し
  getNextToken(); // '('
  std::vector<std::unique_ptr<ExprAST>> args;
  if (g_curTok != ')') {
    while (true) {
      if (auto arg = parseExpression())
        args.push_back(std::move(arg));
      else
        return nullptr;
      if (g_curTok == ')')
        break;
      if (g_curTok != ',')
        return logError("引数リストには ',' か ')' が必要です");
      getNextToken();
    }
  }
  getNextToken(); // ')'
  return std::make_unique<CallExprAST>(idName, std::move(args));
}

/// ifexpr ::= 'if' expr 'then' expr 'else' expr
static std::unique_ptr<ExprAST> parseIfExpr() {
  getNextToken(); // 'if'
  auto cond = parseExpression();
  if (!cond)
    return nullptr;
  if (g_curTok != tok_then)
    return logError("'then' が必要です");
  getNextToken(); // 'then'
  auto then = parseExpression();
  if (!then)
    return nullptr;
  if (g_curTok != tok_else)
    return logError("'else' が必要です");
  getNextToken(); // 'else'
  auto els = parseExpression();
  if (!els)
    return nullptr;
  return std::make_unique<IfExprAST>(std::move(cond), std::move(then),
                                     std::move(els));
}

/// forexpr ::= 'for' id '=' expr ',' expr (',' expr)? 'in' expr
static std::unique_ptr<ExprAST> parseForExpr() {
  getNextToken(); // 'for'
  if (g_curTok != tok_identifier)
    return logError("'for' の後にはループ変数名が必要です");
  std::string idName = g_identifierStr;
  getNextToken();

  if (g_curTok != '=')
    return logError("ループ変数の後に '=' が必要です");
  getNextToken();

  auto start = parseExpression();
  if (!start)
    return nullptr;
  if (g_curTok != ',')
    return logError("開始値の後に ',' が必要です");
  getNextToken();

  auto cond = parseExpression();
  if (!cond)
    return nullptr;

  // ステップは省略可 (省略時は 1.0)
  std::unique_ptr<ExprAST> step;
  if (g_curTok == ',') {
    getNextToken();
    step = parseExpression();
    if (!step)
      return nullptr;
  }

  if (g_curTok != tok_in)
    return logError("ループ条件の後に 'in' が必要です");
  getNextToken();

  auto body = parseExpression();
  if (!body)
    return nullptr;

  return std::make_unique<ForExprAST>(idName, std::move(start), std::move(cond),
                                      std::move(step), std::move(body));
}

/// primary ::= identifierexpr | numberexpr | parenexpr | ifexpr | forexpr
static std::unique_ptr<ExprAST> parsePrimary() {
  switch (g_curTok) {
  case tok_identifier:
    return parseIdentifierExpr();
  case tok_number:
    return parseNumberExpr();
  case '(':
    return parseParenExpr();
  case tok_if:
    return parseIfExpr();
  case tok_for:
    return parseForExpr();
  default:
    return logError("式が必要なところに未知のトークンがあります");
  }
}

/// 演算子優先順位に従って二項演算の右側を結合していく
static std::unique_ptr<ExprAST> parseBinOpRHS(int exprPrec,
                                              std::unique_ptr<ExprAST> lhs) {
  while (true) {
    int tokPrec = getTokPrecedence();
    if (tokPrec < exprPrec) // これ以上強い演算子がなければ終了
      return lhs;

    char binOp = (char)g_curTok;
    getNextToken();

    auto rhs = parsePrimary();
    if (!rhs)
      return nullptr;

    // 右隣の演算子がより強ければ、先に右側を結合する
    int nextPrec = getTokPrecedence();
    if (tokPrec < nextPrec) {
      rhs = parseBinOpRHS(tokPrec + 1, std::move(rhs));
      if (!rhs)
        return nullptr;
    }

    lhs = std::make_unique<BinaryExprAST>(binOp, std::move(lhs), std::move(rhs));
  }
}

/// expression ::= primary binoprhs
static std::unique_ptr<ExprAST> parseExpression() {
  auto lhs = parsePrimary();
  if (!lhs)
    return nullptr;
  return parseBinOpRHS(0, std::move(lhs));
}

/// prototype ::= id '(' id* ')'
static std::unique_ptr<PrototypeAST> parsePrototype() {
  if (g_curTok != tok_identifier)
    return logErrorP("関数名が必要です");
  std::string fnName = g_identifierStr;
  getNextToken();

  if (g_curTok != '(')
    return logErrorP("プロトタイプには '(' が必要です");

  std::vector<std::string> argNames;
  while (getNextToken() == tok_identifier)
    argNames.push_back(g_identifierStr);
  if (g_curTok != ')')
    return logErrorP("プロトタイプには ')' が必要です");
  getNextToken(); // ')'

  return std::make_unique<PrototypeAST>(fnName, std::move(argNames));
}

/// definition ::= 'def' prototype expression
static std::unique_ptr<FunctionAST> parseDefinition() {
  getNextToken(); // 'def'
  auto proto = parsePrototype();
  if (!proto)
    return nullptr;
  if (auto e = parseExpression())
    return std::make_unique<FunctionAST>(std::move(proto), std::move(e));
  return nullptr;
}

/// external ::= 'extern' prototype
static std::unique_ptr<PrototypeAST> parseExtern() {
  getNextToken(); // 'extern'
  return parsePrototype();
}

//===----------------------------------------------------------------------===//
// 4. CodeGen — AST から LLVM IR を生成
//===----------------------------------------------------------------------===//

static std::unique_ptr<LLVMContext> g_ctx;
static std::unique_ptr<Module> g_module;
static std::unique_ptr<IRBuilder<>> g_builder;
static std::map<std::string, Value *> g_namedValues; // 関数内の引数/変数

static Value *logErrorV(const char *str) {
  logError(str);
  return nullptr;
}

static Type *doubleTy() { return Type::getDoubleTy(*g_ctx); }

Value *NumberExprAST::codegen() {
  return ConstantFP::get(*g_ctx, APFloat(val));
}

Value *VariableExprAST::codegen() {
  auto it = g_namedValues.find(name);
  if (it == g_namedValues.end())
    return logErrorV("未定義の変数です");
  return it->second;
}

Value *BinaryExprAST::codegen() {
  Value *l = lhs->codegen();
  Value *r = rhs->codegen();
  if (!l || !r)
    return nullptr;

  switch (op) {
  case '+':
    return g_builder->CreateFAdd(l, r, "addtmp");
  case '-':
    return g_builder->CreateFSub(l, r, "subtmp");
  case '*':
    return g_builder->CreateFMul(l, r, "multmp");
  case '/':
    return g_builder->CreateFDiv(l, r, "divtmp");
  case '<':
    // i1 (真偽) を double (0.0/1.0) に変換して返す
    l = g_builder->CreateFCmpULT(l, r, "cmptmp");
    return g_builder->CreateUIToFP(l, doubleTy(), "booltmp");
  case '>':
    l = g_builder->CreateFCmpUGT(l, r, "cmptmp");
    return g_builder->CreateUIToFP(l, doubleTy(), "booltmp");
  default:
    return logErrorV("未知の二項演算子です");
  }
}

Value *CallExprAST::codegen() {
  Function *calleeF = g_module->getFunction(callee);
  if (!calleeF)
    return logErrorV("未定義の関数を呼び出しています");
  if (calleeF->arg_size() != args.size())
    return logErrorV("引数の数が一致しません");

  std::vector<Value *> argsV;
  for (auto &a : args) {
    argsV.push_back(a->codegen());
    if (!argsV.back())
      return nullptr;
  }
  return g_builder->CreateCall(calleeF, argsV, "calltmp");
}

Value *IfExprAST::codegen() {
  Value *condV = cond->codegen();
  if (!condV)
    return nullptr;

  // 条件を 0.0 と比較して真偽 (i1) にする
  condV = g_builder->CreateFCmpONE(
      condV, ConstantFP::get(*g_ctx, APFloat(0.0)), "ifcond");

  Function *fn = g_builder->GetInsertBlock()->getParent();
  BasicBlock *thenBB = BasicBlock::Create(*g_ctx, "then", fn);
  BasicBlock *elseBB = BasicBlock::Create(*g_ctx, "else");
  BasicBlock *mergeBB = BasicBlock::Create(*g_ctx, "ifcont");

  g_builder->CreateCondBr(condV, thenBB, elseBB);

  // then ブロック
  g_builder->SetInsertPoint(thenBB);
  Value *thenV = then->codegen();
  if (!thenV)
    return nullptr;
  g_builder->CreateBr(mergeBB);
  thenBB = g_builder->GetInsertBlock(); // phi 用に最新ブロックを取得

  // else ブロック
  fn->insert(fn->end(), elseBB);
  g_builder->SetInsertPoint(elseBB);
  Value *elseV = els->codegen();
  if (!elseV)
    return nullptr;
  g_builder->CreateBr(mergeBB);
  elseBB = g_builder->GetInsertBlock();

  // merge ブロック: phi で両分岐の値を統合
  fn->insert(fn->end(), mergeBB);
  g_builder->SetInsertPoint(mergeBB);
  PHINode *phi = g_builder->CreatePHI(doubleTy(), 2, "iftmp");
  phi->addIncoming(thenV, thenBB);
  phi->addIncoming(elseV, elseBB);
  return phi;
}

Value *ForExprAST::codegen() {
  // 前判定ループ:  cond を満たす間だけ body を実行し、最後に i += step
  //   preheader → [cond] →(真) body → cond ...
  //                      →(偽) after
  Value *startV = start->codegen();
  if (!startV)
    return nullptr;

  Function *fn = g_builder->GetInsertBlock()->getParent();
  BasicBlock *preheaderBB = g_builder->GetInsertBlock();
  BasicBlock *condBB = BasicBlock::Create(*g_ctx, "loopcond", fn);
  BasicBlock *bodyBB = BasicBlock::Create(*g_ctx, "loopbody", fn);
  BasicBlock *afterBB = BasicBlock::Create(*g_ctx, "afterloop", fn);

  g_builder->CreateBr(condBB);

  // cond ブロック: ループ変数の phi と継続条件
  g_builder->SetInsertPoint(condBB);
  PHINode *var = g_builder->CreatePHI(doubleTy(), 2, varName);
  var->addIncoming(startV, preheaderBB);

  // 同名変数があれば退避してループ変数で隠す
  Value *oldVal = g_namedValues.count(varName) ? g_namedValues[varName] : nullptr;
  g_namedValues[varName] = var;

  Value *condV = cond->codegen();
  if (!condV)
    return nullptr;
  condV = g_builder->CreateFCmpONE(
      condV, ConstantFP::get(*g_ctx, APFloat(0.0)), "loopcheck");
  g_builder->CreateCondBr(condV, bodyBB, afterBB);

  // body ブロック: 本体を実行 → i += step → cond へ戻る
  g_builder->SetInsertPoint(bodyBB);
  if (!body->codegen()) // 本体の値は捨てる
    return nullptr;

  Value *stepV = step ? step->codegen() : ConstantFP::get(*g_ctx, APFloat(1.0));
  if (!stepV)
    return nullptr;
  Value *nextVar = g_builder->CreateFAdd(var, stepV, "nextvar");
  BasicBlock *bodyEndBB = g_builder->GetInsertBlock(); // body 内で分岐した場合に備える
  g_builder->CreateBr(condBB);
  var->addIncoming(nextVar, bodyEndBB);

  // after ブロック
  g_builder->SetInsertPoint(afterBB);

  // スコープを復元
  if (oldVal)
    g_namedValues[varName] = oldVal;
  else
    g_namedValues.erase(varName);

  // for 式は常に 0.0 を返す
  return ConstantFP::get(*g_ctx, APFloat(0.0));
}

Function *PrototypeAST::codegen() {
  std::vector<Type *> doubles(args.size(), doubleTy());
  FunctionType *ft = FunctionType::get(doubleTy(), doubles, false);
  Function *fn =
      Function::Create(ft, Function::ExternalLinkage, name, g_module.get());

  unsigned idx = 0;
  for (auto &arg : fn->args())
    arg.setName(args[idx++]);
  return fn;
}

Function *FunctionAST::declare() {
  if (Function *existing = g_module->getFunction(proto->getName()))
    return existing; // extern などで宣言済みなら再利用
  return proto->codegen();
}

Function *FunctionAST::codegen() {
  Function *fn = g_module->getFunction(proto->getName());
  if (!fn)
    return nullptr; // プロトタイプは事前に declare() 済みのはず

  BasicBlock *bb = BasicBlock::Create(*g_ctx, "entry", fn);
  g_builder->SetInsertPoint(bb);

  g_namedValues.clear();
  for (auto &arg : fn->args())
    g_namedValues[std::string(arg.getName())] = &arg;

  if (Value *ret = body->codegen()) {
    g_builder->CreateRet(ret);
    verifyFunction(*fn);
    return fn;
  }

  fn->eraseFromParent(); // 本体生成に失敗 → 関数を削除
  return nullptr;
}

//===----------------------------------------------------------------------===//
// 5. ランタイム — JIT したコードから呼べる組み込み関数
//===----------------------------------------------------------------------===//

extern "C" double printd(double x) {
  printf("%g\n", x);
  return 0;
}

extern "C" double putchard(double x) {
  putchar((int)x);
  return 0;
}

//===----------------------------------------------------------------------===//
// 6. ドライバ — パース → コード生成 → JIT 実行
//===----------------------------------------------------------------------===//

int main(int argc, char **argv) {
  // 引数解析
  bool emitIR = false;
  const char *path = nullptr;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--emit-ir" || a == "-S")
      emitIR = true;
    else if (a == "--help" || a == "-h") {
      printf("使い方: kalc [--emit-ir] [file.kal]\n"
             "  ファイル省略時は標準入力から読み込みます。\n");
      return 0;
    } else
      path = argv[i];
  }

  // ソースを全部読み込む
  if (path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
      fprintf(stderr, "ファイルを開けません: %s\n", path);
      return 1;
    }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
      g_src.append(buf, n);
    fclose(f);
  } else {
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), stdin)) > 0)
      g_src.append(buf, n);
  }

  // LLVM 初期化
  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();
  InitializeNativeTargetAsmParser();

  g_ctx = std::make_unique<LLVMContext>();
  g_module = std::make_unique<Module>("kal", *g_ctx);
  g_builder = std::make_unique<IRBuilder<>>(*g_ctx);

  // --- パース: トップレベル項目を 3 種類に振り分ける ---
  std::vector<std::unique_ptr<PrototypeAST>> externs;
  std::vector<std::unique_ptr<FunctionAST>> functions;
  std::vector<std::unique_ptr<ExprAST>> topExprs;

  getNextToken();
  while (g_curTok != tok_eof) {
    switch (g_curTok) {
    case ';': // 空文を読み飛ばす
      getNextToken();
      break;
    case tok_def:
      if (auto fn = parseDefinition())
        functions.push_back(std::move(fn));
      else
        return 1;
      break;
    case tok_extern:
      if (auto ex = parseExtern())
        externs.push_back(std::move(ex));
      else
        return 1;
      break;
    default:
      if (auto e = parseExpression())
        topExprs.push_back(std::move(e));
      else
        return 1;
      break;
    }
  }

  // --- コード生成 ---
  // (1) まず全プロトタイプを宣言 → 前方参照・相互再帰に対応
  for (auto &ex : externs)
    ex->codegen();
  for (auto &fn : functions)
    fn->declare();

  // 組み込み printd / putchard を宣言 (extern 省略でも使えるように)
  {
    FunctionType *ft = FunctionType::get(doubleTy(), {doubleTy()}, false);
    if (!g_module->getFunction("printd"))
      Function::Create(ft, Function::ExternalLinkage, "printd", g_module.get());
    if (!g_module->getFunction("putchard"))
      Function::Create(ft, Function::ExternalLinkage, "putchard", g_module.get());
  }

  // (2) 関数本体を生成
  for (auto &fn : functions)
    if (!fn->codegen())
      return 1;

  // (3) トップレベル式を集めて __main を生成
  {
    FunctionType *mainTy = FunctionType::get(Type::getVoidTy(*g_ctx), false);
    Function *mainFn = Function::Create(mainTy, Function::ExternalLinkage,
                                        "__main", g_module.get());
    BasicBlock *bb = BasicBlock::Create(*g_ctx, "entry", mainFn);
    g_builder->SetInsertPoint(bb);
    g_namedValues.clear();

    Function *printdFn = g_module->getFunction("printd");
    for (auto &e : topExprs) {
      Value *v = e->codegen();
      if (!v)
        return 1;
      g_builder->CreateCall(printdFn, {v}); // 各式の値を表示
    }
    g_builder->CreateRetVoid();
    verifyFunction(*mainFn);
  }

  // --emit-ir: 実行せず IR を表示して終了
  if (emitIR) {
    g_module->print(outs(), nullptr);
    return 0;
  }

  // --- ORC JIT で実行 ---
  auto jitOrErr = orc::LLJITBuilder().create();
  if (!jitOrErr) {
    errs() << "JIT 生成に失敗: " << toString(jitOrErr.takeError()) << "\n";
    return 1;
  }
  auto jit = std::move(*jitOrErr);

  // モジュールのデータレイアウトを JIT に合わせる
  g_module->setDataLayout(jit->getDataLayout());

  // 組み込み関数を JIT のシンボルとして登録
  orc::SymbolMap syms;
  syms[jit->mangleAndIntern("printd")] = {
      orc::ExecutorAddr::fromPtr(&printd), JITSymbolFlags::Exported};
  syms[jit->mangleAndIntern("putchard")] = {
      orc::ExecutorAddr::fromPtr(&putchard), JITSymbolFlags::Exported};
  if (auto err = jit->getMainJITDylib().define(orc::absoluteSymbols(syms))) {
    errs() << "シンボル登録に失敗: " << toString(std::move(err)) << "\n";
    return 1;
  }

  // 標準ライブラリ (sin, cos など) も解決できるようにする
  if (auto gen = orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
          jit->getDataLayout().getGlobalPrefix()))
    jit->getMainJITDylib().addGenerator(std::move(*gen));

  // モジュールを JIT に追加
  if (auto err = jit->addIRModule(
          orc::ThreadSafeModule(std::move(g_module), std::move(g_ctx)))) {
    errs() << "モジュール追加に失敗: " << toString(std::move(err)) << "\n";
    return 1;
  }

  // __main を引いて実行
  auto sym = jit->lookup("__main");
  if (!sym) {
    errs() << "__main が見つかりません: " << toString(sym.takeError()) << "\n";
    return 1;
  }
  auto *mainPtr = sym->toPtr<void (*)()>();
  mainPtr();
  return 0;
}
