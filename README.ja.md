# Kal — LLVM で作る自作の式言語

[![CI](https://github.com/nktkt/kal/actions/workflows/ci.yml/badge.svg)](https://github.com/nktkt/kal/actions/workflows/ci.yml)

Kal は LLVM をバックエンドにした「電卓・式言語」(Kaleidoscope 系) の実装です。
ソースを **LLVM IR** にコンパイルし、**ORC JIT** でその場実行します。

```
ソース → [Lexer] → トークン → [Parser] → AST → [CodeGen] → LLVM IR → [ORC JIT] → 実行
```

コンパイラは `src/` と `include/kal/` に小さな単位で分割されています（ソース管理・
span 付き診断・字句解析・構文解析・codegen を持たない AST・コード生成）。各段が独立
しており、長期計画（[ROADMAP.md](ROADMAP.md)）の土台になっています。

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
├── CMakeLists.txt           # find_package(LLVM) でリンク設定
├── include/kal/             # 公開ヘッダ
│   ├── SourceManager.h      #   ソース・span・行/列
│   ├── Diagnostic.h         #   span 付き rustc 風診断
│   ├── Token.h  Lexer.h     #   トークン + 字句解析
│   ├── AST.h    Parser.h    #   codegen を持たない AST + 構文解析
│   └── CodeGen.h            #   AST → LLVM IR
├── src/                     # 実装 + main.cpp（JIT ドライバ）
├── examples/                # arith, fib, loop, extern
├── tests/                   # ゴールデンテスト（run_tests.sh）
└── .github/workflows/ci.yml # Linux / macOS でビルド & テスト
```

パイプラインは段階的: `Lexer → Parser(AST) → CodeGen(LLVM IR) → ORC JIT`。
AST は codegen ロジックを持たず、`CodeGen` が別に走査するため、各段が独立して
拡張できます（型付き HIR・MIR・借用チェッカへ。[ROADMAP.md](ROADMAP.md) 参照）。

診断はソースの該当箇所を正確に指します:

```
error[E0100]: 未定義の変数です
 --> prog.kal:2:9
  |
2 | def f() x;
  |         ^
```

## テスト

```sh
cmake --build build -j            # 先に kalc をビルド
bash tests/run_tests.sh           # ゴールデンテスト実行
bash tests/run_tests.sh --bless   # 意図した変更後に期待出力を再生成
```

例題は `tests/expected/*.out`、わざと不正な `tests/diagnostics/` のプログラムは
期待 `*.stderr` と比較されます。CI が push / PR ごとに Linux・macOS で実行します。

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
