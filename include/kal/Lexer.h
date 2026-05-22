//===- Lexer.h - turns source bytes into tokens ---------------------------===//
#pragma once

#include "kal/Token.h"
#include "llvm/ADT/StringRef.h"
#include <cstdint>

namespace kal {

class SourceManager;
class DiagnosticEngine;

class Lexer {
public:
  Lexer(const SourceManager &sm, DiagnosticEngine &diag, uint32_t fileId);

  /// 次のトークンを 1 つ取り出す。
  Token next();

private:
  const SourceManager &sm_;
  DiagnosticEngine &diag_;
  uint32_t fileId_;
  llvm::StringRef buf_;
  uint32_t pos_ = 0;
};

} // namespace kal
