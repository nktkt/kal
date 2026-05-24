# Changelog

All notable changes to Kal. Versions track [ROADMAP.md](ROADMAP.md) phases.
Pre-1.0 releases are unstable: syntax and semantics may change between versions.

## [Unreleased]

- **Associated functions and `::`.** A function in an `impl` block that takes no
  `self` is an associated function (constructor/factory), called as
  `Type::name(args)` (new `::` token). Methods and associated functions coexist in
  one `impl`; on a generic type the type arguments are inferred from the call
  (`Pair::make(7, true)` → `Pair<i64, bool>`). Arguments are by value (moved);
  monomorphized like methods. Diagnostics `E0270`/`E0271` (parse), `E0272`
  (unknown associated function), `E0206` (name clash with a method).
- **Borrowed temporaries are dropped.** An owned-heap temporary passed to a
  borrowing operation is now freed after that operation, closing the most common
  remaining leaks: `prints(a + b)`, `len(make_vec())`, `push_str(s, a + b)`, and a
  temporary `String` coerced to a `str` parameter (`f("a" + "b")`). Place
  arguments (variables, fields) are still borrowed and kept. (Two narrow gaps
  remain — an indexed or method-receiver temporary, e.g. `(a + b)[0]` / `mk().m()`
  — both memory-safe.)
- **`String` → `str` coercion (argument position).** Passing a `String` to a
  function parameter of type `str` now borrows it as a `str` view automatically
  (`{ptr,len,cap}` → `{ptr,len}`); the caller keeps the `String` (no move, no
  double-free), so read-only helpers written over `str` accept both `str` literals
  and `String`s. Applies at plain function-call arguments (not yet methods,
  generic calls, returns, or fields); the reverse isn't implicit (use `string`).
  No leak for `String` variables; a discarded temporary `String` argument leaks as
  before (memory-safe). JIT and AOT agree.
- **String concatenation `+`.** `str`/`String` concatenate with `+` (mixing
  allowed) into a fresh owned `String` (`malloc` + two `memcpy`). Operands are
  borrowed, so variables stay usable; an owned-temporary operand is freed right
  after, so chains (`a + b + c`) and reassignment (`s = s + x`, including in a
  loop) don't leak intermediates. The same temporary-operand cleanup now also
  applies to string comparison, so `string("a") == string("b")` no longer leaks.
  Verified leak-free and double-free free (incl. `s = s + x` evaluation order),
  JIT and AOT agree.
- **Discarded temporaries are dropped.** An owned-heap value produced at statement
  position and thrown away — `box(x);`, `string(x);`, `pop(v);`, or a call whose
  owned result is ignored — is now freed at the end of that statement instead of
  leaking. Only genuine *temporaries* (non-place expressions) are dropped, so a
  value moved into a consuming position (a function argument, field, `Vec`
  element, `let`) is left to its new owner — no double-free. (Still leaking: an
  owned temporary merely *borrowed* inside a larger expression, e.g.
  `len(make_vec())`; bind it to a `let` first.)
- **`Vec` removal: `pop` and `clear`.** `pop(v) -> Option<T>` removes and returns
  the last element (`None` when empty); it shrinks the length first, so ownership
  moves to the caller and the `Vec` won't also drop that element — this is the
  sound way to move a non-`Copy` element out of a `Vec` (indexing still can't).
  `clear(v)` drops every element and resets the length to 0, keeping the capacity.
  Both require a mutable `Vec` and borrow it. Verified leak-free and double-free
  free (incl. `Vec<Box<_>>` / `Vec<String>`), JIT and AOT agree. Diagnostics
  `E0265`–`E0269`.
- **String comparison:** `str` and `String` now compare with `== != < <= > >=`
  by byte (lexicographic) order, via libc `memcmp` (equal prefix → shorter sorts
  first; empty string is smallest). The two types mix freely (`s == "hi"`), both
  operands are borrowed (a comparison never moves/consumes a `String`), and the
  result is `bool`. JIT and AOT agree; no leaks.
- **Owned `String`:** a heap-backed, growable UTF-8 string (`{ptr, len, cap}` ≈
  `Vec<u8>`). `string(s)` copies a `str` onto the heap, `push_str(s, t)` appends
  a `str` (reallocating to `max(cap*2, needed)`), and a `String` owns its buffer
  so Drop frees it. `prints`/`len`/`s[i]` accept both `str` and `String`;
  `str` stays read-only (`s[i] = …` is rejected). Works as a field / payload /
  `Vec<String>` / generic argument; idiomatic (bound) use is leak-free, JIT and
  AOT agree. Diagnostics `E0258`/`E0259`/`E0261`–`E0264`. (Equality, `+`
  concatenation, and `String`→`str` coercion are still future work.)
- **Strings (`str`):** string literals `"..."` (escapes `\n \t \r \0 \\ \"`) of
  type `str` — a read-only `{ptr, len}` view of UTF-8 bytes in static data, so
  `str` is `Copy` and never allocates or drops. `prints(s)` writes the bytes,
  `len(s)` is the byte length, `s[i]` is a bounds-checked byte (`u8`). Works as a
  variable / parameter / field / payload / generic argument. JIT and AOT emit
  byte-identical output (incl. embedded `\0`). Diagnostics `E0003`/`E0004`
  (lexer), `E0052`, `E0256`/`E0257`. (Equality, concatenation, and an owned
  `String` are not built yet.)
- **`Vec<T>` (growable array):** a heap-backed dynamic array. `vec()` creates an
  empty one (element type inferred from the annotation), `push(v, x)` appends
  (reallocating, doubling capacity), `len(v)` is the length, and `v[i]`
  reads/writes an element with a runtime bounds check. A `Vec` owns its buffer:
  Drop frees every live element and then the buffer. `push` requires a mutable
  place and moves the value in; non-`Copy` elements can't be moved out by
  indexing (like slices). Works as a field/payload, nested (`Vec<Vec<T>>`), and
  through generics. Verified leak-free (incl. `Vec<Box<_>>` across reallocs) with
  `leaks` and malloc guards. Diagnostics `E0050`/`E0184`/`E0250`–`E0255`.
  *Known limitation:* a discarded owned-heap **temporary** (an unbound `Box`/`Vec`)
  is not dropped yet (memory-safe leak); bind it to a `let`.
- **Drop completeness:** plugged two leak holes the `Vec` audit surfaced (both
  also affected `Box`, both memory-safe — leaks only, never double-free):
  assigning over a droppable place (`v = …`, `v[i] = …`, `s.f = …`, `*p = …`) now
  **drops the old value first** (and a move-then-reassign keeps the new value
  live), and a by-value `match` now **drops payloads that an arm doesn't bind**
  (`_`, wildcard arms, partially-bound variants).
- **Drop / RAII:** owned heap (`Box`) is now **freed automatically** when it goes
  out of scope — at block end, function end, and on early `return`/`?`. Moved-out
  values aren't dropped (tracked with per-local drop flags, so conditional moves
  are handled), and drop glue recurses into structs/enums/tuples/arrays. Verified
  with `leaks` (0 leaked bytes) and malloc guards (no double-free). Matching a
  *reference* to an enum now rejects binding non-`Copy` payloads by value
  (`E0246`), since moving out of a borrow would double-free.
