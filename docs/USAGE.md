# yq-DB Usage Guide

A practical, task-oriented guide to embedding yq-DB in a C application.

> [English](USAGE.md) · [简体中文](USAGE.zh-CN.md)
> See also: [README](../README.md) · [FORMAT](../FORMAT.md) · [ERRORS](../ERRORS.md) · [yq.h](../include/yq.h)

## Table of Contents

- [1. Linking the library](#1-linking-the-library)
- [2. Opening a database](#2-opening-a-database)
- [3. Basic reads and writes](#3-basic-reads-and-writes)
- [4. Transactions and durability](#4-transactions-and-durability)
- [5. Handling borrowed pointers](#5-handling-borrowed-pointers)
- [6. Cursors and iteration](#6-cursors-and-iteration)
- [7. Concurrency](#7-concurrency)
- [8. Maintenance: checkpoint, sync, stats](#8-maintenance-checkpoint-sync-stats)
- [9. Error handling patterns](#9-error-handling-patterns)
- [10. Tuning](#10-tuning)
- [11. Crash recovery and backups](#11-crash-recovery-and-backups)
- [12. Complete example](#12-complete-example)
- [13. Troubleshooting](#13-troubleshooting)

---

## 1. Linking the library

Build the static library once:

```sh
gcc -std=c11 -Wall -Wextra -Iinclude -O2 -c src/*.c
ar rcs libyqdb.a *.o
```

Then compile your application against it:

```sh
gcc -std=c11 -I path/to/yqdb/include your_app.c path/to/yqdb/libyqdb.a -o your_app
```

Only `yq.h` needs to be included. All handles are opaque; never `sizeof` or
dereference them.

```c
#include "yq.h"
```

---

## 2. Opening a database

Always zero-initialize `yq_opts` and set `struct_size` first. Every other field
left at `0` selects the library default.

```c
yq_opts opts;
memset(&opts, 0, sizeof(opts));
opts.struct_size = sizeof(yq_opts);

yq_db *db = NULL;
int rc = yq_open("app.yqdb", &opts, &db);
if (rc != YQ_OK) {
    fprintf(stderr, "open failed: %s\n", yq_strerror(rc));
    return rc;
}
```

### Open flags

| Flag | Effect |
|------|--------|
| `YQ_OPEN_CREATE` | Create the file if it does not exist |
| `YQ_OPEN_EXCL` | Combined with `CREATE`; fail with `YQ_ERR_EXISTS` if it exists |
| `YQ_OPEN_READONLY` | Never modify the main DB file (still writes `path.shm` for reader slots) |
| `YQ_OPEN_NOSYNC` | Ignore all durability waits (discardable data only) |
| `YQ_OPEN_IMMUTABLE` | Skip locks and reader registration; caller promises no concurrent writer |

> `YQ_OPEN_IMMUTABLE` is a promise, not a convenience. If another process writes
> while you hold an immutable handle, data corruption is the caller's
> responsibility. Use it only for read-only media or single-process offline
> analysis.

Typical combinations:

```c
/* create-or-open, read-write */
opts.flags = YQ_OPEN_CREATE;

/* open existing read-only */
opts.flags = YQ_OPEN_READONLY;

/* create only if new, else fail */
opts.flags = YQ_OPEN_CREATE | YQ_OPEN_EXCL;
```

### Closing

```c
yq_close(db);   /* NULL is allowed and returns YQ_OK */
```

`yq_close` rolls back any active transactions, runs a checkpoint, syncs, and
releases the mapping. It does **not** wait for readers in other processes; if
another process still holds a snapshot, the checkpoint may be skipped and the log
is left untruncated — this is expected and still returns `YQ_OK`.

---

## 3. Basic reads and writes

All writes must happen inside a read-write transaction.

```c
yq_txn *txn = NULL;
yq_txn_begin(db, YQ_TXN_READWRITE, &txn);

yq_slice k = { "user:1001", 9 };
yq_slice v = { "Ada", 3 };

int rc = yq_put(txn, k, v, YQ_PUT_UPSERT);   /* insert or overwrite */
if (rc != YQ_OK) {
    yq_txn_abort(txn);
    return rc;
}

yq_txn_commit(txn);
```

### Upsert vs. no-overwrite

| Mode | Behavior when the key exists |
|------|------------------------------|
| `YQ_PUT_UPSERT` | Overwrite |
| `YQ_PUT_NOOVERWRITE` | Return `YQ_ERR_EXISTS`, no change |

### Delete

```c
yq_txn_begin(db, YQ_TXN_READWRITE, &txn);
yq_del(txn, k);          /* idempotent: deleting a missing key returns YQ_OK */
yq_txn_commit(txn);
```

### Point lookup

```c
yq_txn_begin(db, YQ_TXN_READONLY, &txn);

yq_slice out = { 0 };
rc = yq_get(txn, k, &out);
if (rc == YQ_OK) {
    /* out.data points into the mmap; out.size bytes are valid now */
} else if (rc == YQ_ERR_NOTFOUND) {
    /* normal "absent" case, not an error */
}

yq_txn_commit(txn);
```

### Size limits

| Item | Limit |
|------|-------|
| Key | `key.size <= 1024` bytes |
| Value | `val.size <= 1 GiB` |

Exceeding a limit returns `YQ_ERR_TOOBIG`.

---

## 4. Transactions and durability

### Read-only vs. read-write

```c
yq_txn_begin(db, YQ_TXN_READONLY,  &rtxn);  /* snapshot, never blocks writers */
yq_txn_begin(db, YQ_TXN_READWRITE, &wtxn);  /* needs the write lock */
```

A read-write `begin` returns `YQ_ERR_BUSY` when another process holds the write
lock and `lock_timeout_ms` is `0`.

### Commit and abort

```c
yq_txn_commit(txn);   /* durable per sync_mode */
yq_txn_abort(txn);    /* discard everything, release snapshot */
```

After either call the handle is dead; reusing it returns `YQ_ERR_TXN_CLOSED`.
Committing a read-only transaction simply releases its snapshot.

### Sync modes

Set `opts.sync_mode` at open time. It governs how much a commit waits for the
data to reach stable storage.

| Mode | Meaning | Crash exposure |
|------|---------|----------------|
| `YQ_SYNC_OFF` | Do not wait for disk | Last commits may be lost |
| `YQ_SYNC_NORMAL` | Group commit (default) | Durable; several txns may share one flush |
| `YQ_SYNC_FULL` | Flush before every commit returns | Durable, slowest |

If you opened with `YQ_SYNC_OFF`, `yq_sync` is your manual durability point.

### Recommended write pattern

```c
yq_txn *txn = NULL;
if (yq_txn_begin(db, YQ_TXN_READWRITE, &txn) != YQ_OK) {
    return -1;
}
/* ... one or more yq_put / yq_del ... */
if (yq_txn_commit(txn) != YQ_OK) {
    /* commit failed: changes were not applied */
    return -1;
}
```

Group related writes into one transaction — it is both faster and atomic.

---

## 5. Handling borrowed pointers

`yq_get` and the cursor accessors return **borrowed** pointers into internal
memory. They are valid only within a documented window:

| Context | Valid until |
|---------|-------------|
| Read-only transaction | That transaction commits or aborts |
| Read-write transaction | The next `yq_put` / `yq_del` in the same transaction |
| Cursor | The next cursor move or transaction end |
| Across transactions or threads | Never |

To keep the bytes, copy them:

```c
yq_slice out;
if (yq_get(txn, k, &out) == YQ_OK) {
    char *copy = malloc(out.size);
    memcpy(copy, out.data, out.size);
    /* use copy after txn ends, then free(copy) */
}
```

---

## 6. Cursors and iteration

A cursor is bound to one transaction and iterates in `memcmp` key order. A fresh
cursor is *invalid* until you position it.

```c
yq_cur *cur = NULL;
yq_cur_open(txn, &cur);

for (int rc = yq_cur_first(cur); rc == YQ_OK; rc = yq_cur_next(cur)) {
    yq_slice ck, cv;
    yq_cur_key(cur, &ck);
    yq_cur_val(cur, &cv);
    printf("%.*s = %.*s\n",
           (int)ck.size, (const char *)ck.data,
           (int)cv.size, (const char *)cv.data);
}

yq_cur_close(cur);
```

### Positioning calls

| Call | Positions at |
|------|--------------|
| `yq_cur_first` / `yq_cur_last` | First / last record |
| `yq_cur_seek(cur, key)` | First record with `key >= target` |
| `yq_cur_seek_le(cur, key)` | Last record with `key <= target` |
| `yq_cur_seek_exact(cur, key)` | The record equal to `key` |

When no matching record exists, the cursor becomes invalid and the call returns
`YQ_ERR_NOTFOUND` — this is normal, not a failure.

### Range scan

```c
yq_slice lo = { "user:", 5 };
yq_slice hi = { "user;", 5 };   /* ';' is the successor of ':' */

yq_cur *cur = NULL;
yq_cur_open(txn, &cur);
for (int rc = yq_cur_seek(cur, lo); rc == YQ_OK; rc = yq_cur_next(cur)) {
    yq_slice ck;
    yq_cur_key(cur, &ck);
    if (ck.size >= hi.size && memcmp(ck.data, hi.data, hi.size) >= 0) break;
    /* process record */
}
yq_cur_close(cur);
```

### Checking validity

```c
if (yq_cur_valid(cur)) {
    yq_slice key;
    yq_cur_key(cur, &key);
}
```

`yq_cur_key` / `yq_cur_val` on an invalid cursor return `YQ_ERR_CURSOR`.

---

## 7. Concurrency

yq-DB follows a **single-writer, multi-reader** model.

- **Readers** take a consistent snapshot with no lock and never block the writer.
  Reader count is capped by `max_readers` (default 126); exhausting slots returns
  `YQ_ERR_READER_FULL`, which requires reopening with a larger value.
- **Writers** are serialized. Within one process, only one read-write transaction
  may be active per thread; across processes, `path.lock` enforces a single
  writer.

### Waiting instead of failing

By default a second writer gets `YQ_ERR_BUSY` immediately. To wait instead:

```c
opts.lock_timeout_ms = 5000;   /* wait up to 5s for the write lock */
```

A timeout then returns `YQ_ERR_TIMEOUT`.

### Long read snapshots

A long-lived read-only transaction keeps its snapshot pinned, which can delay
reclamation. Keep read transactions as short as your workload allows.

---

## 8. Maintenance: checkpoint, sync, stats

```c
yq_checkpoint(db);        /* merge memtable into B+Tree, truncate log */
yq_sync(db);              /* flush memtable + log to stable storage */
```

- Call `yq_checkpoint` periodically in long-running write workloads to bound log
  growth. It is blocking and returns `YQ_ERR_BUSY` if the write lock is held.
- `yq_sync` flushes without reorganizing pages; in `YQ_SYNC_OFF` mode it is your
  only manual durability point.

### Reading statistics

```c
yq_stat st;
memset(&st, 0, sizeof(st));
st.struct_size = sizeof(yq_stat);
yq_db_stat(db, &st);

printf("format=%u txn=%llu pages=%llu log=%llu mem=%llu readers=%u/%u\n",
       st.format_version,
       (unsigned long long)st.txn_id,
       (unsigned long long)st.npages,
       (unsigned long long)st.log_bytes,
       (unsigned long long)st.memtable_bytes,
       st.active_readers, st.max_readers);
```

Use `log_bytes` and `memtable_bytes` to decide when to checkpoint.

---

## 9. Error handling patterns

`0` (`YQ_OK`) means success; anything else is a failure. Never treat `YQ_OK`
loosely — compare explicitly.

### Normal, non-error results

- `YQ_ERR_NOTFOUND` from `yq_get` / cursor seek — an ordinary "absent" branch.
- `YQ_ERR_EXISTS` from `YQ_PUT_NOOVERWRITE` — an expected conflict.

Do not log these as faults.

### Retryable errors

`YQ_ERR_BUSY`, `YQ_ERR_TIMEOUT`, `YQ_ERR_NOMEM`, `YQ_ERR_IO`, `YQ_ERR_NOSPACE`
may succeed on retry with backoff. Better: use `lock_timeout_ms` so the library
waits internally for the write lock rather than polling.

```c
for (int attempt = 0; attempt < 5; ++attempt) {
    rc = yq_txn_begin(db, YQ_TXN_READWRITE, &txn);
    if (rc == YQ_OK) break;
    if (rc != YQ_ERR_BUSY && rc != YQ_ERR_TIMEOUT) break;
    Sleep(1 << attempt);   /* 1, 2, 4, 8, 16 ms; usleep on POSIX */
}
```

### Corruption: stop writing

`YQ_ERR_CORRUPT`, `YQ_ERR_VERSION`, and `YQ_ERR_PANIC` put the database into a
**read-only protection mode**: writes fail, reads still work for export, and no
automatic repair happens.

Recommended response:

1. Stop all write paths.
2. Copy the `.yqdb` and `.log` files aside to preserve the evidence.
3. Export what you can through a read-only handle.
4. Report with `yq_db_stat` output and `yq_last_io_error`.
5. Never patch in place and continue.

See [ERRORS.md](../ERRORS.md) for the full classification.

```c
const char *msg = yq_strerror(rc);   /* static string, do not free */
int sys = yq_last_io_error();        /* meaningful for YQ_ERR_IO */
```

---

## 10. Tuning

| Field | Default | Tune when |
|-------|---------|-----------|
| `page_size` | 4096 | Larger pages help sequential/range throughput |
| `map_size` | 1 GiB | Increase if you hit `YQ_ERR_MAP_FULL` |
| `memtable_bytes` | 64 MiB | Raise for larger in-memory working sets |
| `log_bytes` | 256 MiB | Raise to checkpoint less often |
| `max_readers` | 126 | Raise if you hit `YQ_ERR_READER_FULL` |
| `lock_timeout_ms` | 0 | Set `>0` to wait for the writer instead of failing |

```c
opts.page_size      = 8192;
opts.map_size       = 4ULL * 1024 * 1024 * 1024;
opts.memtable_bytes = 256ULL * 1024 * 1024;
opts.max_readers    = 512;
opts.lock_timeout_ms = 3000;
opts.sync_mode      = YQ_SYNC_NORMAL;
```

---

## 11. Crash recovery and backups

### Recovery is automatic

Opening a database scans the WAL and replays only **committed** transactions into
the memtable. Uncommitted transactions are discarded. Recovery runs while holding
the write lock and needs no manual step.

### Backup

Because durability runs through `path.log`, a correct backup must include both
files. The safest approach is to close the database (which checkpoints and syncs)
and then copy:

```text
app.yqdb
app.yqdb.log
```

`app.yqdb.shm` and `app.yqdb.lock` are runtime-only and need not be copied.

---

## 12. Complete example

```c
#include <stdio.h>
#include <string.h>
#include "yq.h"

int main(void) {
    yq_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size     = sizeof(yq_opts);
    opts.flags           = YQ_OPEN_CREATE;
    opts.lock_timeout_ms = 2000;

    yq_db *db = NULL;
    int rc = yq_open("demo.yqdb", &opts, &db);
    if (rc != YQ_OK) {
        fprintf(stderr, "open: %s\n", yq_strerror(rc));
        return 1;
    }

    /* Batch insert in one transaction */
    yq_txn *txn = NULL;
    yq_txn_begin(db, YQ_TXN_READWRITE, &txn);
    for (int i = 0; i < 100; ++i) {
        char kb[32], vb[32];
        int kl = snprintf(kb, sizeof(kb), "key:%04d", i);
        int vl = snprintf(vb, sizeof(vb), "value-%d", i);
        yq_slice k = { kb, (size_t)kl };
        yq_slice v = { vb, (size_t)vl };
        yq_put(txn, k, v, YQ_PUT_UPSERT);
    }
    if (yq_txn_commit(txn) != YQ_OK) {
        fprintf(stderr, "commit failed\n");
        yq_close(db);
        return 1;
    }

    /* Scan */
    yq_txn_begin(db, YQ_TXN_READONLY, &txn);
    yq_cur *cur = NULL;
    yq_cur_open(txn, &cur);
    for (int r = yq_cur_first(cur); r == YQ_OK; r = yq_cur_next(cur)) {
        yq_slice ck, cv;
        yq_cur_key(cur, &ck);
        yq_cur_val(cur, &cv);
        printf("%.*s = %.*s\n",
               (int)ck.size, (const char *)ck.data,
               (int)cv.size, (const char *)cv.data);
    }
    yq_cur_close(cur);
    yq_txn_commit(txn);

    yq_stat st;
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(yq_stat);
    yq_db_stat(db, &st);
    printf("txn_id=%llu log_bytes=%llu\n",
           (unsigned long long)st.txn_id,
           (unsigned long long)st.log_bytes);

    yq_close(db);
    return 0;
}
```

---

## 13. Troubleshooting

| Symptom | Likely cause | Action |
|---------|--------------|--------|
| `YQ_ERR_INVAL` on open | `struct_size` not set, bad `page_size`, `path` NULL | Fill `struct_size = sizeof(yq_opts)`; use a power-of-two `page_size` |
| `YQ_ERR_BUSY` on `txn_begin` | Another writer holds the lock, `lock_timeout_ms == 0` | Retry, or set `lock_timeout_ms > 0` |
| `YQ_ERR_READER_FULL` | Readers exceed `max_readers` | Reopen with a larger `max_readers` |
| `YQ_ERR_MAP_FULL` | Dataset exceeds `map_size` | Reopen with a larger `map_size` |
| `YQ_ERR_TOOBIG` | Key > 1024 bytes | Shorten or hash the key |
| `YQ_ERR_CORRUPT` | CRC mismatch or illegal format | Stop writes, copy files, export read-only, report |
| Log keeps growing | No checkpoints during long run | Call `yq_checkpoint` periodically |
| Data missing after reopen | Committed with `YQ_SYNC_OFF` | Use `YQ_SYNC_NORMAL`/`FULL`, or call `yq_sync` |
| `yq_get` returns stale pointer after write | Borrowed pointer invalidated by `yq_put`/`yq_del` | Copy the value before further writes |

---

For the on-disk format see [FORMAT.md](../FORMAT.md); for error semantics see
[ERRORS.md](../ERRORS.md); for the exact ABI see [include/yq.h](../include/yq.h).
