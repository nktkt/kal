# Kal — a tiny expression language built on LLVM

> 🇯🇵 日本語版は [README.ja.md](README.ja.md)

Kal is a minimal "calculator / expression" language (in the spirit of LLVM's
*Kaleidoscope* tutorial). It compiles source to **LLVM IR** and runs it
on the fly with an **ORC JIT**.

```
source → [Lexer] → tokens → [Parser] → AST → [CodeGen] → LLVM IR → [ORC JIT] → run
```

The whole implementation lives in a single, heavily-commented file:
[`src/kal.cpp`](src/kal.cpp) (~700 lines), so it doubles as a readable
introduction to building a language frontend on LLVM.

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
├── CMakeLists.txt      # links against LLVM via find_package(LLVM)
├── src/kal.cpp         # Lexer / Parser / CodeGen / JIT driver
└── examples/
    ├── arith.kal       # arithmetic & precedence
    ├── fib.kal         # recursion (Fibonacci)
    ├── loop.kal        # for loops and putchard
    └── extern.kal      # calling libm's sin/cos/sqrt
```

`src/kal.cpp` is organized into sections matching its comment headers:
1. **Lexer** — characters → tokens
2. **AST** — syntax-tree node definitions
3. **Parser** — recursive descent + operator-precedence parsing
4. **CodeGen** — each node's `codegen()` emits LLVM IR
5. **Runtime** — `printd` / `putchard` (host-side `extern "C"`)
6. **Driver** — parse → declare all prototypes → emit bodies → emit `__main` → JIT

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
