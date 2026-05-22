//===- main.cpp - the kalc driver -----------------------------------------===//
//
// パイプライン: ソース → Lexer → Parser(AST) → CodeGen(LLVM IR) → ORC JIT
//
//===----------------------------------------------------------------------===//

#include "kal/CodeGen.h"
#include "kal/Diagnostic.h"
#include "kal/Lexer.h"
#include "kal/Parser.h"
#include "kal/SourceManager.h"

#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdio>
#include <memory>
#include <string>

using namespace kal;
using namespace llvm;

//===----------------------------------------------------------------------===//
// JIT から呼べる組み込み関数
//===----------------------------------------------------------------------===//

extern "C" double printd(double x) {
  std::printf("%g\n", x);
  return 0;
}

extern "C" double putchard(double x) {
  std::putchar(static_cast<int>(x));
  return 0;
}

//===----------------------------------------------------------------------===//

static std::string readAll(FILE *f) {
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
    out.append(buf, n);
  return out;
}

int main(int argc, char **argv) {
  bool emitIR = false;
  const char *path = nullptr;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--emit-ir" || a == "-S")
      emitIR = true;
    else if (a == "--help" || a == "-h") {
      std::printf("使い方: kalc [--emit-ir] [file.kal]\n"
                  "  ファイル省略時は標準入力から読み込みます。\n");
      return 0;
    } else
      path = argv[i];
  }

  // ソースを読み込む
  std::string src;
  std::string name;
  if (path) {
    FILE *f = std::fopen(path, "rb");
    if (!f) {
      errs() << "ファイルを開けません: " << path << "\n";
      return 1;
    }
    src = readAll(f);
    std::fclose(f);
    name = path;
  } else {
    src = readAll(stdin);
    name = "<stdin>";
  }

  // 字句解析 + 構文解析
  SourceManager sm;
  uint32_t fileId = sm.addFile(name, std::move(src));
  DiagnosticEngine diag(sm);
  Lexer lexer(sm, diag, fileId);
  Parser parser(lexer, diag);
  Program prog = parser.parseProgram();
  if (diag.numErrors() > 0) {
    errs() << diag.numErrors() << " 個のエラーで中断しました\n";
    return 1;
  }

  // LLVM 初期化
  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();
  InitializeNativeTargetAsmParser();

  // コード生成
  auto ctx = std::make_unique<LLVMContext>();
  CodeGen cg(*ctx, diag);
  auto module = cg.run(prog);
  if (!module) {
    errs() << diag.numErrors() << " 個のエラーで中断しました\n";
    return 1;
  }

  // --emit-ir: 実行せず IR を表示
  if (emitIR) {
    module->print(outs(), nullptr);
    return 0;
  }

  // ORC JIT で実行
  auto jitOrErr = orc::LLJITBuilder().create();
  if (!jitOrErr) {
    errs() << "JIT 生成に失敗: " << toString(jitOrErr.takeError()) << "\n";
    return 1;
  }
  auto jit = std::move(*jitOrErr);
  module->setDataLayout(jit->getDataLayout());

  // 組み込み関数を JIT のシンボルとして登録
  orc::SymbolMap syms;
  syms[jit->mangleAndIntern("printd")] = {orc::ExecutorAddr::fromPtr(&printd),
                                          JITSymbolFlags::Exported};
  syms[jit->mangleAndIntern("putchard")] = {
      orc::ExecutorAddr::fromPtr(&putchard), JITSymbolFlags::Exported};
  if (auto err = jit->getMainJITDylib().define(orc::absoluteSymbols(syms))) {
    errs() << "シンボル登録に失敗: " << toString(std::move(err)) << "\n";
    return 1;
  }

  // 標準ライブラリ (sin, cos など) も解決可能にする
  if (auto gen = orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
          jit->getDataLayout().getGlobalPrefix()))
    jit->getMainJITDylib().addGenerator(std::move(*gen));

  if (auto err = jit->addIRModule(
          orc::ThreadSafeModule(std::move(module), std::move(ctx)))) {
    errs() << "モジュール追加に失敗: " << toString(std::move(err)) << "\n";
    return 1;
  }

  auto sym = jit->lookup("__main");
  if (!sym) {
    errs() << "__main が見つかりません: " << toString(sym.takeError()) << "\n";
    return 1;
  }
  auto *mainPtr = sym->toPtr<void (*)()>();
  mainPtr();
  return 0;
}
