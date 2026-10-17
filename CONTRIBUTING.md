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
- [CMake](https://cmake.org/) at least the version in
  `cmake_minimum_required()` at the top of `CMakeLists.txt` (3.10 today). A
  direct `gcc` build also works if you cannot install CMake.

### Build with CMake

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

This builds the `yqdb` static library plus every test executable under `build/`
(e.g. `build/yq_test_basic`).

### Build directly with gcc

The library is a plain static archive — there is nothing to install and no
generated headers. Build the objects, then `ar` them into `libyqdb.a`:

```sh
mkdir -p build-manual && cd build-manual

gcc -std=gnu11 -O2 -Wall -Wextra -I../include -c \
    ../src/yq_enc.c ../src/yq_memblk.c ../src/yq_vfs.c ../src/yq_slice.c \
    ../src/yq_page.c ../src/yq_btree.c ../src/yq_memtable.c ../src/yq_wal.c \
    ../src/yq_mvcc.c ../src/yq_recover.c ../src/yq.c ../src/yq_mempool.c
ar rcs libyqdb.a *.o
```

`libyqdb.a` is the whole engine; link it into any program that includes
`yq.h`:

```sh
gcc -std=gnu11 -I../include your_app.c libyqdb.a -o your_app
```

Two details worth knowing:

- **List the library sources explicitly.** A `src/*.c` glob would also pull in
  the `yq_test_*.c` files, each of which defines its own `main()`.
- **Use `-std=gnu11`, not `-std=c11`.** The tests call POSIX
  `clock_gettime()`/`CLOCK_MONOTONIC`, which strict ISO C11 hides. CMake does
  the same (`C_EXTENSIONS` is on), so a `-std=c11` build of a test file fails
  with *"CLOCK_MONOTONIC undeclared"*.

---

## Running the tests

The suite is driven by CTest:

```sh
ctest --test-dir build --output-on-failure
```

**Every** test executable must pass — the suite grows over time, so rather than
trusting a count in this document, read it from CTest:

```sh
ctest --test-dir build -N     # list the tests and the total
```

CI runs the same suite on Ubuntu (gcc and clang) and Windows (MSVC).

### Running a single test

`-R` takes a regex over the test names, which are stable and descriptive
(`crc_test`, `wal_test`, `apis_contract_test`, …):

```sh
ctest --test-dir build -R crc --output-on-failure   # just crc_test
ctest --test-dir build -R 'wal|mvcc' -V             # a subset, verbose
```

Or invoke the binary directly — each one is a standalone `main()` that prints
its own `ok`/`FAIL` lines and returns non-zero on failure:

```sh
./build/yq_test_wal
echo $?          # 0 = pass
```

### Debugging a failing test

Configure a debug build with symbols (no `-O2`, no `NDEBUG`, so the
`assert()`s inside the library stay live), then run the one test under `gdb`:

```sh
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug -j
gdb --args ./build-debug/yq_test_wal
```

`ctest --test-dir build-debug -R wal --output-on-failure` shows the test's own
diagnostics; add `-V` when you also want the exact command line CTest used.
If the failure looks like memory corruption, build the sanitizer tree once and
then run just that test with LeakSanitizer on:

```sh
./scripts/run_sanitizers.sh build-san                # build; suite runs leak-off
ASAN_OPTIONS=detect_leaks=1 ./build-san/yq_test_wal  # one test, leaks on
```

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

- **Language:** C11 (compiled as `gnu11`; MSVC uses `/std:c11`). Avoid GNU-
  or POSIX-only extensions in code that must build everywhere — `clock_gettime`
  in tests is the one accepted exception.
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

yq-DB is released under the [Apache-2.0 License](LICENSE). Contributions are
accepted under the same terms — **inbound = outbound**:

- You keep the copyright to your work; you license it to the project under
  Apache-2.0, which already grants the patent license and the redistribution
  terms the project needs.
- **No CLA and no copyright assignment** is required. Opening a pull request is
  the sign-off.
- Only contribute code you wrote or that is compatibly licensed. Do not paste in
  GPL/AGPL code, and do not add third-party source to the core (see *Coding
  conventions* above) — `bench/third_party/` is the only exception, and its
  licences must be recorded there.
- There are no per-file licence headers — a single top-level `LICENSE` covers
  the tree — so new files need none.
