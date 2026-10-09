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
