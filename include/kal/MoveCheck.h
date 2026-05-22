//===- MoveCheck.h - move semantics & use-after-move ----------------------===//
#pragma once

#include "kal/AST.h"
#include "kal/Type.h"
#include <map>
#include <string>

namespace kal {

class DiagnosticEngine;

/// ムーブ意味論の検査 (Sema の後・型注釈済み AST を走査)。
/// Copy 型 (数値/bool/unit/&T) はコピー、それ以外 (struct/enum/タプル/&mut) は
/// 値として使うとムーブされ、ムーブ後の使用はエラー。
/// 直線コードは正確、if/match は分岐ごとに解析して合流、ループ本体での
/// 外側変数のムーブは「次反復で再利用」としてエラーにする (健全側に倒す)。
class MoveCheck {
public:
  explicit MoveCheck(DiagnosticEngine &diag);
  bool run(const Program &program); // エラーがなければ true

private:
  bool isCopy(const Type &t) const;
  void moveVar(const std::string &name, Span span);

  void use(const Expr *e);         // 値として消費する位置
  void requireLive(const Expr *e); // 場所を読む/借用する (ムーブしない)

  DiagnosticEngine &diag_;
  std::map<std::string, Span> moved_; // ムーブ済み変数名 → ムーブ箇所
};

} // namespace kal
