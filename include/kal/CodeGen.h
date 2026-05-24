//===- CodeGen.h - lowers the AST to an LLVM module -----------------------===//
#pragma once

#include "kal/AST.h"
#include "kal/Diagnostic.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace kal {

class CodeGen {
public:
  CodeGen(llvm::LLVMContext &ctx, DiagnosticEngine &diag,
          const llvm::DataLayout &dl);

  /// Program を LLVM モジュールに変換する。
  /// emitRuntime=true なら自己完結バイナリ用に printi/printd/putchard の本体と
  /// C の main を生成する (AOT 用)。false なら宣言のみ (JIT がホスト側で解決)。
  /// 失敗時は nullptr を返す (診断は diag に報告済み)。
  std::unique_ptr<llvm::Module> run(const Program &program,
                                    bool emitRuntime = false);

private:
  llvm::Type *toLLVM(const Type &t); // kal::Type → llvm::Type (unit は void)

  llvm::Value *genExpr(const Expr *e);
  llvm::Value *genNumber(const NumberExpr *e);
  llvm::Value *genVariable(const VariableExpr *e);
  llvm::Value *genBinary(const BinaryExpr *e);
  llvm::Value *genUnary(const UnaryExpr *e);
  llvm::Value *genCall(const CallExpr *e);
  llvm::Value *genIf(const IfExpr *e);
  llvm::Value *genFor(const ForExpr *e);
  llvm::Value *genCast(const CastExpr *e);
  llvm::Value *genStructLit(const StructLitExpr *e);
  llvm::Value *genField(const FieldExpr *e);
  llvm::Value *genTupleLit(const TupleLitExpr *e);
  llvm::Value *genTupleIndex(const TupleIndexExpr *e);
  llvm::Value *genBlock(const BlockExpr *e);
  llvm::Value *genAssign(const AssignExpr *e);
  llvm::Value *genArrayLit(const ArrayLitExpr *e);
  llvm::Value *genIndex(const IndexExpr *e);
  llvm::Value *genMethodCall(const MethodCallExpr *e);
  llvm::Value *genReturn(const ReturnExpr *e);
  llvm::Value *genTry(const TryExpr *e);
  // Vec<T> = { T* ptr, i64 len, i64 cap }。LLVM 表現は要素型に依らず一定 (不透明 ptr)。
  llvm::StructType *vecLLVMTy();
  llvm::Value *genVecNew(const CallExpr *e); // 組み込み vec(): 空の Vec を作る
  llvm::Value *genPush(const CallExpr *e);   // 組み込み push(v, x): 末尾に追加
  llvm::Value *genPop(const CallExpr *e);     // 組み込み pop(v): 末尾を Option<T> で取り出す
  llvm::Value *genClear(const CallExpr *e);   // 組み込み clear(v): 全要素 drop して空に
  llvm::Value *genStringLit(const StringLitExpr *e); // "..." → {ptr, len}
  llvm::Value *genPushStr(const CallExpr *e);        // push_str(s, t): str を追記
  // 現在のブロックが終端命令を持つ (return 等で発散した) か。
  bool blockDone();
  llvm::Value *genMatch(const MatchExpr *e);
  llvm::Value *genBorrow(const BorrowExpr *e);
  llvm::Value *genDeref(const DerefExpr *e);
  // 場所 (lvalue) のアドレスを返す。一時値はメモリに退避する。
  llvm::Value *genAddr(const Expr *e);
  // entry ブロックに alloca を作る (mem2reg が昇格できる)
  llvm::AllocaInst *entryAlloca(llvm::Type *ty, const std::string &name);
  // 添字 idx が [0, len) の範囲外なら kal_panic を呼ぶコードを挿入する。
  void emitBoundsCheck(llvm::Value *idx, const Type &idxType, llvm::Value *len);

