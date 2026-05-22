#!/usr/bin/env bash
#
# Golden-test harness for the Kal compiler.
#
#   tests/run_tests.sh            run all tests, diff against expected output
#   tests/run_tests.sh --bless    (re)generate the expected/golden files
#
# Test categories:
#   * SUCCESS cases    -- examples/*.kal must compile+run with exit 0;
#                         their STDOUT is compared to tests/expected/<name>.out
#   * DIAGNOSTIC cases -- tests/diagnostics/*.kal must FAIL (non-zero exit);
#                         their STDERR is compared to tests/diagnostics/<name>.stderr
#
# The compiler binary is taken from $KALC (default: ./build/kalc, relative to
# the repo root). Files are passed to kalc using their repo-relative path so
# that diagnostics print stable, machine-independent paths. The compiler is
# expected to disable ANSI color when its stderr is not a TTY (it is redirected
# to a file here), keeping the golden .stderr files deterministic.

set -u

# --- locate the repo root (parent of this script's directory) ---------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT" || exit 1

# --- locate the compiler binary ---------------------------------------------
KALC="${KALC:-./build/kalc}"
if [ ! -x "$KALC" ]; then
  echo "error: kalc binary not found or not executable at '$KALC'." >&2
  echo "       Build it first:" >&2
  echo "         cmake -S . -B build -DLLVM_DIR=\"\$(llvm-config --cmakedir)\" -DCMAKE_BUILD_TYPE=Release" >&2
  echo "         cmake --build build -j" >&2
  echo "       Or point \$KALC at an existing binary." >&2
  exit 1
fi

# --- parse arguments --------------------------------------------------------
BLESS=0
case "${1:-}" in
  --bless) BLESS=1 ;;
  "")      ;;
  *)       echo "usage: $0 [--bless]" >&2; exit 2 ;;
esac

EXPECTED_DIR="tests/expected"
DIAG_DIR="tests/diagnostics"
mkdir -p "$EXPECTED_DIR"

PASS=0
FAIL=0

# Run a unified diff and print it indented; returns diff's exit status.
show_diff() {
  # $1 = expected file, $2 = actual file, $3 = label
  diff -u "$1" "$2" | sed 's/^/    /'
}

# --- SUCCESS cases: examples/*.kal -> stdout --------------------------------
shopt -s nullglob
for src in examples/*.kal; do
  base="$(basename "${src%.kal}")"
  golden="$EXPECTED_DIR/$base.out"
  actual="$(mktemp)"

  "$KALC" "$src" >"$actual" 2>/dev/null
  status=$?

  if [ "$BLESS" -eq 1 ]; then
    if [ "$status" -ne 0 ]; then
      echo "BLESS WARN  $src exited $status (expected success); blessing stdout anyway"
    fi
    cp "$actual" "$golden"
    echo "BLESSED     $golden"
    rm -f "$actual"
    continue
  fi

  if [ "$status" -ne 0 ]; then
    echo "FAIL        $src (kalc exited $status, expected success)"
    FAIL=$((FAIL + 1))
    rm -f "$actual"
    continue
  fi
  if [ ! -f "$golden" ]; then
    echo "FAIL        $src (no golden file $golden; run --bless)"
    FAIL=$((FAIL + 1))
    rm -f "$actual"
    continue
  fi
  if diff -q "$golden" "$actual" >/dev/null; then
    echo "PASS        $src"
    PASS=$((PASS + 1))
  else
    echo "FAIL        $src (stdout differs from $golden)"
    show_diff "$golden" "$actual"
    FAIL=$((FAIL + 1))
  fi
  rm -f "$actual"
done

# --- AOT cases: examples/*.kal compiled to a native binary ------------------
# `kalc build` must produce a standalone executable whose output matches the
# same golden as the JIT run. (Skipped during --bless; reuses the JIT goldens.)
if [ "$BLESS" -ne 1 ]; then
  for src in examples/*.kal; do
    base="$(basename "${src%.kal}")"
    golden="$EXPECTED_DIR/$base.out"
    bin="$(mktemp)"
    actual="$(mktemp)"

    if ! "$KALC" build "$src" -o "$bin" 2>/dev/null; then
      echo "FAIL        $src (aot) (kalc build failed)"
      FAIL=$((FAIL + 1))
      rm -f "$bin" "$actual"
      continue
    fi
    "$bin" >"$actual" 2>/dev/null
    if [ -f "$golden" ] && diff -q "$golden" "$actual" >/dev/null; then
      echo "PASS        $src (aot)"
      PASS=$((PASS + 1))
    else
      echo "FAIL        $src (aot) (binary output differs from $golden)"
      show_diff "$golden" "$actual"
      FAIL=$((FAIL + 1))
    fi
    rm -f "$bin" "$actual"
  done
fi

# --- DIAGNOSTIC cases: tests/diagnostics/*.kal -> stderr --------------------
for src in "$DIAG_DIR"/*.kal; do
  base="$(basename "${src%.kal}")"
  golden="$DIAG_DIR/$base.stderr"
  actual="$(mktemp)"

  "$KALC" "$src" >/dev/null 2>"$actual"
  status=$?

  if [ "$BLESS" -eq 1 ]; then
    if [ "$status" -eq 0 ]; then
      echo "BLESS WARN  $src compiled successfully (expected failure); blessing stderr anyway"
    fi
    cp "$actual" "$golden"
    echo "BLESSED     $golden"
    rm -f "$actual"
    continue
  fi

  if [ "$status" -eq 0 ]; then
    echo "FAIL        $src (kalc exited 0, expected a compile error)"
    FAIL=$((FAIL + 1))
    rm -f "$actual"
    continue
  fi
  if [ ! -f "$golden" ]; then
    echo "FAIL        $src (no golden file $golden; run --bless)"
    FAIL=$((FAIL + 1))
    rm -f "$actual"
    continue
  fi
  if diff -q "$golden" "$actual" >/dev/null; then
    echo "PASS        $src"
    PASS=$((PASS + 1))
  else
    echo "FAIL        $src (stderr differs from $golden)"
    show_diff "$golden" "$actual"
    FAIL=$((FAIL + 1))
  fi
  rm -f "$actual"
done

# --- summary ----------------------------------------------------------------
if [ "$BLESS" -eq 1 ]; then
  echo "blessed expected files in $EXPECTED_DIR and $DIAG_DIR"
  exit 0
fi

echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
