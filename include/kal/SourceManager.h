//===- SourceManager.h - source files, byte spans, line/column ------------===//
#pragma once

#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace kal {

/// ソース上の範囲を表す (バイトオフセット, 終端は排他的)。
struct Span {
  uint32_t fileId = 0;
  uint32_t start = 0; // 開始バイト位置 (含む)
  uint32_t end = 0;   // 終了バイト位置 (含まない)
};

/// 複数ソースファイルを保持し、オフセット↔行・列の変換を提供する。
class SourceManager {
public:
  /// ファイルを登録して fileId を返す。
  uint32_t addFile(std::string name, std::string buffer);

  llvm::StringRef name(uint32_t fileId) const;
  llvm::StringRef buffer(uint32_t fileId) const;

  /// バイトオフセットに対する 1 始まりの (行, 列)。
  std::pair<unsigned, unsigned> lineCol(uint32_t fileId, uint32_t offset) const;

  /// 1 始まりの行のテキスト (改行を除く)。
  llvm::StringRef lineText(uint32_t fileId, unsigned line) const;

private:
  struct File {
    std::string name;
    std::string buffer;
    std::vector<uint32_t> lineStarts; // 各行の開始オフセット
  };
  std::vector<File> files_;
};

} // namespace kal
