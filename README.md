# yq-DB

[![Language](https://img.shields.io/badge/language-C11-blue.svg)](#requirements)
[![Version](https://img.shields.io/badge/version-1.0.0-green.svg)](#versioning)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)
[![Dependencies](https://img.shields.io/badge/dependencies-none-brightgreen.svg)](#requirements)

**A lightweight embedded key-value storage engine written in pure C11.**

yq-DB is a single-file, dependency-free KV storage engine for embedded use. It
exposes a plain C ABI for key-value reads/writes, transactions, and cursors. It
does not speak SQL — the goal is to be **lighter than SQLite** while keeping a
correctness-first design.

> **English** · [简体中文](README.zh-CN.md)

---

## Table of Contents

- [Why yq-DB](#why-yq-db)
- [Features](#features)
- [Architecture at a Glance](#architecture-at-a-glance)
- [Requirements](#requirements)
- [Building](#building)
- [Quick Start](#quick-start)
- [API Overview](#api-overview)
- [Data Files](#data-files)
- [Configuration Defaults](#configuration-defaults)
- [Testing](#testing)
- [Current Scope & Limitations](#current-scope--limitations)
- [Versioning & Compatibility](#versioning--compatibility)
- [Documentation](#documentation)
- [License](#license)

---

## Why yq-DB

- **Lighter than SQLite.** No third-party dependencies, no build system
  requirements beyond a C11 compiler, no separate server process. The core
  engine is roughly 3000 lines of C.
- **Single file.** One `.yqdb` file holds all data. Auxiliary files exist only for
  the log, shared-memory reader slots, and the cross-process lock.
- **Embedded.** Linked into your process. No network, no daemon, no background
  threads.
- **Correctness first.** Every page and every log record carries a CRC32C. On
  corruption the engine refuses to guess — it degrades to a read-only protection
  mode instead of silently "repairing" data.

## Features

| Area | Support |
|------|---------|
| Copy-on-write B+Tree | Slotted pages, leaf/internal node types, configurable page size |
| MVCC snapshot reads | Read-only transactions take a `(txn_id, root_page)` snapshot without blocking writers |
| Write-ahead logging | Commit-first WAL, CRC32C on header and payload, torn-write tolerant |
| Transactions | Single-writer / multi-reader, atomic and durable commits |
| Staged writes | Writes buffer in a private queue and apply atomically on commit |
| Crash recovery | Log is replayed on open; only committed transactions survive |
| Dual meta pages | Page 0 / page 1 rotate by `txn_id` / `meta_seq` for crash safety |
| Zero-copy reads | Values returned as borrowed pointers into the mmap |
| Cursors | Bidirectional, ordered iteration with seek / seek_le / seek_exact |
| Portability | Windows (Win32) and POSIX backends behind a single VFS interface |

## Architecture at a Glance

```
        Application
            │  C ABI (yq.h)
            ▼
   ┌──────────────────┐
   │   yq / yq_txn    │  public API, staged transactions
   └────────┬─────────┘
            │
   ┌────────▼─────────┐   ┌──────────────┐   ┌──────────────┐
   │    memtable      │   │  WAL + recover│   │  MVCC / shm  │
   │  (ordered KV)    │   │  (durability) │   │  (snapshots) │
   └────────┬─────────┘   └──────────────┘   └──────────────┘
            │
   ┌────────▼─────────┐
   │  COW B+Tree pages│
   └────────┬─────────┘
            │
   ┌────────▼─────────┐
   │   VFS (Win32 /   │
   │      POSIX)      │
   └──────────────────┘
```

See [FORMAT.md](FORMAT.md) for the on-disk layout, page structure, log record
format, and the recovery algorithm.

## Requirements

- A C11 compiler: **GCC**, **Clang**, or **MSVC**.
- **CMake ≥ 3.10** (optional — a direct `gcc` build works too).
- No third-party libraries. On POSIX there is no `-lpthread` requirement; the
  library does not use threads internally.

## Building

### With CMake

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

### With gcc directly

```sh
gcc -std=c11 -Wall -Wextra -Iinclude -O2 -c src/*.c
ar rcs libyqdb.a *.o
```

### Linking into your project

```sh
gcc -std=c11 -I path/to/yqdb/include your_app.c path/to/yqdb/libyqdb.a -o your_app
```

## Quick Start

```c
#include <stdio.h>
#include <string.h>
#include "yq.h"

int main(void) {
    yq_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(yq_opts);   /* required */
    opts.flags       = YQ_OPEN_CREATE;    /* create if absent */

    yq_db *db = NULL;
    if (yq_open("my.yqdb", &opts, &db) != YQ_OK) {
        return 1;
    }

    /* Write */
    yq_txn *txn = NULL;
    yq_txn_begin(db, YQ_TXN_READWRITE, &txn);
    yq_slice k = { "hello", 5 };
    yq_slice v = { "world", 5 };
    yq_put(txn, k, v, YQ_PUT_UPSERT);
    yq_txn_commit(txn);

    /* Read */
    yq_txn_begin(db, YQ_TXN_READONLY, &txn);
    yq_slice out = { 0 };
    if (yq_get(txn, k, &out) == YQ_OK) {
        printf("hello = %.*s\n", (int)out.size, (const char *)out.data);
    }
    yq_txn_commit(txn);

    yq_close(db);
    return 0;
}
```

More complete examples (cursors, error handling, checkpointing, concurrency) are
in the [Usage Guide](docs/USAGE.md).

## API Overview

**Lifecycle**

| Function | Description |
|----------|-------------|
| `yq_open` | Open a database (runs crash recovery) |
| `yq_close` | Roll back active txns, checkpoint, sync, unmap |
| `yq_version` | Return `major.minor.patch` |
| `yq_strerror` | Human-readable string for an error code |
| `yq_last_io_error` | Last system I/O error (`errno` / `GetLastError`) |

**Transactions**

| Function | Description |
|----------|-------------|
| `yq_txn_begin` | Start a `READONLY` or `READWRITE` transaction |
| `yq_txn_commit` | Commit (durability follows `sync_mode`) |
| `yq_txn_abort` | Discard all changes and release the snapshot |

**Key-value operations**

| Function | Description |
|----------|-------------|
| `yq_put` | Insert / update (`UPSERT` or `NOOVERWRITE`) |
| `yq_del` | Delete (idempotent) |
| `yq_get` | Point lookup (zero-copy borrowed pointer) |

**Cursors**

| Function | Description |
|----------|-------------|
| `yq_cur_open` / `yq_cur_close` | Create / destroy a cursor bound to a txn |
| `yq_cur_first` / `yq_cur_last` | Position at the first / last record |
| `yq_cur_seek` / `yq_cur_seek_le` / `yq_cur_seek_exact` | Position by key |
| `yq_cur_next` / `yq_cur_prev` | Move one record forward / backward |
| `yq_cur_valid` / `yq_cur_key` / `yq_cur_val` | Inspect the current record |

**Maintenance**

| Function | Description |
|----------|-------------|
| `yq_checkpoint` | Merge memtable into the B+Tree and truncate the log |
| `yq_sync` | Flush memtable and log to stable storage |
| `yq_db_stat` | Read engine statistics |

Every function returning `int` returns `YQ_OK` (0) on success. All other codes
are documented in [ERRORS.md](ERRORS.md). `yq_get` and cursor accessors return
**borrowed pointers** valid only within the documented lifetime — copy the bytes
if you need to keep them.

## Data Files

The `path` passed to `yq_open` **is the database file itself**. Three auxiliary
files live beside it:

| File | Purpose | Safe to delete? |
|------|---------|-----------------|
| `path` | Main database (meta pages + B+Tree pages) | No |
| `path.log` | Write-ahead log (WAL) | No — it holds not-yet-flushed data |
| `path.shm` | Reader slot table (runtime only) | Yes — rebuilt on open |
| `path.lock` | Cross-process writer lock | Yes |

By convention, yq-DB database files use the `.yqdb` extension (for example
`my.yqdb`), which yields `my.yqdb.log`, `my.yqdb.shm`, and `my.yqdb.lock` as the
auxiliary files.

The exact on-disk layout is specified in [FORMAT.md](FORMAT.md).

## Configuration Defaults

All fields of `yq_opts` may be zero-initialized; a zero value selects the default
unless noted otherwise.

| Field | Default | Notes |
|-------|---------|-------|
| `page_size` | 4096 | Power of two in `[4096, 65536]` |
| `sync_mode` | `YQ_SYNC_NORMAL` | Group commit; `YQ_SYNC_FULL` syncs every commit |
| `map_size` | 1 GiB | mmap region size |
| `memtable_bytes` | 64 MiB | Checkpoint threshold |
| `log_bytes` | 256 MiB | Log checkpoint threshold |
| `max_readers` | 126 | Concurrent reader slots (hard cap 65535) |
| `lock_timeout_ms` | 0 | 0 = do not wait; `>0` waits for the write lock |

## Testing

```sh
# CMake
ctest --test-dir build --output-on-failure

# or build and run directly
gcc -std=c11 -Iinclude src/yq_test_basic.c src/*.o -o yq_test_basic
gcc -std=c11 -Iinclude src/yq_test_integration.c src/*.o -o yq_test_integration
```

| Test | Coverage |
|------|----------|
| `yq_test_basic` | varint codec, slice comparison, CRC32C, memory-block allocator |
| `yq_test_integration` | version / error codes, open·close, put·get·del, overwrite protection, txn rollback, cursor iteration, checkpoint, concurrent readers, batch throughput, statistics |

All 12 integration tests currently pass.

## Current Scope & Limitations

This is the **first stable 1.0 release**. Please note the following boundaries:

1. **Durability path.** Persistence is currently provided by "commit writes the
   WAL → open replays committed transactions into the memtable". B+Tree page
   spilling and log truncation are reserved capabilities that are not yet
   enabled. As a result:
   - Data is recoverable from `path.log` after a close;
   - `path.log` grows while the process runs without a checkpoint.
2. **Single writer.** At most one read-write transaction exists at a time,
   enforced across processes by `path.lock`; read-only transactions run
   concurrently up to `max_readers`.
3. **Data size.** The active dataset is bounded by `memtable_bytes` / `map_size`
   (64 MiB / 1 GiB by default).
4. **No SQL.** Only KV primitives — no query language, secondary indexes, or
   table schemas.

None of these affect the correctness of the 1.0 feature set; they are directions
for later releases.

## Versioning & Compatibility

- **Library version** follows `YQ_VERSION_MAJOR.MINOR.PATCH` (currently
  `1.0.0`) and is queryable at runtime via `yq_version`.
- **On-disk format version** is `format_version = 1` (see `yq_stat`). It is
  incremented on breaking changes; an unknown newer format is rejected with
  `YQ_ERR_VERSION`.
- **Error codes** are frozen once released — never renumbered or reused.
- **ABI**: public structs may only gain fields at the end, and `struct_size`
  must be updated in lockstep.

## Documentation

| Document | Language | Contents |
|----------|----------|----------|
| [README.zh-CN.md](README.zh-CN.md) | 中文 | This document, in Simplified Chinese |
| [docs/USAGE.md](docs/USAGE.md) | English | Usage guide: opening, transactions, cursors, tuning, troubleshooting |
| [docs/USAGE.zh-CN.md](docs/USAGE.zh-CN.md) | 中文 | Usage guide, in Simplified Chinese |
| [docs/BENCHMARKS.md](docs/BENCHMARKS.md) | English | Benchmark report vs. SQLite and LMDB |
| [docs/BENCHMARKS.zh-CN.md](docs/BENCHMARKS.zh-CN.md) | 中文 | Benchmark report, in Simplified Chinese |
| [FORMAT.md](FORMAT.md) | 中文 | On-disk format specification (v1) |
| [ERRORS.md](ERRORS.md) | 中文 | Error code semantics and retry strategy |
| [include/yq.h](include/yq.h) | — | The authoritative public ABI contract |

## License

Released under the [Apache License 2.0](LICENSE).

```
Copyright 2026 YQTeam

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0
```
