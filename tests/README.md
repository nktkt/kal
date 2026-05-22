# Kal tests

A small golden-test ("snapshot") harness for the Kal compiler.

It runs the compiler over two kinds of inputs and compares the result against
committed golden files:

| Category    | Inputs                     | What's compared | Golden file                        |
|-------------|----------------------------|-----------------|------------------------------------|
| Success     | `examples/*.kal`           | **STDOUT**, exit 0 | `tests/expected/<name>.out`     |
| Diagnostics | `tests/diagnostics/*.kal`  | **STDERR**, non-zero exit | `tests/diagnostics/<name>.stderr` |

The diagnostic inputs are intentionally invalid programs that must fail to
compile (an undefined variable, an unclosed paren, a wrong argument count, and
an illegal character).

## Build the compiler

The harness needs the `kalc` binary. Build it from the repo root (requires
**LLVM 22**, **CMake 3.20+**, and a C++17 compiler):

```sh
cmake -S . -B build -DLLVM_DIR="$(llvm-config --cmakedir)" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

This produces `build/kalc`.

## Run the tests

From anywhere (the script locates the repo root itself):

```sh
bash tests/run_tests.sh
```

It prints a `PASS`/`FAIL` line per test and a final `N passed, M failed`
summary, and exits non-zero if any test failed.

By default it uses `./build/kalc`. To test a binary elsewhere, set `KALC`:

```sh
KALC=/path/to/kalc bash tests/run_tests.sh
```

## Regenerate (bless) the expected output

`tests/expected/*.out` and `tests/diagnostics/*.stderr` are **generated/blessed
artifacts**, not hand-written. After building the compiler for the first time,
or whenever you intentionally change the compiler's output, regenerate them:

```sh
bash tests/run_tests.sh --bless
```

This (re)creates every golden file from the current `kalc`'s actual output
(creating `tests/expected/` if needed). Review the diff before committing — a
blessed change means the observable behaviour of the compiler changed.

## Notes on determinism

* Each `.kal` file is passed to `kalc` by its repo-relative path, so any path
  printed in a diagnostic is stable across machines.
* `kalc` disables ANSI color when its stderr is not a TTY (it is redirected to a
  file here), so the golden `.stderr` files contain no color escape codes.
