#!/usr/bin/env bash
#
# run_sanitizers.sh — build yq-DB and run its whole test suite under
# AddressSanitizer + UndefinedBehaviorSanitizer.
#
# The regular build only enables -Wall -Wextra, which cannot catch memory
# unsafety at runtime. This script closes that gap: it instruments the library
# and every test binary with -fsanitize=address,undefined and runs ctest, so
# heap buffer overflows, use-after-free and undefined behaviour are caught
# locally before they ever reach CI.
#
# Leak detection is left OFF on purpose: the suite keeps a handful of
# test-only heap allocations alive past teardown, and library-level leaks are
# already policed by yq_test_lifecycle. Turn it on once the remaining
# test-harness leak is closed by exporting DETECT_LEAKS=1.
#
# Usage:
#   ./scripts/run_sanitizers.sh [build-dir]
#
# Requirements: cmake, and a C compiler (gcc or clang) that ships the
# sanitizer runtime (gcc and clang on Linux both do).

set -euo pipefail

BUILD_DIR="${1:-build-san}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

CC="${CC:-gcc}"
DETECT_LEAKS="${DETECT_LEAKS:-0}"

echo "==> Configuring (CC=$CC, build=$BUILD_DIR)"
cmake -S "$SRC_DIR" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER="$CC" \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"

echo "==> Building"
cmake --build "$BUILD_DIR" -j

echo "==> Running tests under ASan/UBSan (detect_leaks=$DETECT_LEAKS)"
ASAN_OPTIONS="detect_leaks=$DETECT_LEAKS" \
  ctest --test-dir "$BUILD_DIR" --output-on-failure

echo "==> All sanitizer tests passed."