  // --- Drop / RAII ---
  bool needsDrop(const Type &t);          // 値として Box を含む (解放が要る) か
  llvm::Function *getDropFn(const Type &t); // 型 t のドロップグルー関数 (遅延生成)
  void genDropBody(const Type &t, llvm::Function *fn); // ドロップグルーの本体生成
  // ドロップ対象のローカル (場所 slot・生存フラグ flag・型) を現スコープに登録。
  // needsDrop(t) なら i1 フラグ (初期 true) を作って積み、戻り値とする。
  void registerLocal(llvm::Value *slot, const Type &t);
  void emitDrop(llvm::Value *slot, llvm::Value *flag, const Type &t); // flag 立ちで drop
  void popDropScope();        // 現スコープの全ローカルを (逆順で) drop して pop
  void dropAllScopesForExit(); // return 用: 全スコープを drop (pop はしない)
  // enum バリアント構築 (具体化された enum 型 + tag + ペイロード)
  llvm::Value *genVariant(const Type &enumType, int tag,
                          llvm::ArrayRef<llvm::Value *> payload);

  // 具体化された struct 型 (型引数つき) → LLVM 構造体型 (単態化)
  llvm::StructType *getStructType(const Type &structType);
  // 具体化された enum 型 (型引数つき) → タグ付き共用体 (単態化)
  llvm::StructType *getEnumType(const Type &enumType);
  llvm::Function *declareProto(const Prototype &p);
  bool genFunction(const FunctionDef &f);
  // 関数本体を fn に生成する。subst はジェネリック単態化の型置換 (非総称は空)。
  bool genFunctionInto(const FunctionDef &f, llvm::Function *fn,
                       const std::map<std::string, Type> &subst,
                       const Type &retType);
  // ジェネリック関数の具体化を宣言し (未処理なら) 生成キューへ積む。
  llvm::Function *ensureInstance(const FunctionDef *def,
                                 const std::vector<Type> &typeArgs);
  void emitRuntimeDefs(); // printi/printd/putchard の本体 (libc 呼び出し)
  void emitCMain();       // i32 main() { __main(); return 0; }

  llvm::LLVMContext &ctx_;
  DiagnosticEngine &diag_;
  llvm::DataLayout dl_;
  std::unique_ptr<llvm::Module> module_;
  llvm::IRBuilder<> builder_;
  std::map<std::string, llvm::Value *> namedValues_;
  std::map<std::string, const StructDef *> structDefs_;
  std::map<std::string, llvm::StructType *> structTypes_;
  std::map<std::string, const EnumDef *> enumDefs_;
  std::map<std::string, llvm::StructType *> enumTypes_;

  // メソッド: 型名 → メソッド名 → 定義 (ジェネリック関数として単態化する)
  std::map<std::string, std::map<std::string, const FunctionDef *>> methodDefs_;

  // ジェネリック関数とその単態化
  std::map<std::string, const FunctionDef *> genericFuncDefs_;
  std::map<std::string, Type> typeSubst_; // 現在生成中インスタンスの型置換
  struct PendingInstance {
    const FunctionDef *def;
    std::map<std::string, Type> subst;
    std::string mangled;
  };
  std::vector<PendingInstance> pendingInstances_; // 本体生成待ちの具体化

  // 早期リターン (return / ?) 用。生成中の関数ごとに設定。
  llvm::Value *currentRetSlot_ = nullptr;       // 戻り値の置き場 (unit なら null)
  llvm::BasicBlock *currentRetBlock_ = nullptr; // 終端ブロック
  Type currentRetTypeCG_;                        // 戻り値型 (置換済み)

  // Drop: 型ごとのドロップグルー関数と、スコープごとのドロップ対象ローカル。
  struct DropEntry {
    llvm::Value *slot;
    llvm::Value *flag;
    Type type;
  };
  std::map<std::string, llvm::Function *> dropFns_; // マングル名 → drop_T
  std::vector<std::pair<Type, llvm::Function *>> pendingDropFns_; // 本体生成待ち
  std::vector<std::vector<DropEntry>> dropScopes_;                // スコープ stack
};

} // namespace kal
