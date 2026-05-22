//===- main.cpp - the kalc driver -----------------------------------------===//
//
// パイプライン: ソース → Lexer → Parser → Sema → CodeGen → (最適化) →
//                 run   : ORC JIT で実行
//                 build : オブジェクト出力 → cc でリンク → 単体バイナリ
//                 emit-obj / emit-ir : 中間生成物の出力
//
//===----------------------------------------------------------------------===//

#include "kal/CodeGen.h"
#include "kal/Diagnostic.h"
#include "kal/Lexer.h"
#include "kal/Parser.h"
#include "kal/Sema.h"
#include "kal/SourceManager.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>

using namespace kal;
using namespace llvm;

//===----------------------------------------------------------------------===//
// JIT から呼べる組み込み関数 (AOT ではモジュール側に本体を生成する)
//===----------------------------------------------------------------------===//

extern "C" void printi(int64_t x) {
  std::printf("%lld\n", static_cast<long long>(x));
}
extern "C" void printd(double x) { std::printf("%g\n", x); }
extern "C" void putchard(int64_t x) { std::putchar(static_cast<int>(x)); }

//===----------------------------------------------------------------------===//

namespace {

std::string readAll(FILE *f) {
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
    out.append(buf, n);
  return out;
}

/// 中間表現を最適化する (O1..O3)。
void optimizeModule(Module &m, OptimizationLevel level) {
  LoopAnalysisManager lam;
  FunctionAnalysisManager fam;
  CGSCCAnalysisManager cgam;
  ModuleAnalysisManager mam;
  PassBuilder pb;
  pb.registerModuleAnalyses(mam);
  pb.registerCGSCCAnalyses(cgam);
  pb.registerFunctionAnalyses(fam);
  pb.registerLoopAnalyses(lam);
  pb.crossRegisterProxies(lam, fam, cgam, mam);
  ModulePassManager mpm = pb.buildPerModuleDefaultPipeline(level);
  mpm.run(m, mam);
}

/// ホスト用 TargetMachine を作る。
std::unique_ptr<TargetMachine> createHostTargetMachine() {
  Triple triple(sys::getDefaultTargetTriple());
  std::string err;
  const Target *target = TargetRegistry::lookupTarget(triple, err);
  if (!target) {
    errs() << "ターゲットが見つかりません: " << err << "\n";
    return nullptr;
  }
  TargetOptions opt;
  return std::unique_ptr<TargetMachine>(target->createTargetMachine(
      triple, "generic", "", opt, std::optional<Reloc::Model>(Reloc::PIC_)));
}

/// モジュールをオブジェクトファイルに書き出す。
bool emitObjectFile(Module &m, TargetMachine &tm, StringRef path) {
  std::error_code ec;
  raw_fd_ostream dest(path, ec, sys::fs::OF_None);
  if (ec) {
    errs() << "オブジェクトファイルを開けません: " << ec.message() << "\n";
    return false;
  }
  legacy::PassManager pass;
  if (tm.addPassesToEmitFile(pass, dest, nullptr, CodeGenFileType::ObjectFile)) {
    errs() << "このターゲットはオブジェクト出力に対応していません\n";
    return false;
  }
  pass.run(m);
  dest.flush();
  return true;
}

/// オブジェクトファイルを cc でリンクして実行ファイルを作る。
bool linkExecutable(StringRef objPath, StringRef outPath) {
  ErrorOr<std::string> cc = sys::findProgramByName("cc");
  if (!cc)
    cc = sys::findProgramByName("clang");
  if (!cc) {
    errs() << "リンカ (cc / clang) が見つかりません\n";
    return false;
  }
  SmallVector<StringRef, 8> args = {*cc, objPath, "-o", outPath, "-lm"};
  std::string errMsg;
  int rc = sys::ExecuteAndWait(*cc, args, std::nullopt, {}, 0, 0, &errMsg);
  if (rc != 0) {
    errs() << "リンクに失敗しました: " << errMsg << " (code " << rc << ")\n";
    return false;
  }
  return true;
}

void printHelp() {
  std::printf(
      "使い方: kalc [command] [options] [file.kal]\n"
      "\n"
      "command:\n"
      "  run        JIT コンパイルして実行 (省略時の既定)\n"
      "  build      ネイティブ実行ファイルを生成 (-o で出力名、既定 a.out)\n"
      "  emit-obj   オブジェクトファイルを出力 (-o で出力名、既定 out.o)\n"
      "  emit-ir    LLVM IR を表示 (--emit-ir / -S も可)\n"
      "\n"
      "options:\n"
      "  -O0 -O1 -O2 -O3   最適化レベル (既定 -O0)\n"
      "  -o NAME           出力ファイル名\n"
      "  -h, --help        このヘルプ\n"
      "\n"
      "file 省略時は標準入力から読み込みます。\n");
}

enum class Cmd { Run, Build, EmitObj, EmitIR };

} // namespace

