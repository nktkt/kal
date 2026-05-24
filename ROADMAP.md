# Kal Roadmap — from a JIT toy to a scalable systems language

> Status: living document. Horizons are relative and assume a small team; they
> describe **order and dependencies**, not committed dates.

## 1. Vision

Kal aims to become a **general-purpose, ahead-of-time compiled systems
language** with **ownership-and-borrowing** memory management (no GC), in the
spirit of Rust / Zig — built on LLVM.

The headline promise: **memory safety and high performance without a garbage
collector**, with a toolchain that scales from a one-file script to a
million-line codebase.

### What "scalable product" means here

We track scalability on four explicit axes, and every phase must defend them:

| Axis | Question | Levers |
|------|----------|--------|
| **Code scale** | Does a 1M-line program compile in reasonable time? | Incremental + parallel compilation, modular IR, query-based compiler |
| **Performance scale** | Is generated code competitive with C? | Monomorphization, LLVM opt pipeline, zero-cost abstractions |
| **Team scale** | Can many contributors work without breaking things? | Layered architecture, golden tests, CI, RFC process |
| **User scale** | Can outsiders adopt it productively? | Package manager, LSP, docs, registry, cross-platform binaries |

## 2. Design principles

1. **Errors are a feature.** Diagnostics with spans, codes, and fix-it hints from day one (rustc-quality is the bar).
2. **Layered IRs.** `AST → HIR (typed) → MIR (CFG) → LLVM IR`. Each layer has one job.
3. **No undefined behavior in safe code.** `unsafe` is an explicit, auditable escape hatch.
4. **Zero-cost abstractions.** Generics monomorphize; ownership is compile-time only.
5. **Tooling is part of the language.** Formatter, LSP, test runner, and package manager are first-class, not afterthoughts.
6. **Test everything, always.** Every phase ships with golden/snapshot tests and CI green.

### Non-goals (at least through v1.0)
- A garbage collector (ownership is the model).
- Full C++ template-level metaprogramming.
- Source/ABI stability *before* v1.0 (we will break things deliberately to get the design right).

## 3. Target architecture (what we are refactoring toward)

```
 source files (*.kal)
   → [Lexer]          tokens + byte spans
   → [Parser]         AST
   → [Resolver]       modules, name resolution        → resolved AST
   → [Type checker]   inference + checking             → HIR (typed)
   → [MIR lowering]   desugar to control-flow graph    → MIR
   → [Borrow checker] ownership / lifetimes on MIR
   → [Codegen]        MIR → LLVM IR
   → [LLVM]           optimize + emit object files
   → [Linker]         native executable / library
```

The current v0.1 collapses *all* of this into one pass in `src/kal.cpp`. Phase 0
breaks it apart — that refactor *is* the foundation for everything else.

---

## 4. Phased roadmap

Effort key: **S** ≤1 mo · **M** 1–3 mo · **L** 3–6 mo · **XL** 6 mo+ (small team).

### Horizon A — Foundations (near term)

#### Phase 0 — Compiler re-architecture · **L** · → v0.2 · ✅ done
The single-pass design cannot scale to a real language. Rebuild the skeleton.
- Source manager: multi-file input, byte spans, line/column mapping.
- **Diagnostics engine**: errors/warnings with spans, severity, error codes, colored output, and `--error-format=json` for tools.
- Split into modules/libraries: `lexer`, `parser`, `ast`, `diagnostics`, `driver`.
- Real **AST** decoupled from codegen (no more `codegen()` on parse nodes).
- **Test infrastructure**: golden tests for lexer/parser/diagnostics + `examples/` run as integration tests.
- **CI** (GitHub Actions): build + test on Linux/macOS, every PR.
- *Exit:* current v0.1 features work on the new pipeline, fully tested, CI green.

#### Phase 1 — Static type system (monomorphic core) · **L** · → v0.3 · ✅ done
- Primitive types: `i8…i64`, `u8…u64`, `f32`, `f64`, `bool`, `()` (unit). ✅ (`char` deferred)
- Typed function signatures (`fn f(x: T) -> T`); **bidirectional literal inference**. ✅
- A `Sema` type-checker annotates the AST with types; precise type errors via the
  diagnostics engine. (A separate HIR tree is deferred — the typed AST plays that role.)
- No implicit numeric conversions — explicit `as` casts. The "everything is a
  double" model is gone. ✅
- *Exit:* programs are type-checked; mismatches produce precise errors. ✅

### Horizon B — A real language (mid term)

#### Phase 2 — Aggregates, pattern matching & AOT · **L** · → v0.4 · ✅ done

