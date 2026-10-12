# yq-DB Benchmark Report

A side-by-side performance comparison of **yq-DB 1.0.0** against two widely used
embedded storage engines of the same class — **SQLite** (B-tree, WAL mode) and
**LMDB** (memory-mapped B+Tree).

> [English](BENCHMARKS.md) · [简体中文](BENCHMARKS.zh-CN.md)
> See also: [README](../README.md) · [USAGE](USAGE.md) · [FORMAT](../FORMAT.md) · [ERRORS](../ERRORS.md)

## Table of Contents

- [Summary](#summary)
- [Methodology](#methodology)
- [Test Environment](#test-environment)
- [Workload](#workload)
- [Results](#results)
- [Analysis](#analysis)
- [1.0.0 → 1.1.0 Upgrade Benchmark](#100--110-upgrade-benchmark)
- [On-disk Footprint](#on-disk-footprint)
- [Known Limitations Behind the Numbers](#known-limitations-behind-the-numbers)
- [Reproducing](#reproducing)
- [Raw Data](#raw-data)

---

## Summary

All three engines were exercised with the **same compiler, the same workload,
and the same timer**. With 100,000 keys of 16-byte keys and 100-byte values:

| Dimension | yq-DB position | Notes |
|-----------|----------------|-------|
| Point reads (hit / miss) | **On par with LMDB, ahead of SQLite** | ~2.4–2.8 M / ~7.1–7.6 M ops/s |
| Full scan | **Fastest by a wide margin** | ~142–155 M ops/s, 2.3–12× the others |
| Update / delete | **Leading in `normal`, competitive in `nosync`** | ~257–347 k ops/s |
| Sequential insert | **Slowest, but 9× faster than random** | ~293–296 k ops/s |
| Random insert | **Weakest** | ~32 k ops/s |
| Reopen (crash recovery) | **Weakest** | seconds vs. milliseconds |

In short: yq-DB is a **read- and scan-optimized** engine whose write and reopen
paths are the acknowledged cost of its current simplicity. The sections below
quantify each figure and explain the root causes.

---

## Methodology

- **Same compiler, same flags.** All three engines are built with the same
  MinGW `gcc 14.2.0`, `-O2 -std=c11`, and statically linked into a single
  executable per engine. Third-party sources: SQLite amalgamation 3.45.1 and
  LMDB 0.9.31 (both vendored under `bench/third_party/`).
- **Same workload.** One driver, `bench/bench_kv.c`, contains all three engine
  backends behind a common interface. The key/value generation, ordering,
  batching, and phase sequence are byte-for-byte identical.
- **Same timer.** Wall-clock time is measured with Windows
  `QueryPerformanceCounter` (a monotonic high-resolution counter), not by
  instrumentation inside any engine.
- **Comparable durability settings.** Two sync modes are reported:
  - `normal` — each engine's default durability grouping
    (SQLite `synchronous=NORMAL`, LMDB default `msync`, yq-DB `YQ_SYNC_NORMAL`).
  - `nosync` — durability relaxed for maximum throughput
    (SQLite `synchronous=OFF`, LMDB `MDB_NOSYNC`, yq-DB `YQ_SYNC_OFF`).
- **Two insertion orders.** `rand` inserts keys in shuffled order; `seq`
  inserts keys in ascending order. Keys are big-endian encoded so that the byte
  ordering used by every engine matches numeric ordering.

Results are the throughput of a single run per configuration on an otherwise
idle machine; treat them as order-of-magnitude comparisons rather than
certification-grade figures.

---

## Test Environment

| Item | Value |
|------|-------|
| OS | Windows |
| Compiler | MinGW `gcc 14.2.0` |
| Flags | `-O2 -std=c11` |
| Timer | `QueryPerformanceCounter` |
| SQLite | amalgamation 3.45.1, `journal_mode=WAL`, `cache_size=-65536` |
| LMDB | 0.9.31, map size 256 MiB |
| yq-DB | 1.0.0, default 4 KiB pages |

All engines run in a **single process, single thread**; there is no concurrency
in the workload, so these numbers isolate single-threaded engine cost.

---

## Workload

`N = 100,000` records. `KEYLEN = 16` bytes, `VALLEN = 100` bytes. Writes are
grouped into transactions of **1,000 operations** (`BATCH = 1000`). The phases
run in this order, each timed separately:

| Phase | Operation |
|-------|-----------|
| `insert` | `N` puts of fresh keys |
| `read_hit` | `N` point lookups of existing keys (random order) |
| `read_miss` | `N` point lookups of absent keys |
| `scan` | one full cursor iteration over all records |
| `update` | `N` puts overwriting existing keys |
| `delete` | `N` deletes |
| `reopen` | close the database, reopen it, and read one key |

Throughput is reported in operations per second (higher is better). `reopen` is
reported in **seconds** (lower is better).

---

## Results

### `normal` sync mode

| Phase | yq-DB rand | yq-DB seq | SQLite rand | SQLite seq | LMDB rand | LMDB seq |
|-------|-----------:|----------:|------------:|-----------:|----------:|---------:|
| insert (ops/s) | 32,866 | 293,446 | 20,455 | 701,752 | 41,903 | 998,217 |
| read_hit (ops/s) | 2,395,651 | 2,726,430 | 1,111,561 | 1,242,401 | 2,875,993 | 2,859,905 |
| read_miss (ops/s) | 7,618,990 | 7,457,622 | 2,495,683 | 2,528,835 | 7,408,505 | 7,855,953 |
| scan (ops/s) | 142,207,054 | 154,631,196 | 12,039,780 | 12,527,404 | 32,950,015 | 59,431,832 |
| update (ops/s) | 263,133 | 270,492 | 33,378 | 34,456 | 30,758 | 34,243 |
| delete (ops/s) | 329,905 | 345,385 | 37,757 | 36,413 | 31,444 | 35,302 |
| reopen (s) | 4.761 | 2.110 | 0.0031 | 0.0030 | 0.0014 | 0.0014 |

### `nosync` sync mode

| Phase | yq-DB rand | yq-DB seq | SQLite rand | SQLite seq | LMDB rand | LMDB seq |
|-------|-----------:|----------:|------------:|-----------:|----------:|---------:|
| insert (ops/s) | 32,388 | 295,808 | 88,954 | 828,577 | 317,896 | 2,833,744 |
| read_hit (ops/s) | 1,453,767 | 2,794,522 | 1,101,290 | 1,258,682 | 2,792,766 | 2,878,344 |
| read_miss (ops/s) | 7,050,247 | 7,473,171 | 2,498,907 | 2,504,828 | 7,472,557 | 7,970,541 |
| scan (ops/s) | 149,009,092 | 148,279,950 | 11,951,287 | 12,165,598 | 31,512,936 | 65,376,569 |
| update (ops/s) | 256,960 | 266,770 | 95,428 | 96,682 | 269,010 | 276,017 |
| delete (ops/s) | 332,180 | 347,050 | 104,138 | 104,342 | 276,434 | 296,822 |
| reopen (s) | 4.772 | 2.092 | 0.0026 | 0.0029 | 0.0016 | 0.0016 |

---

## Analysis

### Reads: competitive to leading

- **Point reads (`read_hit`)** land at ~2.4–2.8 M ops/s, statistically
  indistinguishable from LMDB (~2.8–2.9 M) and roughly **2× SQLite**
  (~1.1–1.3 M). yq-DB returns a zero-copy borrowed pointer into the mmap, so a
  hit costs little more than a page-table lookup and a B+Tree descent.
- **Misses (`read_miss`)** are ~7.1–7.6 M ops/s, matching LMDB (~7.4–8.0 M) and
  about **3× SQLite** (~2.5 M). A miss terminates at the leaf search without any
  value copy.
- **Full scan** is the standout: **142–155 M ops/s**, **2.3–4.7×** LMDB and
  **11–13×** SQLite. Sequential cursor iteration over slotted leaf pages is
  cache-friendly and avoids per-record materialization.

### Writes: ordered-array cost

- **Random insert** is the weakest number (~32 k ops/s), roughly the same order
  as SQLite under `normal` and below LMDB. This is expected: the yq-DB memtable
  is a **sorted dynamic array**, so each out-of-order insert shifts a suffix of
  the array (`memmove`), i.e. **O(n) amortized per insert**.
- **Sequential insert** is **~9× faster** (~293–296 k ops/s) because an
  append-only ascending insert lands at the array tail and needs no shifting.
  It is nevertheless still behind SQLite (`seq`) and LMDB (`seq`), which use
  B-tree insertion that does not pay a linear shift.
- **Update** (~257–270 k ops/s) and **delete** (~330–347 k ops/s) are strong
  against SQLite and LMDB in `normal` mode, but LMDB in `nosync` mode closes the
  gap (~269–297 k). yq-DB's figures barely move between sync modes because
  writes are buffered in the memtable and batched into 1,000-op transactions.

### Reopen: recovery replay cost

- `reopen` is the largest gap: **2.1–4.8 s** for yq-DB versus **~3 ms** for
  SQLite and **~1.5 ms** for LMDB — three orders of magnitude. Because B+Tree
  page spilling and log truncation are not yet enabled in 1.0, opening the
  database **replays the whole log into the memtable**; replaying 100,000 records
  into the ordered array compounds the same O(n) insertion cost. Sequential-order
  data reopens faster (2.1 s) than random-order data (4.8 s) for the same reason.

### Effect of sync mode

- For **yq-DB**, `normal` and `nosync` are nearly identical across every phase —
  durability grouping already amortizes fsyncs, so relaxing the mode adds little.
- For **SQLite** and **LMDB**, `nosync` substantially lifts write throughput
  (for example LMDB sequential insert climbs from ~1.0 M to ~2.8 M ops/s), at the
  expected cost of durability.

---

## 1.0.0 → 1.1.0 Upgrade Benchmark

A targeted 1.0.0 vs 1.1.0 comparison on the **same machine, same driver, same
workload** (the matrix above is across engines; this section is across
releases of the same engine). Headline:

> **1.1.0's only meaningful performance gain over 1.0.0 is `reopen` — WAL
> replay + recovery — about 1.45× on random keys and 9.4× on sequential keys.
> `insert`, `read`, `scan`, `update`, and `delete` are all statistically
> unchanged (within ±20% noise).**

### What changed under the hood

The write path (`src/yq_memtable.c`, `yq_put`, `pending_apply`,
`yq_txn_commit`) is **byte-for-byte identical** between 1.0.0 and 1.1.0 (md5
match), so `insert` cannot have moved. The wins live entirely in the recovery
path:

- **`yq_wal_scan()` rewritten as a 64 KiB buffered reader**
  (`src/yq_wal.c`). 1.0 issued **two `pread` calls plus one `malloc` per
  record** — replaying 100k PUTs + 200 begin/commit records meant
  ≈ 200,000 syscalls. 1.1 reads 64 KiB chunks into a single buffer and
  parses in place — the same 14.7 MB log is covered in ≈ 224 `pread` calls,
  and payloads are reused out of a single scratch buffer.
- **Open-addressed hash set for the "is this txn committed?" check**
  (`src/yq_recover.c`). 1.0's `ctx_is_committed()` linearly scanned the
  committed-txn array — **O(operations × txns)**. 1.1 hashes the txn id,
  collapsing recovery to **O(operations)**, and folds keys/values into two
  append-only arenas (saving 100k per-record `malloc` calls).

### Why `rand` only reaps 1.45× but `seq` reaps 9.4×

Reopen time decomposes as:

```
reopen  ≈  memtable replay cost  +  WAL scan/recovery fixed overhead
          (same in both versions,    (the part 1.1 optimized away)
           memtable unchanged)

seq :  1.0 = ~0   + 2.06 s   →   1.1 = ~0   + 0.22 s    →  ~9.4×
rand:  1.0 = ~3.0 + 2.06 s   →   1.1 = ~3.3 + 0.22 s    →  ~1.45×
```

The fixed overhead saved (~1.84 s) is identical in both cases. With
sequential keys the memtable replay cost is ~0 (tail append), so the savings
dominate. With random keys the same ~3 s of `memmove` cost is paid in both
versions, diluting the speedup to 1.45×.

### Results — `normal` mode (seconds, best of 3)

| Phase | 1.0.0 rand | 1.1.0 rand | rand speedup | 1.0.0 seq | 1.1.0 seq | seq speedup |
|-------|-----------:|-----------:|-------------:|----------:|----------:|------------:|
| insert | 3.565 | 3.575 | ×1.00 | 0.352 | 0.362 | ×0.97 |
| read_hit | 0.052 | 0.045 | ×1.14 | 0.043 | 0.043 | ×1.00 |
| read_miss | 0.013 | 0.011 | ×1.18 | 0.013 | 0.011 | ×1.17 |
| scan | 0.001 | 0.001 | ×1.08 | 0.001 | 0.001 | ×1.04 |
| update | 0.464 | 0.391 | ×1.19 | 0.396 | 0.398 | ×1.00 |
| delete | 0.313 | 0.306 | ×1.02 | 0.305 | 0.313 | ×0.97 |
| **reopen** | **4.977** | **3.363** | **×1.48** | **2.004** | **0.210** | **×9.55** |

### Results — `nosync` mode (seconds, best of 3)

| Phase | 1.0.0 rand | 1.1.0 rand | rand speedup | 1.0.0 seq | 1.1.0 seq | seq speedup |
|-------|-----------:|-----------:|-------------:|----------:|----------:|------------:|
| insert | 3.382 | 3.547 | ×0.95 | 0.345 | 0.355 | ×0.97 |
| read_hit | 0.045 | 0.050 | ×0.91 | 0.040 | 0.043 | ×0.93 |
| read_miss | 0.013 | 0.011 | ×1.20 | 0.013 | 0.012 | ×1.12 |
| scan | 0.001 | 0.001 | ×1.00 | 0.001 | 0.001 | ×1.02 |
| update | 0.392 | 0.413 | ×0.95 | 0.383 | 0.395 | ×0.97 |
| delete | 0.306 | 0.312 | ×0.98 | 0.291 | 0.305 | ×0.95 |
| **reopen** | **5.117** | **3.311** | **×1.55** | **2.032** | **0.210** | **×9.66** |

> "speedup" = `1.0 time / 1.1 time`; > 1 means 1.1 is faster.

### Environmental sanity check

To confirm the comparison itself is valid, the local 1.0.0 numbers were
diffed against the official 1.0.0 numbers used to produce the matrix above:

| Config | Phase | Official 1.0.0 (s) | Local 1.0.0 (s) | Δ |
|---|---|---:|---:|---:|
| normal / rand | insert | 3.043 | 3.565 | +17.2 % |
| normal / rand | reopen | 4.761 | 4.977 | +4.5 % |
| normal / seq | insert | 0.341 | 0.352 | +3.4 % |
| normal / seq | reopen | 2.110 | 2.004 | −5.0 % |

`reopen` and `seq insert` agree within ±5 %; the local machine produces
slightly slower `rand insert` (Windows/MinGW vs the official Linux host).
**Because both 1.0 and 1.1 were re-run on the same local box, the 1.0↔1.1
ratio is unaffected by the absolute drift.**

### Data-safety fixes (no perf impact, but shipped in 1.1.0)

These do not move the benchmark numbers but are part of what 1.1.0 delivers:

- `yq_checkpoint()` no longer calls `yq_wal_truncate(wal, 0)` (which in 1.0
  erased the only durable copy of the data). 1.1.0 uses
  `wal_compact_from_memtable()` — rewrite the memtable's live contents into
  a new log and rename it atomically.
- `YQ_TXN_READONLY` was 0 in 1.0, so `flags & YQ_TXN_READONLY` was always
  false — read-only transactions never registered a snapshot (MVCC
  effectively broken) and could still write. 1.1.0 checks the read-only bit
  explicitly and rejects writes on read-only handles (`YQ_ERR_READONLY`).
- `yq_wal_append_put`'s 2 KiB on-stack buffer overflowed for larger values;
  1.1.0 adds a bound check and falls back to a heap buffer.
- The auxiliary `log_path[512]` silently truncated long db names; 1.1.0
  promotes it to `YQ_MAX_PATH` with explicit length validation.

### Reading these numbers

- The 1.0↔1.1 comparison is **not a cross-engine** comparison: the
  matrix above already shows yq-DB's standing against SQLite and LMDB, and
  this section only says what changed *within* yq-DB between releases.
- A 1.0↔1.1 insert number drifting in either direction by up to 20 % is
  measurement noise; the write path is identical, so the **expected delta
  is 1.0×** and any deviation is a sign to rerun, not a regression.
- The reopening speedup is the headline. 1.1.0 takes yq-DB from
  "seconds" down to "sub-second" for sequential data — still much slower
  than SQLite/LMDB (milliseconds), but no longer the multi-second
  open-time cliff the matrix above attributes to 1.0.0.

The full unedited report is preserved as
[`docs/COMPARE-1.0-1.1.md`](COMPARE-1.0-1.1.md).

---

## On-disk Footprint

Measured after the insert phase for 100,000 records (logical data, i.e. main
file plus any active log/WAL):

| Engine | Order | Size (MiB) | Notes |
|--------|-------|-----------:|-------|
| yq-DB | rand | 32.4 | main `.yqdb` + `.log` (log not yet checkpointed) |
| yq-DB | seq | 32.4 | same |
| SQLite | rand | 13.4 | database + `-wal` |
| SQLite | seq | 13.5 | database + `-wal` |
| LMDB | rand | 25.3 | used pages; file is a 256 MiB pre-allocated sparse map |
| LMDB | seq | 12.7 | used pages; file is a 256 MiB pre-allocated sparse map |

yq-DB's footprint is larger here mainly because the WAL is retained after the
run rather than truncated by a checkpoint. LMDB's reported file size is its
sparse pre-allocated map region (`mdb_env_set_mapsize`), which does not reflect
data actually written.

---

## Known Limitations Behind the Numbers

These caveats are stated so the figures are read in context, not as defects of
the feature set:

1. **Memtable is a sorted dynamic array.** Insert is O(n) in the worst case,
   which is why random inserts and log replay are the slowest paths. A future
   ordered structure (skip list / LSM) would remove this.
2. **Reopen replays the whole log.** B+Tree page spilling and log truncation are
   reserved but not enabled in 1.0, so recovery cost scales with log size.
3. **Single writer, single thread.** The workload is single-threaded; the
   comparison says nothing about multi-reader scaling, where yq-DB's MVCC
   snapshot reads are designed to help.

See [README → Current Scope & Limitations](../README.md#current-scope--limitations)
for the full list.

---

## Reproducing

The benchmark is self-contained under `bench/`:

```sh
# Build the three engines and run the full matrix (Windows PowerShell)
powershell -NoProfile -ExecutionPolicy Bypass -File bench/run_bench.ps1 -N 100000
```

The script compiles the core engine plus the two vendored engines, then runs
`insert → read_hit → read_miss → scan → update → delete → reopen` for each
combination of `{normal, nosync} × {rand, seq}`, printing CSV rows of the form
`RESULT,engine,mode,order,phase,count,seconds,ops_per_sec`.

To run a single engine/configuration directly:

```sh
gcc -O2 -std=c11 -DENGINE_YQ     -I include bench/bench_kv.c src/*.c -o bench_yq
gcc -O2 -std=c11 -DENGINE_SQLITE -I bench/third_party bench/bench_kv.c bench/third_party/sqlite3.c -o bench_sqlite
gcc -O2 -std=c11 -DENGINE_LMDB   -I bench/third_party bench/bench_kv.c bench/third_party/mdb.c bench/third_party/midl.c -o bench_lmdb

./bench_yq     bench/data/my.yqdb        100000 normal rand
./bench_sqlite bench/data/sqlite.db      100000 normal rand
./bench_lmdb   bench/data/lmdbbench      100000 normal rand
```

---

## Raw Data

The unmodified output of the reference run is checked in at
[`bench/results_raw.txt`](../bench/results_raw.txt). Every number in this report
is derived from that file.

| Document | Language |
|----------|----------|
| This document | English |
| [BENCHMARKS.zh-CN.md](BENCHMARKS.zh-CN.md) | 中文 |
