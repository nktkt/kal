# Kal — LLVM で作る自作の式言語

[![CI](https://github.com/nktkt/kal/actions/workflows/ci.yml/badge.svg)](https://github.com/nktkt/kal/actions/workflows/ci.yml)

Kal は LLVM をバックエンドにした「電卓・式言語」(Kaleidoscope 系) の実装です。
ソースを **LLVM IR** にコンパイルし、**ORC JIT** でその場実行します。

```
ソース → [Lexer] → [Parser] → AST → [Sema] → 型付きAST → [MoveCheck] → [CodeGen] → LLVM IR → [ORC JIT] / ネイティブバイナリ
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
ユーザー定義の **`struct`**・**`enum`**（代数的データ型。どちらも**ジェネリック**可――
`Name<T, …>`、`Option<T>` / `Result<T, E>` は組み込み）・タプル `(T, U, …)`・
**配列** `[T; N]`・**スライス** `&[T]` / `&mut [T]`・**参照** `&T` / `&mut T`・
ヒープ確保の **`Box<T>`**。
整数リテラルの既定は **i32**、小数リテラルは **f64** ですが、文脈から型が決まります
（例: `n: i64` のとき `n < 2` の `2` は `i64`）。真偽値リテラルは `true` / `false`。
**暗黙変換はなく**、`as` で明示変換します。

### 演算子（優先順位の高い順）

| 演算子 | 意味 | 優先度 |
|---|---|---|
| `-x` `!x` | 単項マイナス / 論理否定 | （前置） |
| `e as T` | 型キャスト | （後置） |
| `*` `/` `%` | 乗除・剰余 | 40 |
| `+` `-` | 加減 | 20 |
| `< > <= >= == !=` | 比較・等価（結果は bool） | 10 |
| `&&` | 論理積（短絡） | 6 |
| `\|\|` | 論理和（短絡） | 4 |
| `place = value` | 代入（結果は unit） | （最弱） |

`==` / `!=` は数値と `bool` に使用可。順序比較と算術は数値のみ。`&&` / `||` は
`bool` を取り短絡評価します。

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
if cond then expr                # else は省略可 — 文として扱われ値は ()
```

### for ループ（前判定・値は unit）

```
for i = start, cond, step in body   # cond(bool) が成り立つ間、各反復後に i += step
for i = 1, i < 6, 1 in printi(i as i64);   # 1 2 3 4 5（step 省略時は 1）
```

### 構造体・タプル

```
struct Point { x: f64, y: f64 }     # 公称の積型

fn norm2(p: Point) -> f64 = p.x * p.x + p.y * p.y;   # `.field` アクセス

Point { x: 3.0, y: 4.0 }.x;         # => 3
(6, 7).0 * (6, 7).1;                # => 42   (タプル + .0/.1)
```

構造体・タプルは値型（値渡し・値返し）です。

### 配列

```
let xs: [i64; 4] = [10, 20, 30, 40];   # 固定長 [T; N]・配列リテラル [..]
xs[2];                                  # => 30   (添字で要素を読む)
xs[0] = 99;                             # 添字経由で書き込み (let mut が必要)

let grid: [[i64; 2]; 2] = [[1, 2], [3, 4]];   # 配列は入れ子にできる
grid[1][0];                             # => 3
```

`[T; N]` は `T` の要素が `N` 個連続したものです。リテラルの全要素は同じ型で、
`[T; N]` は `T` が `Copy`（数値・`bool`）なら `Copy`、そうでなければ構造体と
同様にムーブされます。添字 `a[i]` は読み書きでき（書き込みには `mut` 束縛か
`&mut` が必要）、添字は任意の整数型です。**境界チェックされます**――範囲外や
負の添字は panic で異常終了します（最適化時は範囲内と分かるチェックは除去されます）。

### スライス

スライス `&[T]` / `&mut [T]` は配列への「長さを持つビュー」――fat pointer
`{ptr, len}` です。**配列を借用するとスライスになり**、関数が任意長の配列を
扱えるようになります:

```
fn sum(s: &[i64]) -> i64 = {        # 任意長のスライスを受け取る
  let mut total: i64 = 0;
  for i = 0 as i64, i < len(s), 1 in { total = total + s[i]; };
  total
};

fn fill(s: &mut [i64], v: i64) = {  # &mut [T] はスライス越しに書き込める
  for i = 0 as i64, i < len(s), 1 in { s[i] = v; };
};

{ let xs: [i64; 4] = [10, 20, 30, 40]; sum(&xs) };   # => 100  (&xs はスライス)
{ let mut ys: [i64; 3] = [0, 0, 0]; fill(&mut ys, 7); ys[0] };   # => 7
```

`len(s)` はスライスの長さを `i64` で返します。`s[i]` は i 番目の要素を読み
（`&mut [T]` なら書き）ます――配列の添字と同様、**境界チェックされます**。
`&[T]` は `Copy`（`&T` と同様）、`&mut [T]` はムーブ（`&mut T` と同様）。スライス
から非 `Copy` の要素をムーブして取り出すことはできません。

### ブロック・`let`・ミューテーション

```
fn triangle(n: i32) -> i32 = {     # ブロック { 文; 末尾式 } は式
  let mut s = 0;                    # let / let mut で局所束縛
  for i = 1, i < n, 1 in {
    s = s + i;                      # 代入 (s は mut が必要)
  };
  s                                 # 末尾の式がブロックの値
};
```

ブロック `{ 文; …; 末尾式 }` は文を順に実行し、末尾式を値とします（なければ `()`）。
`let x = e;` / `let mut x = e;` で局所束縛（`: T` 注釈は省略可）。代入 `place = value`
には `mut` 束縛か `&mut` 参照が必要です。関数本体は `= 式;` でもブロック `{ … }` でも可。

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
（`_` は無視）。enum の*参照*に対する match（`match &e` や `&self`/`&mut self`
メソッド内の `match self`）は借用するだけなので、束縛も借用になります。`Copy` の
ペイロードは束縛できますが、非 `Copy` のものは束縛**できません**（借用からムーブすると
二重解放になるため）。`_` で無視するか、束縛越しにフィールドを読んでください。

### ジェネリクスと `Option` / `Result`

enum と struct は型に対して**ジェネリック**にできます: `enum Name<T, …> { … }` /
`struct Name<T, …> { … }`。具体化ごとに*単態化*され（使用される型引数の組み合わせごとに
別の型を生成）、型引数 `Name<T1, …>` は構築時にペイロード／フィールドと期待型から
**推論**されます（turbofish 不要）。

```
struct Pair<A, B> { first: A, second: B }    # ジェネリック struct
Pair { first: 3, second: 4.0 }.second;        # => 4   (Pair<i32, f64>)
```

**関数**もジェネリックにできます（`fn name<T, …>(…)`）。呼び出しごとに単態化され、
本体は型引数を抽象のまま一度だけ検査されます。トレイト境界がないため本体は
「型に依存しない操作」に限られますが、`Option`/`Result` のヘルパにはこれで十分です:

```
fn unwrap_or<T>(o: Option<T>, dflt: T) -> T =
  match o { Some(x) => x, None => dflt };

unwrap_or(checked_div(10, 2), -1);    # => 5     (unwrap_or<i64>)
unwrap_or(Some(3.5), 0.0);            # => 3.5   (同じ定義を unwrap_or<f64> として)
```

`Option<T>` と `Result<T, E>` は組み込み（prelude）なので、`Some`/`None`・
`Ok`/`Err` はいつでも使えます:

```
fn checked_div(a: i64, b: i64) -> Option<i64> =
  if b == 0 then None else Some(a / b);     # None/Some は Option<i64> と推論

fn unwrap_or(o: Option<i64>, dflt: i64) -> i64 =
  match o { Some(x) => x, None => dflt };    # 具体化に対して match

unwrap_or(checked_div(10, 2), -1);           # => 5
unwrap_or(checked_div(10, 0), -1);           # => -1
```

```
enum Option<T>    { Some(T), None }          # prelude の組み込み定義
enum Result<T, E> { Ok(T), Err(E) }
```

推論できない箇所では型引数の明示が必要です（例: 裸の `None` には `: Option<i64>` の
注釈か型の決まった文脈が必要）。値として再帰する型（例 `enum List<T> { Cons(T, List<T>), Nil }`）は
無限サイズになるため拒否されます――参照・スライス・`Box<T>` による間接化が必要です（後述）。

### 早期リターンと `?` 演算子

`return e;`（または `return;`）で関数から早期に抜けられます――ガード節
（`if c then return x;`）に便利です。`?` 演算子はエラーを伝播します: `e?` は
`Option`/`Result` の成功時の中身を値とし、`None`/`Err` のときは関数から
早期リターンします（関数の戻り値型が対応する `Option`/`Result` である必要があります）。

```
fn ratio(a: i64, b: i64, c: i64) -> Option<i64> = {
  let first: i64  = checked_div(a, b)?;   # b == 0 ならここで None を返す
  let second: i64 = checked_div(first, c)?;
  Some(second)
};
```

### メソッド（impl ブロック）

`impl Type { … }` で型にメソッドを結びつけ、`recv.method(args)` で呼びます。
レシーバは `self`（値・ムーブ）・`&self`（共有借用）・`&mut self`（可変借用）。
呼び出し側でレシーバは自動的に借用され、フィールドアクセスは参照を自動 deref します
（`&self` メソッド内で `self.x` が書けます）。ジェネリック型にもメソッドを定義でき、
ジェネリック関数と同様に具体化ごとに単態化されます。

```
struct Point { x: i64, y: i64 }
impl Point {
  fn norm2(&self) -> i64 = self.x * self.x + self.y * self.y;
  fn shift(&mut self, dx: i64, dy: i64) { self.x = self.x + dx; self.y = self.y + dy; }
}

Point { x: 3, y: 4 }.norm2();        # => 25
{ let mut p: Point = Point { x: 1, y: 2 }; p.shift(10, 20); p.norm2() };   # => 605

impl Pair<A, B> { fn fst(&self) -> A = self.first; }   # ジェネリック型のメソッド
```

### トレイト

`trait` はインターフェース（メソッドのシグネチャ）を宣言し、`impl Trait for Type`
で実装します（適合性が検査されます）。ジェネリック関数はトレイトで**境界**をつけられ
（`fn f<T: Trait>(…)`）、本体で `T` の値に対しトレイトのメソッドを呼べます。呼び出しは
**静的ディスパッチ**（具体化ごとに具体 `impl` へ単態化）です。

```
trait Area { fn area(&self) -> i64; }

struct Square { side: i64 }
impl Area for Square { fn area(&self) -> i64 = self.side * self.side; }

Square { side: 5 }.area();                       # => 25   (直接呼べる)

fn describe<T: Area>(s: T) -> i64 = s.area() * 2;   # 境界つきジェネリック
describe(Square { side: 6 });                    # => 72
```

境界を満たさない型を渡すとエラーです。Self 型・デフォルトメソッド本体・ジェネリック型への
トレイト実装は今後の課題です（[ROADMAP.md](ROADMAP.md)）。

### 参照

```
fn get(p: &i64) -> i64 = *p;          # &T で借用、*p で参照外し
fn incr(p: &mut i64) = { *p = *p + 1; };   # &mut 経由で変更

{ let mut x: i64 = 41; incr(&mut x); x };  # => 42
```

`&x` / `&mut x` は場所（変数・フィールド等）を参照として借用し、`*p` で参照外し、
`*p = e` で `&mut` 経由の書き込みをします。ローカルはメモリ常駐（アドレスを取れる）で、
`-O` で再びレジスタへ昇格します。可変性は検査されます（非 `mut` への代入や `&mut` 借用は
エラー）。

### ムーブ意味論

非 `Copy` 型（`struct`・`enum`・タプル・`&mut T`）は値の受け渡し・束縛で**ムーブ**され、
元の変数は使えなくなります。`Copy` 型（数値・`bool`・`&T`）はコピーされ、借用 `&x` は
ムーブしません。

```
struct Buf { n: i64 }
fn consume(b: Buf) -> i64 = b.n;

{ let a = Buf { n: 7 }; let b = a; consume(b) };   # OK: a を b にムーブ後、b を使用
# { let a = Buf { n: 7 }; let b = a; consume(a) }  # エラー: ムーブ済みの a を使用
```

ムーブ後の使用はコンパイルエラー（直線コード・`if`/`match` の分岐・ループ本体で検査）。
エイリアス/ライフタイム検査（`&mut` は排他・`&` は複数可・ダングリング防止）は Phase 3 の
残作業です（[ROADMAP.md](ROADMAP.md)）。

### `Box<T>`（ヒープ）

`box(e)` は `e` をヒープに確保して `Box<T>`（所有ポインタ）を返し、`*b` で中身を読みます。
`Box` はポインタなので、値だと無限サイズになる**再帰的なデータ型**を表現できます:

```
let b: Box<i64> = box(42);
*b;                                          # => 42

enum List<T> { Cons(T, Box<List<T>>), Nil }  # Box で再帰
fn sum(l: List<i64>) -> i64 =
  match l { Cons(h, t) => h + sum(*t), Nil => 0 };

sum(Cons(10, box(Cons(20, box(Nil)))));      # => 30
```

Box は所有者がスコープを抜けるときに**自動で解放**されます（Drop/RAII）――
ブロック末・関数末・早期 `return`/`?` の各経路で。ムーブされた値は drop されず
（ローカルごとに追跡するので条件付きムーブも正しく扱われます）、解放は
struct/enum/tuple/array に再帰します。上のリストは全ノードが解放され、リークも
二重解放もありません。

### 組み込み関数
- `printi(x: i64)` … 整数を 1 行で表示
- `printd(x: f64)` … 浮動小数点を 1 行で表示
- `putchard(x: i64)` … 文字コード x の 1 文字を出力（例: `putchard(10)` で改行）
- `len(s: &[T]) -> i64` … スライスの長さ
- `box(x: T) -> Box<T>` … x をヒープへ移す

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
│   ├── MoveCheck.h          #   ムーブ意味論 / use-after-move
│   └── CodeGen.h            #   型付き AST → LLVM IR
├── src/                     # 実装 + main.cpp（JIT ドライバ）
├── examples/                # arith, fib, loop, extern, cast, struct, enum, ref, mut, move, operators, arrays, slices, option, generic, generic_struct, generic_fn, bool, methods, trait, question, box
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
**[ROADMAP.md](ROADMAP.md)**（英語）に、各リリースの内容は
**[CHANGELOG.md](CHANGELOG.md)** にまとめています。

今すぐ着手するなら: パースエラーを診断モジュール経由にする、ソース span を付与する、
`src/kal.cpp` を lexer/parser/ast/codegen に分割する（Phase 0）あたりから。
