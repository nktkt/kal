# Kal — LLVM で作る自作の式言語

[![CI](https://github.com/nktkt/kal/actions/workflows/ci.yml/badge.svg)](https://github.com/nktkt/kal/actions/workflows/ci.yml)

Kal は LLVM をバックエンドにした「電卓・式言語」(Kaleidoscope 系) の実装です。
ソースを **LLVM IR** にコンパイルし、**ORC JIT** でその場実行します。

```
ソース → [Lexer] → トークン → [Parser] → AST → [Sema] → 型付きAST → [CodeGen] → LLVM IR → [ORC JIT] → 実行
```

**静的型付き**（`i8`〜`i64`・`u8`〜`u64`・`f32`・`f64`・`bool`）で、型検査器が
リテラルの型を文脈から推論し、正確なエラーを出します。暗黙の数値変換はありません（`as` を使用）。

コンパイラは `src/` と `include/kal/` に小さな単位で分割されています（ソース管理・
span 付き診断・字句解析・構文解析・codegen を持たない AST・型検査・コード生成）。各段が
独立しており、長期計画（[ROADMAP.md](ROADMAP.md)）の土台になっています。

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

## 実行・コンパイル

`kalc` は JIT 実行も、単体ネイティブバイナリへの AOT コンパイルもできます:

```sh
./build/kalc run examples/fib.kal        # JIT コンパイルして実行 (既定)
./build/kalc examples/fib.kal            # 同上 (run が既定コマンド)
echo '1 + 2 * 3;' | ./build/kalc         # 標準入力から → 7

./build/kalc build examples/fib.kal -o fib   # ネイティブ実行ファイルを生成
./fib                                        # 単体で動く → 55 610 6765

./build/kalc -O2 build examples/fib.kal -o fib   # 最適化つき (-O0〜-O3)
./build/kalc emit-ir  examples/fib.kal       # 生成された LLVM IR を表示
./build/kalc emit-obj examples/fib.kal -o fib.o   # オブジェクトファイル出力
```

AOT バイナリは自己完結します（libc/libm のみリンク）。`kalc` がランタイム
（`printi`/`printd`/`putchard`）と C の `main` をモジュールに生成し、LLVM の
`PassBuilder` で最適化、ホストの `TargetMachine` でオブジェクト出力、`cc` でリンクします。

トップレベルに書いた式は型に応じて**自動表示**されます（整数・浮動小数点・bool）。
`()`（unit）型の式――ループや出力組み込み――は何も表示しません。

---

## 言語仕様

### 型
`i8 i16 i32 i64`・`u8 u16 u32 u64`・`f32 f64`・`bool`・`()`（unit）に加え、
ユーザー定義の **`struct`**・**`enum`**（代数的データ型）・タプル `(T, U, …)`。
整数リテラルの既定は **i32**、小数リテラルは **f64** ですが、文脈から型が決まります
（例: `n: i64` のとき `n < 2` の `2` は `i64`）。**暗黙変換はなく**、`as` で明示変換します。

### 演算子（優先順位の高い順）

| 演算子 | 意味 | 優先度 |
|---|---|---|
| `as` | 型キャスト | （後置） |
| `*` `/` | 乗除 | 40 |
| `+` `-` | 加減 | 20 |
| `<` `>` | 比較（結果は bool） | 10 |

### 関数定義 / 外部宣言

```
fn add(a: i64, b: i64) -> i64 = a + b;   # 型付き引数・`-> 型` の戻り値・`= 式` の本体
fn fib(n: i64) -> i64 =
  if n < 2 then n
  else fib(n - 1) + fib(n - 2);

extern sin(x: f64) -> f64;               # 外部関数 (libm 等) を宣言して呼べる
sin(0.0);                                # => 0
```

`-> 型` を省略すると戻り値型は `()`（unit）になります。

### キャスト

```
9 as f64;        # i32 → f64
3.9 as i64;      # f64 → i64（切り捨て）→ 3
```

### if / then / else（式）

```
if cond then expr1 else expr2    # cond は bool 型。両分岐は同じ型である必要がある
```

### for ループ（前判定・値は unit）

```
for i = start, cond, step in body   # cond(bool) が成り立つ間、各反復後に i += step
for i = 1, i < 6, 1 in printi(i as i64);   # 1 2 3 4 5（step 省略時は 1）
```

### 構造体・タプル・let

```
struct Point { x: f64, y: f64 }     # 公称の積型

fn norm2(p: Point) -> f64 = p.x * p.x + p.y * p.y;   # `.field` アクセス

let p = Point { x: 3.0, y: 4.0 } in norm2(p);   # => 25   (let ... in 式)

let pair = (6, 7) in pair.0 * pair.1;           # => 42   (タプル + .0/.1)
```

構造体・タプルは値型（値渡し・値返し）。`let name = e in body` は `body` の中だけで
有効な不変ローカル束縛です。

### enum とパターンマッチ

```
enum Shape {            # Rust 流の代数的データ型 (タグ付き共用体)
  Circle(f64),
  Rect(f64, f64),
}

fn area(s: Shape) -> f64 =
  match s {             # match は網羅性がチェックされる
    Circle(r)  => 3.14 * r * r,
    Rect(w, h) => w * h,
  };

area(Circle(2.0));      # => 12.56   (バリアントは名前で構築)
```

match のアームは `Variant(束縛) => 式` または `_`（ワイルドカード）。全バリアントを
網羅する（または `_` を含む）必要があります。ペイロードはパターンの名前に束縛されます
（`_` は無視）。

### 組み込み関数
- `printi(x: i64)` … 整数を 1 行で表示
- `printd(x: f64)` … 浮動小数点を 1 行で表示
- `putchard(x: i64)` … 文字コード x の 1 文字を出力（例: `putchard(10)` で改行）

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
│   ├── Type.h               #   型システム
│   ├── AST.h    Parser.h    #   codegen を持たない AST + 構文解析
│   ├── Sema.h               #   型検査 (AST に型を注釈)
│   └── CodeGen.h            #   型付き AST → LLVM IR
├── src/                     # 実装 + main.cpp（JIT ドライバ）
├── examples/                # arith, fib, loop, extern, cast, struct, enum
├── tests/                   # ゴールデンテスト（run_tests.sh）
└── .github/workflows/ci.yml # Linux / macOS でビルド & テスト
```

パイプラインは段階的: `Lexer → Parser(AST) → Sema(型付きAST) → CodeGen(LLVM IR) → ORC JIT`。
AST は codegen ロジックを持たず、`Sema` が型を注釈し `CodeGen` が別に走査するため、各段が
独立して拡張できます（本格的な HIR・MIR・借用チェッカへ。[ROADMAP.md](ROADMAP.md) 参照）。

診断はソースの該当箇所を正確に指します:

```
error[E0121]: 二項演算の両辺の型が一致しません (i32 と f64)
 --> prog.kal:1:1
  |
1 | 1 + 2.0;
  | ^^^^^^^
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
