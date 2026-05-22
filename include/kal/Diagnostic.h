//===- Diagnostic.h - span-aware error reporting --------------------------===//
#pragma once

#include "kal/SourceManager.h"
#include "llvm/ADT/StringRef.h"

namespace kal {

enum class DiagLevel { Error, Warning, Note };

/// 診断 (エラー/警告) を rustc 風の整形で stderr に出力するエンジン。
class DiagnosticEngine {
public:
  explicit DiagnosticEngine(const SourceManager &sm);

  void report(DiagLevel level, Span span, llvm::StringRef code,
              llvm::StringRef message);

  void error(Span span, llvm::StringRef code, llvm::StringRef message) {
    report(DiagLevel::Error, span, code, message);
  }
  void warning(Span span, llvm::StringRef code, llvm::StringRef message) {
    report(DiagLevel::Warning, span, code, message);
  }

  unsigned numErrors() const { return errorCount_; }

private:
  const SourceManager &sm_;
  unsigned errorCount_ = 0;
  bool color_;
};

} // namespace kal