int main(int argc, char **argv) {
  Cmd cmd = Cmd::Run;
  unsigned optLevel = 0;
  std::string outPath;
  const char *path = nullptr;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "run")
      cmd = Cmd::Run;
    else if (a == "build")
      cmd = Cmd::Build;
    else if (a == "emit-obj")
      cmd = Cmd::EmitObj;
    else if (a == "emit-ir" || a == "--emit-ir" || a == "-S")
      cmd = Cmd::EmitIR;
    else if (a == "-O0")
      optLevel = 0;
    else if (a == "-O1")
      optLevel = 1;
    else if (a == "-O2")
      optLevel = 2;
    else if (a == "-O3")
      optLevel = 3;
    else if (a == "-o") {
      if (i + 1 < argc)
        outPath = argv[++i];
    } else if (a == "-h" || a == "--help") {
      printHelp();
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

  // 型検査
  Sema sema(diag);
  if (!sema.run(prog)) {
    errs() << diag.numErrors() << " 個のエラーで中断しました\n";
    return 1;
  }

  // LLVM 初期化
  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();
  InitializeNativeTargetAsmParser();

  bool aot = (cmd == Cmd::Build || cmd == Cmd::EmitObj);

  // コード生成
  auto ctx = std::make_unique<LLVMContext>();
  CodeGen cg(*ctx, diag);
  auto module = cg.run(prog, /*emitRuntime=*/aot);
  if (!module) {
    errs() << diag.numErrors() << " 個のエラーで中断しました\n";
    return 1;
  }

  // AOT はターゲット情報を先に設定 (最適化が data layout を使う)
  std::unique_ptr<TargetMachine> tm;
  if (aot) {
    tm = createHostTargetMachine();
    if (!tm)
      return 1;
    module->setDataLayout(tm->createDataLayout());
    module->setTargetTriple(Triple(sys::getDefaultTargetTriple()));
  }

  // 最適化
  if (optLevel > 0) {
    OptimizationLevel lvl = optLevel == 1   ? OptimizationLevel::O1
                            : optLevel == 2 ? OptimizationLevel::O2
                                            : OptimizationLevel::O3;
    optimizeModule(*module, lvl);
  }

  // --- emit-ir ---
  if (cmd == Cmd::EmitIR) {
    module->print(outs(), nullptr);
    return 0;
  }

  // --- emit-obj ---
  if (cmd == Cmd::EmitObj) {
    std::string out = outPath.empty() ? "out.o" : outPath;
    return emitObjectFile(*module, *tm, out) ? 0 : 1;
  }

  // --- build (オブジェクト出力 → リンク) ---
  if (cmd == Cmd::Build) {
    std::string out = outPath.empty() ? "a.out" : outPath;
    SmallString<128> objTmp;
    if (auto ec = sys::fs::createTemporaryFile("kal", "o", objTmp)) {
      errs() << "一時ファイルを作成できません: " << ec.message() << "\n";
      return 1;
    }
    bool ok = emitObjectFile(*module, *tm, objTmp) &&
              linkExecutable(objTmp, out);
    sys::fs::remove(objTmp);
    return ok ? 0 : 1;
  }

  // --- run (ORC JIT) ---
  auto jitOrErr = orc::LLJITBuilder().create();
  if (!jitOrErr) {
    errs() << "JIT 生成に失敗: " << toString(jitOrErr.takeError()) << "\n";
    return 1;
  }
  auto jit = std::move(*jitOrErr);
  module->setDataLayout(jit->getDataLayout());

  orc::SymbolMap syms;
  syms[jit->mangleAndIntern("printi")] = {orc::ExecutorAddr::fromPtr(&printi),
                                          JITSymbolFlags::Exported};
  syms[jit->mangleAndIntern("printd")] = {orc::ExecutorAddr::fromPtr(&printd),
                                          JITSymbolFlags::Exported};
  syms[jit->mangleAndIntern("putchard")] = {
      orc::ExecutorAddr::fromPtr(&putchard), JITSymbolFlags::Exported};
  if (auto err = jit->getMainJITDylib().define(orc::absoluteSymbols(syms))) {
    errs() << "シンボル登録に失敗: " << toString(std::move(err)) << "\n";
    return 1;
  }

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
  sym->toPtr<void (*)()>()();
  return 0;
}
