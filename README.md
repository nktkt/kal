# Kal — a tiny expression language built on LLVM

[![CI](https://github.com/nktkt/kal/actions/workflows/ci.yml/badge.svg)](https://github.com/nktkt/kal/actions/workflows/ci.yml)

> 🇯🇵 日本語版は [README.ja.md](README.ja.md)

Kal is a minimal "calculator / expression" language (in the spirit of LLVM's
*Kaleidoscope* tutorial). It compiles source to **LLVM IR** and runs it
on the fly with an **ORC JIT**.

```
source → [Lexer] → tokens → [Parser] → AST → [CodeGen] → LLVM IR → [ORC JIT] → run
```

The compiler is split into small, focused units under `src/` and `include/kal/`
(source manager, span-aware diagnostics, lexer, parser, a codegen-free AST, and
codegen), so it doubles as a readable introduction to building a real language
frontend on LLVM. This layered structure is the foundation for the long-term
plan in [ROADMAP.md](ROADMAP.md).

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

## Run

```sh
./build/kalc examples/fib.kal       # run a file
./build/kalc < examples/fib.kal     # read from stdin
echo '1 + 2 * 3;' | ./build/kalc    # one-liner → 7
./build/kalc --emit-ir examples/fib.kal   # print the generated LLVM IR (no run)
```

Top-level expressions are **auto-printed** (calculator / REPL behaviour).
Everything is an expression that yields a value, so statements like loops and
`putchard` — which return `0` — also print `0`.

---

## Language reference

### Values
Everything is a **double** (64-bit float). Booleans are `1.0` (true) / `0.0` (false).

### Operators (highest precedence first)

| Operator | Meaning            | Precedence |
|----------|--------------------|-----------|
| `*` `/`  | multiply / divide  | 40        |
| `+` `-`  | add / subtract     | 20        |
| `<` `>`  | comparison (→1/0)  | 10        |

### Functions and externs

```
def add(a b) a + b;          # args are space-separated; body is a single expression
def fib(n)
  if n < 2 then n
  else fib(n-1) + fib(n-2);

extern sin(x);               # declare and call external (e.g. libm) functions
sin(0);                      # => 0
```

### if / then / else (an expression)

```
if cond then expr1 else expr2    # expr1 when cond != 0, otherwise expr2
```

### for loop (pre-tested)

```
for i = start, cond, step in body
# runs body while cond holds, doing i += step after each iteration
# (step defaults to 1); the loop expression always evaluates to 0
for i = 1, i < 6, 1 in printd(i*i);   # 1 4 9 16 25
```

### Built-ins
- `printd(x)` — print `x` on its own line
- `putchard(x)` — write the character with code `x` (e.g. `putchard(10)` is a newline)

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
│   ├── AST.h    Parser.h    #   codegen-free AST + parser
│   └── CodeGen.h            #   AST → LLVM IR
├── src/                     # implementations + main.cpp (JIT driver)
├── examples/                # arith, fib, loop, extern
├── tests/                   # golden-test harness (run_tests.sh) + cases
└── .github/workflows/ci.yml # build + test on Linux & macOS
```

The pipeline is layered: `Lexer → Parser (AST) → CodeGen (LLVM IR) → ORC JIT`.
The AST carries no codegen logic — `CodeGen` walks it separately — which keeps
each stage independent and ready to grow (typed HIR, MIR, a borrow checker; see
[ROADMAP.md](ROADMAP.md)).

Diagnostics point at the exact source span, e.g.:

```
error[E0100]: 未定義の変数です
 --> prog.kal:2:9
  |
2 | def f() x;
  |         ^
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
v1.0 — lives in **[ROADMAP.md](ROADMAP.md)**.

Good first steps if you want to hack on it now: route parse errors through a real
diagnostics module, add source spans, and split `src/kal.cpp` into
lexer/parser/ast/codegen units (Phase 0).

---

## License

MIT — see [LICENSE](LICENSE).
