# Contributing to yq-DB

Thanks for your interest in yq-DB! This document explains how to build, test,
and propose changes so your pull request can be reviewed and merged smoothly.

yq-DB is a lightweight embedded key-value storage engine in pure C11. The
guiding principle is **lighter than SQLite, correctness-first** — keep the core
small and dependency-free, and prove behaviour with tests before merging.

---

## Getting started

### Prerequisites

- A C11 compiler: `gcc` or `clang`.
- [CMake](https://cmake.org/) ≥ 3.10 (optional — a direct `gcc` build also
  works).

### Build with CMake

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

### Build directly with gcc

```sh
gcc -std=c11 -Wall -Wextra -Iinclude src/*.c -o yqdb.a -shared   # or link the objects
```

The CMake build also builds every test executable under `build/` (e.g.
`build/yq_test_basic`).

---

## Running the tests

The suite is driven by CTest:

```sh
ctest --test-dir build --output-on-failure
```

All 15 test executables must pass. CI runs the same suite on Ubuntu (gcc and
clang) and Windows (MSVC).

### Memory safety / undefined behaviour

`scripts/run_sanitizers.sh` builds the library and every test binary with
`-fsanitize=address,undefined` and runs the suite, catching buffer overflows,
use-after-free and UB that the warning-only build misses:

```sh
./scripts/run_sanitizers.sh          # gcc
CC=clang ./scripts/run_sanitizers.sh # clang
```

Leak detection is off by default (the suite keeps a few test-only allocations
alive past teardown; library leaks are already policed by `yq_test_lifecycle`).

---

## Coding conventions

- **Language:** C11, no extensions that break MSVC/clang/gcc portability.
- **Warnings are not optional:** the build uses `-Wall -Wextra` (and `/W4` on
  MSVC). New code should be warning-free.
- **No external dependencies** in the core library. The whole point of yq-DB is
  to stay lighter than SQLite; do not pull in third-party code except under
  `bench/third_party/` for benchmarking.
- **Assertions stay on in tests.** The test executables are compiled with
  `-UNDEBUG` so `assert()` is never compiled away. Never disable assertions to
  "make a test pass" — fix the bug instead.

### Public ABI contract (`include/yq.h`)

The public header is the **only** ABI contract. Before touching it, respect
these rules (they are documented at the top of `include/yq.h`):

1. The core ABI must stay free of SQL / row / column encoding. Those belong in
   optional modules behind `YQ_ERR_NOTSUP`.
2. Public structs always carry a `struct_size` field. **New fields may only be
   appended at the end**, and `struct_size` must be bumped in lockstep.
3. **Error codes are frozen once released** — never renumber or reuse a value.
   Add a new `YQ_ERR_*` at the end instead.
4. Do not expose internal structures. All handles (`yq_db`, `yq_txn`, `yq_cur`)
   are opaque.

### Commit and PR style

- Keep pull requests **small and focused** — one logical change per PR is easier
  to review and less likely to conflict.
- Write PR titles and descriptions in **English**; describe the *why*, not just
  the *what*, and include how you verified the change (build log, test output,
  benchmark numbers).
- Make sure CI is green (build + all tests) before requesting review.
- Match the surrounding code style (tabs/spaces already used by the file;
  2-space indent and K&R braces are the existing convention).

---

## Reporting issues

Open an issue with:

- What you did and what you expected vs. observed.
- The yq-DB version (`yq_version()`) and your platform/compiler.
- A minimal reproduction if possible. For suspected data corruption or crashes,
  include the database files (`*.yqdb`, `*.yqdb.log`, `*.yqdb.shm`,
  `*.yqdb.lock`) and, if relevant, the output of
  `scripts/run_sanitizers.sh`.

---

## License

By contributing, you agree that your contributions will be licensed under the
[Apache-2.0 License](LICENSE) that covers this project.
