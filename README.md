# Kal — a tiny expression language built on LLVM

[![CI](https://github.com/nktkt/kal/actions/workflows/ci.yml/badge.svg)](https://github.com/nktkt/kal/actions/workflows/ci.yml)

> 🇯🇵 日本語版は [README.ja.md](README.ja.md)

Kal is a minimal "calculator / expression" language (in the spirit of LLVM's
*Kaleidoscope* tutorial). It compiles source to **LLVM IR** and runs it
on the fly with an **ORC JIT**.

```
source → [Lexer] → [Parser] → AST → [Sema] → typed AST → [MoveCheck] → [CodeGen] → LLVM IR → [ORC JIT] / native binary
```

It is **statically typed** (`i8`…`i64`, `u8`…`u64`, `f32`, `f64`, `bool`) with a
type checker that infers literal types from context and reports precise errors;
there are no implicit numeric conversions (use `as`).

The compiler is split into small, focused units under `src/` and `include/kal/`
(source manager, span-aware diagnostics, lexer, parser, a codegen-free AST, a
type checker, and codegen), so it doubles as a readable introduction to building
a real language frontend on LLVM. This layered structure is the foundation for
the long-term plan in [ROADMAP.md](ROADMAP.md).

---

## Build

Requirements: **LLVM 22** (e.g. Homebrew `llvm`), **CMake 3.20+**, a C++17 compiler.

```sh
cd kal
cmake -S . -B build -DLLVM_DIR="$(llvm-config --cmakedir)" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

This produces `build/kalc`.

> Note: your editor may show red squiggles about missing LLVM headers. The
> CMake build (which passes `-I$(llvm-config --includedir)`) compiles fine.

## Run & compile

`kalc` can JIT-execute a program or compile it ahead-of-time to a standalone
native binary:

```sh
./build/kalc run examples/fib.kal        # JIT compile and execute (default)
./build/kalc examples/fib.kal            # same — `run` is the default command
echo '1 + 2 * 3;' | ./build/kalc         # read from stdin → 7

./build/kalc build examples/fib.kal -o fib   # compile to a native executable
./fib                                        # ...that runs on its own → 55 610 6765

./build/kalc -O2 build examples/fib.kal -o fib   # with optimization (-O0..-O3)
./build/kalc emit-ir  examples/fib.kal       # print the generated LLVM IR
./build/kalc emit-obj examples/fib.kal -o fib.o   # write an object file
```

AOT binaries are self-contained (they only link libc/libm): `kalc` emits the
runtime (`printi`/`printd`/`putchard`) and a C `main` into the module, optimizes
with LLVM's `PassBuilder`, emits an object file via the host `TargetMachine`, and
links it with `cc`.

Top-level expressions are **auto-printed** by type (integers, floats, bools).
Expressions of type `()` (unit) — loops and the print built-ins — print nothing.

---

## Language reference

### Types
`i8 i16 i32 i64`, `u8 u16 u32 u64`, `f32 f64`, `bool`, `()` (unit), user-defined
**`struct`s**, **`enum`s** (algebraic data types), **tuples** `(T, U, …)`, and
**references** `&T` / `&mut T`.
Integer literals default to **i32**, float literals to **f64**, but a literal
takes its type from context (e.g. `2` is `i64` in `n < 2` when `n: i64`).
There are **no implicit conversions** — convert explicitly with `as`.

### Operators (highest precedence first)

| Operator | Meaning | Precedence |
|----------|---------|-----------|
| `-x` `!x` | unary negate / logical not | (prefix) |
| `e as T` | type cast | (postfix) |
| `*` `/` `%` | multiply / divide / remainder | 40 |
| `+` `-` | add / subtract | 20 |
| `< > <= >= == !=` | comparison & equality (→ bool) | 10 |
| `&&` | logical and (short-circuit) | 6 |
| `\|\|` | logical or (short-circuit) | 4 |
| `place = value` | assignment (→ unit) | (lowest) |

`==` / `!=` work on numbers and `bool`; the ordered comparisons and arithmetic
are numeric-only; `&&` / `||` take `bool` and short-circuit.

### Functions and externs

```
fn add(a: i64, b: i64) -> i64 = a + b;   # typed params, `-> T` return, `= expr` body
fn fib(n: i64) -> i64 =
  if n < 2 then n
  else fib(n - 1) + fib(n - 2);

extern sin(x: f64) -> f64;               # declare & call external (e.g. libm) functions
sin(0.0);                                # => 0
```

Omitting `-> T` makes the return type `()` (unit).

### Casts

```
9 as f64;        # i32 → f64
3.9 as i64;      # f64 → i64 (truncates) → 3
```

### if / then / else (an expression)

```
if cond then expr1 else expr2    # cond must be bool; both branches share one type
```

### for loop (pre-tested, evaluates to unit)

```
for i = start, cond, step in body   # while cond (bool) holds; i += step each iteration
for i = 1, i < 6, 1 in printi(i as i64);   # 1 2 3 4 5  (step defaults to 1)
```

### Structs & tuples

```
struct Point { x: f64, y: f64 }     # nominal product type

fn norm2(p: Point) -> f64 = p.x * p.x + p.y * p.y;   # `.field` access

Point { x: 3.0, y: 4.0 }.x;         # => 3
(6, 7).0 * (6, 7).1;                # => 42   (tuple + .0/.1)
```

Structs and tuples are value types (passed/returned by value).

### Blocks, `let` & mutation

```
fn triangle(n: i32) -> i32 = {     # a block { stmts; tail } is an expression
  let mut s = 0;                    # let / let mut bindings
  for i = 1, i < n, 1 in {
    s = s + i;                      # assignment (s must be `mut`)
  };
  s                                 # trailing expression is the block's value
};
```

A block `{ stmt; …; tail }` runs statements and evaluates to its trailing
expression (or `()` if absent). `let x = e;` / `let mut x = e;` introduce locals
(`: T` annotation optional). Assignment `place = value` requires a `mut` binding
or a `&mut` reference. A function body may be `= expr;` or a block `{ … }`.

### Enums & pattern matching

```
enum Shape {            # Rust-style algebraic data type (tagged union)
  Circle(f64),
  Rect(f64, f64),
}

fn area(s: Shape) -> f64 =
  match s {             # match is checked for exhaustiveness
    Circle(r)  => 3.14 * r * r,
    Rect(w, h) => w * h,
  };

area(Circle(2.0));      # => 12.56   (variants are constructed by name)
```

A `match` arm is `Variant(bindings) => expr` or a `_` wildcard; it must cover
every variant (or include `_`). Variant payloads are bound by the names in the
pattern (`_` ignores one).

### References

```
fn get(p: &i64) -> i64 = *p;          # &T borrow, *p dereference
fn incr(p: &mut i64) = { *p = *p + 1; };   # mutate through &mut

{ let mut x: i64 = 41; incr(&mut x); x };  # => 42
```

`&x` / `&mut x` borrow a place (variable, field, …) as a reference; `*p`
dereferences and `*p = e` writes through a `&mut`. Locals are memory-backed so
they're addressable (`-O` promotes
them back to registers). Mutability is checked (assigning to a non-`mut` binding,
or `&mut` of one, is an error).

### Move semantics

Non-`Copy` values (`struct`, `enum`, tuples, `&mut T`) are **moved** when passed
or bound by value; the source is then unusable. `Copy` types (numbers, `bool`,
`&T`) are copied instead, and borrowing (`&x`) never moves.

```
struct Buf { n: i64 }
fn consume(b: Buf) -> i64 = b.n;

{ let a = Buf { n: 7 }; let b = a; consume(b) };   # ok: a moved into b, then b used
# { let a = Buf { n: 7 }; let b = a; consume(a) }  # error: use of moved value `a`
```

Use-after-move is a compile error (checked in straight-line code, across
`if`/`match` branches, and for loop bodies). Aliasing/lifetime checking (one
`&mut` xor many `&`, no dangling) is the remaining Phase 3 work
([ROADMAP.md](ROADMAP.md)).

### Built-ins
- `printi(x: i64)` — print an integer on its own line
- `printd(x: f64)` — print a float on its own line
- `putchard(x: i64)` — write the character with code `x` (`putchard(10)` is a newline)

### Comments
`#` to end of line.

---

## Layout

```
kal/
├── CMakeLists.txt           # links against LLVM via find_package(LLVM)
├── include/kal/             # public headers
│   ├── SourceManager.h      #   source files, byte spans, line/column
│   ├── Diagnostic.h         #   span-aware, rustc-style error reporting
│   ├── Token.h  Lexer.h     #   tokens + lexer
│   ├── Type.h               #   the type system
│   ├── AST.h    Parser.h    #   codegen-free AST + parser
│   ├── Sema.h               #   type checker (annotates the AST)
│   ├── MoveCheck.h          #   move semantics / use-after-move
│   └── CodeGen.h            #   typed AST → LLVM IR
├── src/                     # implementations + main.cpp (JIT driver)
├── examples/                # arith, fib, loop, extern, cast, struct, enum, ref, mut, move, operators
├── tests/                   # golden-test harness (run_tests.sh) + cases
└── .github/workflows/ci.yml # build + test on Linux & macOS
```

The pipeline is layered: `Lexer → Parser (AST) → Sema (typed AST) → CodeGen
(LLVM IR) → ORC JIT`. The AST carries no codegen logic — `Sema` annotates it
with types and `CodeGen` walks it separately — which keeps each stage independent
and ready to grow (a full HIR/MIR, a borrow checker; see [ROADMAP.md](ROADMAP.md)).

Diagnostics point at the exact source span, e.g.:

```
error[E0121]: 二項演算の両辺の型が一致しません (i32 と f64)
 --> prog.kal:1:1
  |
1 | 1 + 2.0;
  | ^^^^^^^
```

## Tests

```sh
cmake --build build -j        # build kalc first
bash tests/run_tests.sh       # run the golden suite
bash tests/run_tests.sh --bless   # regenerate expected outputs after intended changes
```

Example programs are checked against `tests/expected/*.out`; deliberately
invalid programs in `tests/diagnostics/` are checked against their expected
`*.stderr`. CI runs this suite on Linux and macOS for every push and PR.

---

## Roadmap

Kal v0.1 is a JIT calculator today, but the goal is bigger: a **general-purpose,
ahead-of-time compiled systems language with ownership-based memory management
(no GC)** — in the spirit of Rust / Zig, built on LLVM.

The full long-term plan — compiler re-architecture, type system, borrow checker,
generics, standard library, tooling (LSP / package manager), and the path to
v1.0 — lives in **[ROADMAP.md](ROADMAP.md)**; see **[CHANGELOG.md](CHANGELOG.md)**
for what each release ships.

Good first steps if you want to hack on it now: route parse errors through a real
diagnostics module, add source spans, and split `src/kal.cpp` into
lexer/parser/ast/codegen units (Phase 0).

---

## License

MIT — see [LICENSE](LICENSE).