**2a — AOT & optimization · ✅ done**
- **AOT compilation**: `kalc build` emits an object file via the host
  `TargetMachine` and links it with `cc` into a standalone native binary;
  `kalc run` keeps the JIT path; `emit-obj` / `emit-ir` for intermediates. ✅
- A self-contained runtime (`printi`/`printd`/`putchard`) and a C `main` are
  emitted into the module for AOT builds. ✅
- **LLVM optimization pipeline** via `PassBuilder` (`-O0`…`-O3`). ✅
- CI builds each example AOT and checks the binary's output. ✅

**2b-i — Product types & locals · ✅ done**
- `struct` (nominal, by-value), field access, tuples `(T, U)` with `.0`/`.1`. ✅
- `let name = e in body` immutable local bindings. ✅
- Aggregates flow through Sema (typed) and CodeGen (`insertvalue`/`extractvalue`),
  and work in both JIT and AOT builds (tested). ✅

**2b-ii — Sum types & pattern matching · ✅ done**
- `enum` (Rust-style algebraic data types with payloads); variants constructed
  by name; lowered to a tagged union `{ i64 tag, [N x i8] payload }`. ✅
- `match` with payload binding, `_` wildcards, and **exhaustiveness checking**. ✅
- Works in JIT and AOT (tested).
- Deferred to a later phase: fixed arrays, slices.

#### Phase 3 — Ownership & borrow checking · **XL** · → v0.5 *(the differentiator)*
- **Reference foundation · ✅ done (pre-step):** `&T` / `&mut T` types, `&`/`&mut`
  borrows, `*p` dereference, pass-by-reference, and `let name: T = …` annotations.
  Locals are memory-backed (alloca) so they're addressable; `-O` re-promotes them.
- **Blocks & mutation · ✅ done:** block expressions `{ stmt; …; tail }`,
  `let` / `let mut` statements, assignment `place = value`, `*p = e` through
  `&mut`, block-bodied functions `fn f() { … }`. Mutability is checked (no
  assigning to / `&mut`-borrowing a non-`mut` place). Replaced `let … in`.
- **Move semantics · ✅ done:** `Copy` (numbers, `bool`, `&T`) vs move (`struct`,
  `enum`, tuples, `&mut T`); affine values; **use-after-move** is a compile error
  (straight-line, `if`/`match` branch merge, and loop-body moves). A `MoveCheck`
  pass runs on the typed AST after `Sema`.
- **Borrow checker**; start with NLL-style (non-lexical) regions: aliasing
  rules (one `&mut` xor many `&`), no dangling. *(next)*
- `Drop` / RAII / deterministic destruction.
- `unsafe` blocks with documented invariants.
- *Exit:* memory-safe **without GC**; use-after-move, double-free, and aliasing violations are compile errors.

#### Phase 4 — Generics & traits · **L** · → v0.6
- Generics via **monomorphization** — ✅ **generic enums, structs, and functions
  done** (`Option<T>`, `Result<T, E>` ship as a prelude; type args inferred at
  construction/call; infinite-size recursive types rejected). Generic function
  bodies are checked abstractly; **trait bounds** are what remain to let them do
  more than move values around.
- **Methods** — ✅ **inherent `impl` blocks done** (`impl Type { fn m(self/&self/
  &mut self, …) }`, `recv.method(…)`, auto-borrow + field auto-deref; generic
  impls monomorphize).
- **Traits** — ✅ **core done** (`trait` + `impl Trait for Type` with conformance
  checking; bounded generics `fn f<T: Trait>` with static dispatch via
  monomorphization). Remaining: associated types, default methods, `Self` types,
  trait impls on generic types, and trait objects (dynamic dispatch).
- *Exit:* write reusable generic data structures (`Vec<T>`, `Map<K,V>`).

#### Phase 5 — Standard library & error handling · **L** · → v0.7
- Heap primitives: allocator interface, `Box<T>`, `Vec<T>`, `String`, `HashMap`.
  ✅ `Box<T>` (via `box(e)`) and **Drop/RAII** landed — boxes are freed
  automatically (drop flags for moves; recurses into aggregates).
  ✅ `Vec<T>` (growable array) landed — `vec()`/`push`/`len`/`v[i]`, capacity
  doubling via `realloc`, element-aware Drop.
  ✅ String literals + `str` landed — a `Copy` `{ptr,len}` view of static UTF-8
  (`prints`/`len`/`s[i]`).
  ✅ Owned `String` landed — heap `{ptr,len,cap}` (`string`/`push_str`), Drop-freed.
  ✅ String comparison (`== != < <= > >=`, lexicographic via `memcmp`) landed.
  ✅ `Vec` `pop` (→ `Option<T>`) and `clear` landed.
  ✅ Discarded owned temporaries at statement position are now dropped (no leak).
  ✅ String concatenation `+` (str/String → owned String) landed, leak-free for
  chains and reassignment.
  ✅ `String`→`str` coercion at function-call arguments landed (borrow, no move).
  Next: `HashMap`; broaden coercion to methods/generics/returns (with the borrow
  checker).
  (Remaining: iteration; dropping owned temporaries only *borrowed* inside a
  larger call argument, e.g. `prints(a + b)` — needs per-temporary tracking,
  naturally part of the MIR.)
