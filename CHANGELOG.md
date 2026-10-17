# Changelog

All notable changes to yq-DB are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Each entry below is anchored to a git tag. The 1.0.0 → 1.1.0 performance and
correctness delta is measured in [`docs/COMPARE-1.0-1.1.md`](docs/COMPARE-1.0-1.1.md).

---

## [1.1.0] - 2026-10-09

### Changed

- **WAL scan / recovery is block-buffered.** `yq_wal_scan()` was rewritten to
  read the log in 64 KiB blocks and parse in place, instead of issuing two
  `pread`s (header + payload) plus a `malloc` per record. Over a ~14.7 MB log
  this cuts the syscalls from roughly 200k to ~224.
- **Committed-transaction detection during recovery is O(operations) instead of
  O(operations × transactions).** `src/yq_recover.c` replaced the linear
  `ctx_is_committed()` scan with an open-addressing hash set and consolidated
  key/value storage into two append-only arenas, removing ~100k per-record
  `malloc`s.
- **`yq_checkpoint()` no longer truncates the WAL to zero.** It now rewrites the
  live memtable contents into a fresh WAL and atomically renames it. The old
  behaviour discarded the only durability copy on disk.

### Fixed

- **Read-only transactions now actually register an MVCC snapshot.** `YQ_TXN_READONLY`
  was defined as `0`, so `flags & YQ_TXN_READONLY` was always false: snapshots
  were never taken and a "read-only" transaction could still write. The read/write
  bit is now checked explicitly.
- **A read-only database handle can no longer open a read-write transaction and
  write to disk.** Such attempts are now rejected with `YQ_ERR_READONLY`.
- **WAL append no longer overflows a fixed 2 KB stack buffer.** `yq_wal_append_put`
  added bounds checks with a heap fallback for larger values.
- **Auxiliary file paths no longer silently truncate long database names.** The
  512-byte `log_path` buffer was replaced by `YQ_MAX_PATH` with an explicit length
  check (see `yq_open`).

### Performance

- **Reopen** (WAL replay + recovery) is **~1.45× faster on random keys and
  ~9.4× faster on sequential keys** (same-machine comparison). Insert / read /
  scan / update / delete paths are unchanged — the write path
  (`src/yq_memtable.c`) is byte-for-byte identical to 1.0.0.

---

## [1.0.0] - 2026-10-07

Initial public release.

- Embedded, dependency-free key-value storage engine written in pure C11.
- Core engine: copy-on-write B+Tree, write-ahead log (WAL), and MVCC.
- Transactions (single writer, multi-reader), bidirectional cursors, checkpoints,
  and runtime statistics.
- Stable public C ABI: frozen error codes, versioned on-disk format
  (`format_version = 1`), and append-only `struct_size`-based struct growth.