- **`Box<T>` (heap):** `box(e)` allocates `e` on the heap and returns a
  `Box<T>` (an owned pointer); `*b` reads it. Because a `Box` is a pointer,
  **recursive types** are now expressible (`enum List<T> { Cons(T, Box<List<T>>),
  Nil }`) — by-value recursion is still rejected.
- **Early return & `?`:** `return e;` (and `return;`) exits a function early; the
  `?` operator on an `Option`/`Result` unwraps the success value or early-returns
  the `None`/`Err` (the error type must match the function's). `if cond then e`
  with no `else` is now allowed as a statement (so `if c then return e;` works).
- **Traits:** `trait Name { fn m(&self, …) -> …; }` declares an interface;
  `impl Trait for Type { … }` implements it (checked for conformance). Generic
  functions can be **bounded** — `fn f<T: Trait>(x: T) …` lets the body call
  trait methods on `x`, dispatched statically (monomorphized to the concrete
  `impl` at each call). Trait methods are also callable directly on a concrete
  type. (Self types, default methods, and trait impls on generic types are
  future work.)
- **Methods (`impl` blocks):** `impl Type { fn m(self, …) … }` defines methods,
  called as `recv.method(args)`. Receivers are `self` (by value, moves), `&self`
  (shared borrow), or `&mut self` (mutable borrow), with automatic borrowing of
  the receiver at the call site. Field access auto-derefs through a reference
  (so `self.field` works in a `&self` method). Generic types can have methods
  (`impl Pair<A, B> { … }`), monomorphized per instantiation like generic
  functions. (Trait-based / inherited methods are future work.)
- **Bounds-checked indexing:** array and slice indexing `a[i]` (read and write)
  now checks `0 <= i < len` at runtime and aborts with a panic on an
  out-of-range (or negative) index — memory-safe indexing without GC. (Optimized
  builds elide checks that are provably in range.)
- **Bool literals:** `true` and `false`. (So `Result<i64, bool>` and other
  `bool`-carrying types are now expressible directly.)
- **Generic functions:** `fn name<T, …>(…) -> …` — monomorphized per call. Type
  arguments are **inferred** from the arguments and the expected type (no
  turbofish). Bodies are type-checked once with the type parameters abstract, so
  (lacking trait bounds) they are limited to type-agnostic operations — enough to
  write `Option`/`Result` helpers like `fn unwrap_or<T>(o: Option<T>, d: T) -> T`
  and `fn ok_or<T, E>(o: Option<T>, e: E) -> Result<T, E>`. Generic functions may
  call other (and recursive) generic functions.
- **Generics (enums + structs) + `Option`/`Result`:** generic enums
  `enum Name<T, …> { … }` and generic structs `struct Name<T, …> { … }`,
  monomorphized per concrete instantiation. Type arguments are written
  `Name<T1, …>` and **inferred** at construction from the fields/arguments and
  the expected type (no turbofish). `Option<T>` and `Result<T, E>` are provided
  as a built-in **prelude** (`Some`/`None`, `Ok`/`Err`), and `match` works on
  instantiations. By-value recursive types (which have infinite size) are
  rejected. Generic *functions* are not in yet.
- **Slices:** `&[T]` / `&mut [T]` — a length-carrying view into an array
  (a fat pointer `{ptr, len}`). Borrowing an array yields a slice (`&xs`,
  `&mut xs`), `len(s)` returns its length, and `s[i]` reads/writes through it
  (writes need `&mut [T]`). `&[T]` is `Copy`, `&mut [T]` moves. Lets a function
  take an array of any length (`fn sum(s: &[i64]) -> i64`). No bounds checks yet.
- **Arrays:** fixed-length arrays `[T; N]`, array literals `[e1, …, eN]`, and
  indexing `a[i]` (read and write). Arrays nest (`[[i64; 2]; 2]`) and are `Copy`
  when their element type is (numbers, `bool`); otherwise they move. Indexing is
  **not** bounds-checked yet (a future addition).
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
