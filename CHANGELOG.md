# Changelog

All notable changes to Kal. Versions track [ROADMAP.md](ROADMAP.md) phases.
Pre-1.0 releases are unstable: syntax and semantics may change between versions.

## [Unreleased]

- **Operators:** `==` `!=` `<=` `>=`, remainder `%`, unary `-` and `!`, and
  short-circuiting `&&` / `||`. `==`/`!=` work on numbers and `bool`; logical
  operators take `bool`. Float comparisons are now ordered.

## [v0.5.0-alpha] — 2026-05-23

Phases 0–2 complete; Phase 3 (ownership) in progress. Kal is a small but real
statically-typed systems language that JIT-runs or compiles to standalone native
binaries. Every release below builds green on Linux and macOS (LLVM 22) and is
covered by a golden test suite (39 cases: examples × JIT/AOT + diagnostics).

### Language
- **Types:** `i8…i64`, `u8…u64`, `f32`, `f64`, `bool`, `()` (unit); user-defined
  `struct`s, `enum`s (Rust-style ADTs), tuples `(T, U)`, and references
  `&T` / `&mut T`. Static type checking with bidirectional literal inference; no
  implicit numeric conversions (explicit `as` casts).
- **Expressions:** arithmetic with precedence, comparisons, `if/then/else`,
  pre-tested `for` loops, function calls and recursion, `extern` declarations.
- **Aggregates:** struct literals & field access, tuple `.0`/`.1`, `enum`
  construction by name and `match` with payload binding, `_` wildcards, and
  **exhaustiveness checking**.
- **Blocks & mutation:** block expressions `{ stmt; …; tail }`, `let` / `let mut`
  bindings (optional `: T`), assignment, `*p = e` through `&mut`, block-bodied
  functions. Mutability is checked.
- **Ownership (in progress):** references, borrows `&`/`&mut`, dereference `*p`;
  **move semantics** with **use-after-move** detection (Copy vs move types).
  Aliasing/lifetime borrow checking is still to come.

### Toolchain
- `kalc run` (ORC JIT), `kalc build -o out` (native executable via the host
  TargetMachine + `cc`), `kalc emit-ir` / `emit-obj`, `-O0`…`-O3` optimization
  (LLVM `PassBuilder`). AOT binaries are self-contained (link only libc/libm).
- rustc-style, span-pointed diagnostics with error codes.

### Compiler architecture
Layered pipeline: `Lexer → Parser → Sema → MoveCheck → CodeGen → LLVM`.
The AST carries no codegen logic; passes are independent and ready to grow
(a MIR + full borrow checker, generics/traits, a standard library — see
[ROADMAP.md](ROADMAP.md)).

### Milestones in this release
- **Phase 0** — compiler re-architecture (source spans, diagnostics engine,
  modular pipeline, golden tests, CI).
- **Phase 1** — static type system (replaces the doubles-only model).
- **Phase 2** — structs, tuples, enums, `match`; AOT compilation + optimization.
- **Phase 3 (partial)** — references, blocks/`let mut`/assignment, move semantics.

### Known limitations
- No aliasing/lifetime borrow checking yet (one `&mut` xor many `&`, dangling).
- Move checking is precise for straight-line code and conservative across loops;
  partial moves of a non-Copy field move the whole base.
- `char`, arrays/slices, generics, traits, and a standard library are future work.
