//===- SourceManager.cpp ---------------------------------------------------===//
#include "kal/SourceManager.h"

#include <algorithm>

using namespace kal;

uint32_t SourceManager::addFile(std::string name, std::string buffer) {
  File f;
  f.name = std::move(name);
  f.buffer = std::move(buffer);
  f.lineStarts.push_back(0);
  for (uint32_t i = 0; i < f.buffer.size(); ++i)
    if (f.buffer[i] == '\n')
      f.lineStarts.push_back(i + 1);
  files_.push_back(std::move(f));
  return static_cast<uint32_t>(files_.size() - 1);
}

llvm::StringRef SourceManager::name(uint32_t fileId) const {
  return files_[fileId].name;
}

llvm::StringRef SourceManager::buffer(uint32_t fileId) const {
  return files_[fileId].buffer;
}

std::pair<unsigned, unsigned> SourceManager::lineCol(uint32_t fileId,
                                                     uint32_t offset) const {
  const auto &ls = files_[fileId].lineStarts;
  // offset を含む行 = (offset より大きい最初の行頭) の 1 つ前
  auto it = std::upper_bound(ls.begin(), ls.end(), offset);
  unsigned line = static_cast<unsigned>(it - ls.begin()); // 1 始まり
  if (line == 0)
    line = 1;
  unsigned col = offset - ls[line - 1] + 1;
  return {line, col};
}

llvm::StringRef SourceManager::lineText(uint32_t fileId, unsigned line) const {
  const auto &f = files_[fileId];
  if (line == 0 || line > f.lineStarts.size())
    return {};
  uint32_t start = f.lineStarts[line - 1];
  uint32_t end = (line < f.lineStarts.size())
                     ? f.lineStarts[line]
                     : static_cast<uint32_t>(f.buffer.size());
  llvm::StringRef s = llvm::StringRef(f.buffer).substr(start, end - start);
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
    s = s.drop_back();
  return s;
}