- `Option<T>` / `Result<T,E>` and the `?` operator — ✅ done (prelude types +
  early `return` and `?`). (Heap-backed collections still pending.)
- Iterators and closures.
- IO, filesystem, process, time; `core` (no_std) / `std` split.
- *Exit:* nontrivial real programs without external dependencies.

### Horizon C — Product & ecosystem (long term)

#### Phase 6 — Tooling & developer experience · **XL** · → v0.8
- **Package manager + build tool**: manifest, semver dependency resolution, lockfile.
- **Formatter** (`kalfmt`) and linter.
- **Language Server (LSP)**: completion, go-to-definition, inline diagnostics, rename — the single biggest adoption lever.
- Test runner (`kalc test`); doc generator (`kalc doc`).
- **Debug info (DWARF)** for gdb/lldb.
- *Exit:* install Kal → scaffold a project → build/test/format with full IDE support.

#### Phase 7 — Scale & production hardening · **XL** · → v0.9
- **Incremental compilation** (query-based / salsa-style) — fast rebuilds on large codebases.
- **Parallel compilation** across modules.
- **Cross-compilation** and multiple targets, including **WebAssembly**.
- Concurrency model: threads, channels, and a design decision on `async`.
- **C FFI** for interop; ABI considerations.
- Benchmark suite + performance regression tracking in CI.
- *Exit:* large codebases build fast and run on Linux/macOS/Windows/wasm.

#### Phase 8 — Stability, community & v1.0 · **XL** · → **v1.0**
- **Language specification** + **RFC process** for changes.
- Semantic-versioning policy, stability guarantees, an **edition** mechanism for opt-in breaking changes.
- **Package registry** (a crates.io-style index) and a **web playground** (compile to wasm in the browser).
- Documentation site and a "Kal Book".
- **Self-hosting**: bootstrap the Kal compiler in Kal — the ultimate maturity signal.
- Governance model, `CONTRIBUTING`, `SECURITY`, code of conduct.
- *Exit:* **v1.0** — stable, specified, documented, community-driven.

---

## 5. Cross-cutting tracks (run in every phase)

- **Quality:** golden tests, fuzzing the parser/type-checker, CI gating every merge.
- **Performance:** a benchmark suite from Phase 2; watch both *compile time* and *runtime*.
- **Security:** the compiler is a trusted component — fuzz it, and define a vulnerability disclosure policy by Phase 6.
- **Docs:** keep the language reference in lockstep with features; never let docs lag a release.
- **Sustainability:** decide early how the project is funded/maintained if it grows (sponsors, foundation).

## 6. Release strategy

- `v0.x` are **unstable**: we break syntax/semantics freely to reach the right design.
- Each phase cuts a `v0.N` tag with release notes and migration guidance.
- After v0.9, a **beta** period freezes the language for feedback, then **v1.0** ships the stability promise + editions.

## 7. Risk register

| Risk | Mitigation |
|------|------------|
| Borrow checker (Phase 3) is the hardest part and may stall | Time-box an NLL prototype; ship a usable subset before full lifetimes |
| LLVM API churn across versions | Pin LLVM, isolate codegen behind a thin adapter, test in CI against the pinned version |
| Scope creep delays a usable release | Each phase has a hard *exit criterion*; resist adding features mid-phase |
| Solo bandwidth | Front-load tests + docs so contributors can join at any phase |
| No adoption | Prioritize the LSP and package manager (Phase 6) — DX drives uptake |

## 8. Immediate next steps (start of Phase 0)

1. Add a **diagnostics module** (spans + pretty errors) and route all current parse errors through it.
2. Introduce a **source manager** and attach spans to every token and AST node.
3. Split `src/kal.cpp` into `lexer` / `parser` / `ast` / `codegen` / `driver` units.
4. Add **golden tests** + a **GitHub Actions CI** workflow (build + run `examples/`).
5. Decide the **surface syntax** (the chosen `fn name(x: T) -> U` style) and lock a grammar sketch in `docs/`.

> The fastest credible milestone: **Phase 0 + Phase 1** turns Kal from a
> calculator into a typed language on a real compiler architecture. Everything
> after that compounds on that foundation.
