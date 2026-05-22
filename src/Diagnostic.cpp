//===- Diagnostic.cpp ------------------------------------------------------===//
#include "kal/Diagnostic.h"

#include "llvm/Support/Process.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <string>

using namespace kal;

DiagnosticEngine::DiagnosticEngine(const SourceManager &sm) : sm_(sm) {
  // stderr が端末で、かつ NO_COLOR 未設定のときだけ色付け。
  color_ = llvm::sys::Process::StandardErrIsDisplayed() &&
           std::getenv("NO_COLOR") == nullptr;
}

void DiagnosticEngine::report(DiagLevel level, Span span, llvm::StringRef code,
                              llvm::StringRef message) {
  if (level == DiagLevel::Error)
    ++errorCount_;

  llvm::raw_ostream &os = llvm::errs();
  const char *levelStr = level == DiagLevel::Error     ? "error"
                         : level == DiagLevel::Warning ? "warning"
                                                       : "note";
  const char *colCode = "";
  const char *bold = "";
  const char *reset = "";
  if (color_) {
    colCode = level == DiagLevel::Error     ? "\033[1;31m"
              : level == DiagLevel::Warning ? "\033[1;33m"
                                            : "\033[1;36m";
    bold = "\033[1m";
    reset = "\033[0m";
  }

  auto [line, col] = sm_.lineCol(span.fileId, span.start);

  // ヘッダ:  error[E0001]: メッセージ
  os << colCode << levelStr;
  if (!code.empty())
    os << "[" << code << "]";
  os << reset << bold << ": " << message << reset << "\n";

  // 位置:    --> name:line:col
  os << " --> " << sm_.name(span.fileId) << ":" << line << ":" << col << "\n";

  // ソース行とキャレット
  llvm::StringRef text = sm_.lineText(span.fileId, line);
  std::string num = std::to_string(line);
  std::string gutter(num.size(), ' ');

  os << gutter << " |\n";
  os << num << " | " << text << "\n";
  os << gutter << " | ";
  for (unsigned i = 1; i < col; ++i)
    os << ' ';

  unsigned len = span.end > span.start ? span.end - span.start : 1;
  unsigned remain = text.size() >= (col - 1) ? text.size() - (col - 1) : 0;
  if (remain > 0 && len > remain)
    len = remain;

  os << colCode;
  for (unsigned i = 0; i < len; ++i)
    os << '^';
  os << reset << "\n";
}
