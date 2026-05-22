# Kal — LLVM で作る自作の式言語 v0.1

Kal は LLVM をバックエンドにした「電卓・式言語」(Kaleidoscope 系) の最小実装です。
ソースを **LLVM IR** にコンパイルし、**ORC JIT** でその場実行します。

```
ソース → [Lexer] → トークン → [Parser] → AST → [CodeGen] → LLVM IR → [ORC JIT] → 実行
```

実装はすべて `src/kal.cpp` の 1 ファイル（約 700 行、教材として読めるようコメント多め）。

---

## ビルド

必要なもの: LLVM 22 (Homebrew), CMake 3.20+, C++17 コンパイラ

```sh
cd kal
cmake -S . -B build -DLLVM_DIR="$(llvm-config --cmakedir)" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

`build/kalc` が生成されます。

> 注: エディタが LLVM ヘッダ未検出の赤線を出すことがありますが、CMake 経由のビルド
> （`-I$(llvm-config --includedir)` 付き）は通ります。

## 実行

```sh
./build/kalc examples/fib.kal     # ファイルを実行
./build/kalc < examples/fib.kal   # 標準入力から
echo '1 + 2 * 3;' | ./build/kalc  # ワンライナー → 7
./build/kalc --emit-ir examples/fib.kal   # 実行せず LLVM IR を表示
```

トップレベルに書いた式は値が**自動表示**されます（REPL/電卓の挙動）。
すべてが式＝値を返すので、ループや `putchard` など `0` を返す式は `0` も表示されます。

---

## 言語仕様

### 値
すべて **倍精度浮動小数点 (double)**。真偽は `1.0`(真)/`0.0`(偽)。

### 演算子（優先順位の高い順）

| 演算子 | 意味 | 優先度 |
|---|---|---|
| `*` `/` | 乗除 | 40 |
| `+` `-` | 加減 | 20 |
| `<` `>` | 比較（結果は 1.0/0.0） | 10 |

### 関数定義 / 外部宣言

```
def add(a b) a + b;          # 引数はスペース区切り、本体は 1 つの式
def fib(n)
  if n < 2 then n
  else fib(n-1) + fib(n-2);

extern sin(x);               # 標準ライブラリ等の外部関数を宣言して呼べる
sin(0);                      # => 0
```

### if / then / else（式）

```
if cond then expr1 else expr2    # cond が 0 以外なら expr1、そうでなければ expr2
```

### for ループ（前判定）

```
for i = start, cond, step in body
# cond が成り立つ間 body を実行し、各反復後に i += step（step 省略時は 1）
# 値は常に 0 を返す
for i = 1, i < 6, 1 in printd(i*i);   # 1 4 9 16 25
```

### 組み込み関数
- `printd(x)` … x を 1 行で表示
- `putchard(x)` … 文字コード x の 1 文字を出力（例: `putchard(10)` で改行）

### コメント
`#` から行末まで。

---

## ファイル構成

```
kal/
├── CMakeLists.txt      # find_package(LLVM) でリンク設定
├── src/kal.cpp         # Lexer / Parser / CodeGen / JIT ドライバ
└── examples/
    ├── arith.kal       # 四則演算と優先順位
    ├── fib.kal         # 再帰（フィボナッチ）
    ├── loop.kal        # for ループと putchard
    └── extern.kal      # libm の sin/cos/sqrt 呼び出し
```

`src/kal.cpp` の構成（コメントの章番号と対応）:
1. **Lexer** — 文字列をトークンへ
2. **AST** — 構文木ノード定義
3. **Parser** — 再帰下降 + 演算子優先順位
4. **CodeGen** — 各 AST ノードの `codegen()` が LLVM IR を生成
5. **ランタイム** — `printd` / `putchard`（C++ 側の `extern "C"`）
6. **ドライバ** — パース → 全プロトタイプ宣言 → 本体生成 → `__main` 生成 → JIT 実行

---

## ロードマップ

Kal v0.1 は今は JIT 電卓ですが、目標はもっと大きく――**所有権ベースのメモリ管理
（GCなし）を持つ、AOT コンパイルの汎用システム言語**（Rust / Zig 系、LLVM ベース）
を目指します。

コンパイラの再設計・型システム・借用チェッカ・ジェネリクス・標準ライブラリ・
ツールチェーン（LSP / パッケージマネージャ）・v1.0 までの全工程は
**[ROADMAP.md](ROADMAP.md)**（英語）にまとめています。

今すぐ着手するなら: パースエラーを診断モジュール経由にする、ソース span を付与する、
`src/kal.cpp` を lexer/parser/ast/codegen に分割する（Phase 0）あたりから。
