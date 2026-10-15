#include "yq_memtable.h"
#include "yq_memblk.h"
#include "yq_enc.h"
#include "yq_slice.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/*
 * P1: memtable index is a chunked sorted array.
 *
 * The previous structure was one sorted array: O(log n) probes (cache
 * friendly, contiguous) but O(n) inserts -- a memmove of half the array on
 * every random-key insert, which pinned random insert at ~32k ops/s at 100k
 * keys and made WAL replay O(n^2) (recovery replayed records one by one
 * into the same structure).
 *
 * A skip list was implemented and measured first, and REJECTED: it fixed the
 * insert (~12x) but made point reads ~3x slower and full scans ~15x slower,
 * because a pointer-chasing walk over arena-scattered nodes destroys the
 * locality this engine's read and scan paths are built on (zero-copy
 * borrowed values, ~150M ops/s scans). Structure matters more than
 * asymptotics here.
 *
 * The chunked array keeps the contiguous binary search and the linear scan
 * exactly as they were, and only bounds the memmove:
 *
 *   - entries live in sorted chunks of at most MT_CHUNK_CAP entries;
 *   - a lookup is a binary search over chunk first-keys (log2(num_chunks))
 *     plus a binary search inside the chunk (log2(cap)) -- the SAME total
 *     probe count as the single array's log2(n), with one extra indirection
 *     into the chunk array, which stays hot in L1;
 *   - an insert memmoves only within one chunk: O(cap) instead of O(n);
 *   - iteration walks chunk after chunk, entries contiguous inside each;
 *   - a chunk that fills splits in half, distributing levels again.
 *
 * With MT_CHUNK_CAP = 2048 and n = 100k: 17 probes either way, memmove
 * ~40 KB instead of ~2 MB (50x), so insert cost becomes the WAL/pending
 * work instead of memory bandwidth.
 *
 * Read and scan parity had to be recovered rather than assumed. The first cut
 * of the chunked iterator cost 8.1 ns/entry against the single array's 4.7 ns
 * on a 100k-key scan (0.6x): every step re-derived the chunk descriptor
 * (num_chunks -> chunks[chunk] -> c.count -> c.entries) and the cursor wrapper
 * re-checked validity that next() had just established. Caching the current
 * chunk's (entries, count) in the iterator -- safe because a chunk's array is
 * allocated once and only released under a gen bump -- brings it back to two
 * loads per step, and best-of-7 benchmarks put scan at 0.97x of the
 * single-array engine with every other metric ahead.
 *
 * Accounting is unchanged on purpose: entry_cost is still
 * key_len + val_len + sizeof(mt_entry), so yq_memtable_bytes() /
 * yq_memtable_full() fire at exactly the same thresholds as before (the
 * pre-existing quirk where reviving a tombstone does not reclaim the
 * tombstone's cost is preserved too -- see the unmerged
 * fix/memtable-tombstone-revive-accounting branch).
 */

#define MT_CHUNK_CAP 2048
#define MT_CHUNK_GROW_FROM 256

typedef struct mt_entry {
    size_t key_offset;
    size_t key_len;
    size_t val_offset;
    size_t val_len;
    int tombstone;
} mt_entry;

typedef struct mt_chunk {
    mt_entry *entries;     /* sorted by key; never empty once linked in */
    size_t count;
    size_t cap;
} mt_chunk;

struct yq_memtable {
    yq_memblk *arena;
    mt_chunk *chunks;      /* sorted by first key; [0] is always linked */
    size_t num_chunks;
    size_t cap_chunks;
    size_t num_entries;    /* includes tombstones, as before */
    size_t max_bytes;
    size_t used_bytes;
    size_t arena_cap;
    int enforce_limit;
    /*
     * Generation counter, bumped by reset(). Iterators record it when they
     * position and re-check it on every access, so an iterator cannot serve
     * bytes that reset() has discarded (the old `pos` happened to fail
     * safe; an entry pointer does not).
     */
    uint64_t gen;
};

/*
 * An iterator position is (chunk, idx). The one-past-the-end position is
 * encoded as chunk == num_chunks with idx == 0, which valid() rejects.
 */
struct yq_memtable_iter {
    yq_memtable *mt;
    size_t chunk;          /* current chunk index; == num_chunks at the end */
    size_t idx;            /* index within chunks[chunk].entries */
    uint64_t gen;          /* mt->gen when the iterator last positioned */
    mt_entry *ent;         /* chunks[chunk].entries, NULL at the end sentinel */
    size_t ent_cnt;        /* chunks[chunk].count, cached alongside ent */
};

static size_t entry_cost(size_t key_len, size_t val_len) {
    return key_len + val_len + sizeof(mt_entry);
}

/* Compare a key against an entry. Result is compare(key, entry_key). */
static int entry_cmp_raw(const uint8_t *base, const mt_entry *e,
                         const uint8_t *key, size_t key_len) {
    return yq_slice_compare_raw(key, key_len, base + e->key_offset, e->key_len);
}

/* ── chunk layout ─────────────────────────────────────────────────────── */

static void chunk_free(mt_chunk *c) {
    free(c->entries);
    c->entries = NULL;
    c->count = 0;
    c->cap = 0;
}

static int chunk_init(mt_chunk *c, size_t cap) {
    c->entries = (mt_entry *)malloc(cap * sizeof(mt_entry));
    if (!c->entries) return YQ_ERR_NOMEM;
    c->count = 0;
    c->cap = cap;
    return YQ_OK;
}

/*
 * Binary search inside one chunk: index of the first entry whose key is
 * >= key, or c->count when every key in the chunk is smaller.
 */
static size_t chunk_lower_bound(const uint8_t *base, const mt_chunk *c,
                                const uint8_t *key, size_t key_len) {
    size_t lo = 0, hi = c->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        /* cmp > 0 means key > entry key: the bound is to the right. */
        if (entry_cmp_raw(base, &c->entries[mid], key, key_len) > 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/*
 * Binary search over the chunk index: index of the first chunk whose first
 * key is >= key. Chunks are non-empty and strictly ordered, so the lower
 * bound is either in that chunk or in the one before it.
 */
static size_t chunk_index_lower_bound(const uint8_t *base, const yq_memtable *mt,
                                      const uint8_t *key, size_t key_len) {
    size_t lo = 0, hi = mt->num_chunks;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        /* cmp > 0 means key > the chunk's first key: go right. */
        if (entry_cmp_raw(base, &mt->chunks[mid].entries[0], key, key_len) > 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/*
 * Resolve a lookup into (chunk, idx). Returns 1 with the two out parameters
 * set to the first entry with key >= key, or 0 when every key in the table
 * is smaller.
 */
static int locate(const yq_memtable *mt, const yq_slice *key,
                  size_t *ci, size_t *ii) {
    /*
     * Chunk 0 is the only chunk that can be empty (before the first insert),
     * and it is the only chunk when the table is empty -- reading its
     * entries[0] would touch uninitialised memory, so short-circuit.
     */
    if (mt->num_entries == 0) return 0;
    const uint8_t *base = (const uint8_t *)yq_memblk_base(mt->arena);

    size_t c = chunk_index_lower_bound(base, mt, key->data, key->size);
    if (c > 0) c--;                       /* the chunk whose range covers key */
    size_t i = chunk_lower_bound(base, &mt->chunks[c], key->data, key->size);

    if (i == mt->chunks[c].count) {
        /* key is past this chunk's last entry: the lower bound is the next
         * chunk's first entry (if any). */
        if (c + 1 >= mt->num_chunks) return 0;
        c++;
        i = 0;
    }
    *ci = c;
    *ii = i;
    return 1;
}

/* ── chunk splitting ─────────────────────────────────────────────────── */

static int chunks_reserve(yq_memtable *mt, size_t need) {
    if (need <= mt->cap_chunks) return YQ_OK;
    size_t ncap = mt->cap_chunks ? mt->cap_chunks * 2 : 8;
    while (ncap < need) ncap *= 2;
    mt_chunk *nc = (mt_chunk *)realloc(mt->chunks, ncap * sizeof(mt_chunk));
    if (!nc) return YQ_ERR_NOMEM;
    mt->chunks = nc;
    mt->cap_chunks = ncap;
    return YQ_OK;
}

/*
 * Split chunk `c` in half. The upper half takes over slot c+1, so the lower
 * half keeps index c; entries already placed keep their positions.
 */
static int split_chunk(yq_memtable *mt, size_t c) {
    int rc = chunks_reserve(mt, mt->num_chunks + 1);
    if (rc != YQ_OK) return rc;

    mt_chunk *src = &mt->chunks[c];
    size_t half = src->count / 2;
    if (half == 0) return YQ_OK;   /* cannot happen: caps start at 256 */

    mt_chunk fresh;
    rc = chunk_init(&fresh, MT_CHUNK_CAP);
    if (rc != YQ_OK) return rc;

    memcpy(fresh.entries, src->entries + half,
           (src->count - half) * sizeof(mt_entry));
    fresh.count = src->count - half;
    src->count = half;

    memmove(&mt->chunks[c + 2], &mt->chunks[c + 1],
            (mt->num_chunks - c - 1) * sizeof(mt_chunk));
    mt->chunks[c + 1] = fresh;
    mt->num_chunks++;
    return YQ_OK;
}

/* ── allocation of key/value bytes ────────────────────────────────────── */

static int alloc_copy(yq_memtable *mt, const void *data, size_t len, size_t *out_off) {
    size_t need = len ? len : 1;
    uint8_t *p = (uint8_t *)yq_memblk_alloc(mt->arena, need);
    if (!p) return YQ_ERR_NOMEM;
    if (len) memcpy(p, data, len);
    *out_off = (size_t)(p - (uint8_t *)yq_memblk_base(mt->arena));
    return YQ_OK;
}

/*
 * Place a fully-copied entry at (ci, ii), splitting the chunk first when it
 * is at capacity. Both callers have already decided the position is correct;
 * this only performs the physical insert.
 */
static int insert_at(yq_memtable *mt, size_t ci, size_t ii,
                     size_t koff, size_t klen, size_t voff, size_t vlen) {
    mt_chunk *c = &mt->chunks[ci];
    if (c->count >= c->cap) {
        int rc = split_chunk(mt, ci);
        if (rc != YQ_OK) return rc;
        c = &mt->chunks[ci];
        if (ii > c->count) {
            /* the insert point moved into the new upper half */
            ci++;
            ii -= c->count;
            c = &mt->chunks[ci];
        }
    }

    mt_entry *e = &c->entries[ii];
    memmove(e + 1, e, (c->count - ii) * sizeof(mt_entry));
    e->key_offset = koff;
    e->key_len = klen;
    e->val_offset = voff;
    e->val_len = vlen;
    e->tombstone = 0;
    c->count++;
    mt->num_entries++;
    return YQ_OK;
}

/* ── lifecycle ────────────────────────────────────────────────────────── */

yq_memtable *yq_memtable_create(size_t max_bytes) {
    if (max_bytes == 0) max_bytes = 64 * 1024 * 1024;

    yq_memtable *mt = calloc(1, sizeof(yq_memtable));
    if (!mt) return NULL;

    mt->arena = yq_memblk_create(max_bytes * 2);
    if (!mt->arena) {
        free(mt);
        return NULL;
    }
    mt->arena_cap = yq_memblk_size(mt->arena);

    mt->chunks = (mt_chunk *)calloc(1, sizeof(mt_chunk));
    if (!mt->chunks) {
        yq_memblk_destroy(mt->arena);
        free(mt);
        return NULL;
    }
    mt->cap_chunks = 1;
    mt->num_chunks = 1;               /* chunk 0 always exists, may be empty */
    if (chunk_init(&mt->chunks[0], MT_CHUNK_GROW_FROM) != YQ_OK) {
        free(mt->chunks);
        yq_memblk_destroy(mt->arena);
        free(mt);
        return NULL;
    }

    mt->max_bytes = max_bytes;
    mt->enforce_limit = 1;
    mt->gen = 1;
    return mt;
}

void yq_memtable_destroy(yq_memtable *mt) {
    if (!mt) return;
    if (mt->chunks) {
        for (size_t i = 0; i < mt->num_chunks; i++) chunk_free(&mt->chunks[i]);
        free(mt->chunks);
    }
    if (mt->arena) yq_memblk_destroy(mt->arena);
    free(mt);
}

/* ── point operations ─────────────────────────────────────────────────── */

/*
 * Overwrite path of yq_memtable_put().
 *
 * Two problems, both found by the perf/memtable-inplace-overwrite branch's own
 * test (src/yq_test_memtable_inplace.c), which failed here:
 *
 * 1. Every update allocated a fresh value region from the bump arena and
 *    stranded the previous one. The arena never reclaims, so a hot key written
 *    repeatedly grew it without bound: with a 1 MiB memtable (2 MiB arena) and
 *    1 KiB values, the PUT started failing with YQ_ERR_NOMEM after 2047
 *    overwrites while used_bytes -- the logical accounting -- stayed at 1071
 *    bytes the whole time. The memtable exists to absorb writes between
 *    checkpoints, so failing a write on a key that occupies 1 KiB of 1 MiB is a
 *    robustness hole, not a resource limit.
 *
 * 2. The accounting was adjusted BEFORE the allocation, so when the allocation
 *    failed, used_bytes was left permanently short by the old entry's cost
 *    (down to 0 in the same scenario) even though the entry itself was intact.
 *    That corrupts the admission check for every later write.
 *
 * So: reuse the slot the entry already owns when the new value fits in it, and
 * only ever adjust used_bytes after the bytes exist.
 */
static int memtable_overwrite(yq_memtable *mt, mt_entry *e, yq_slice val,
                              size_t cost) {
    /*
     * In place when the value fits the region the entry already owns. The slot
     * keeps its size: a later update of the same key can reuse it again, so
     * arena usage becomes a function of the number of live keys rather than of
     * the number of writes. A tombstone owns no value region (val_len == 0),
     * so it necessarily falls through to a fresh allocation.
     */
    if (!e->tombstone && val.size <= e->val_len) {
        if (val.size) {
            uint8_t *base = (uint8_t *)yq_memblk_base(mt->arena);
            memcpy(base + e->val_offset, val.data, val.size);
        }
        mt->used_bytes -= entry_cost(e->key_len, e->val_len);
        e->val_len = val.size;
        e->tombstone = 0;
        mt->used_bytes += cost;
        return YQ_OK;
    }

    /* Growing update: allocate first, adjust the accounting only once the bytes
     * exist, so a failure here leaves used_bytes exactly as it was. */
    size_t voff = 0;
    int rc = alloc_copy(mt, val.data, val.size, &voff);
    if (rc != YQ_OK) return rc;
    if (!e->tombstone) mt->used_bytes -= entry_cost(e->key_len, e->val_len);
    e->val_offset = voff;
    e->val_len = val.size;
    e->tombstone = 0;
    mt->used_bytes += cost;
    return YQ_OK;
}


int yq_memtable_put(yq_memtable *mt, yq_slice key, yq_slice val) {
    if (!mt || key.size == 0 || key.size > 1024) return YQ_ERR_INVAL;

    size_t cost = entry_cost(key.size, val.size);
    /*
     * Admission check. Written as cost > max - used (the enforcing caller
     * maintains used <= max, so the subtraction cannot wrap) instead of
     * used + cost > max to stay safe if used_bytes ever overshoots.
     * yq_memtable_set_limit_enforcement(mt, 0) lifts this for crash
     * recovery, where exceeding the threshold must not make the database
     * unopenable.
     */
    if (mt->enforce_limit && cost > mt->max_bytes - mt->used_bytes) {
        return YQ_ERR_NOMEM;
    }

    size_t ci = 0, ii = 0;
    if (locate(mt, &key, &ci, &ii)) {
        mt_chunk *c = &mt->chunks[ci];
        if (ii < c->count) {
            const uint8_t *base = (const uint8_t *)yq_memblk_base(mt->arena);
            if (entry_cmp_raw(base, &c->entries[ii], key.data, key.size) == 0) {
                return memtable_overwrite(mt, &c->entries[ii], val, cost);
            }
        }
    }

    /* New key: copy the bytes first, then place the entry. */
    size_t koff = 0, voff = 0;
    int rc = alloc_copy(mt, key.data, key.size, &koff);
    if (rc != YQ_OK) return rc;
    rc = alloc_copy(mt, val.data, val.size, &voff);
    if (rc != YQ_OK) return rc;

    /*
     * Append fast path: when the key is past the last entry of the last
     * chunk (sequential inserts, WAL replay), one compare replaces both
     * binary searches. The last chunk can be full, in which case it split
     * inside the general path anyway -- so just fall through to it.
     */
    if (mt->num_entries > 0) {
        mt_chunk *lc = &mt->chunks[mt->num_chunks - 1];
        if (lc->count > 0 && lc->count < lc->cap) {
            const uint8_t *base = (const uint8_t *)yq_memblk_base(mt->arena);
            mt_entry *last = &lc->entries[lc->count - 1];
            /* compare(key, last) > 0 means key is past every key in the
             * table: the last chunk holds the global maximum. */
            if (entry_cmp_raw(base, last, key.data, key.size) > 0) {
                rc = insert_at(mt, mt->num_chunks - 1, lc->count,
                               koff, key.size, voff, val.size);
                if (rc != YQ_OK) return rc;
                mt->used_bytes += cost;
                return YQ_OK;
            }
        }
    }

    if (!locate(mt, &key, &ci, &ii)) {
        /* key is past every entry (or the table is empty): append at the end
         * of the last chunk, starting a new one when it is full. */
        ci = mt->num_chunks - 1;
        ii = mt->chunks[ci].count;
        if (ii >= mt->chunks[ci].cap) {
            rc = split_chunk(mt, ci);
            if (rc != YQ_OK) return rc;
            ci++;                          /* the new upper half */
            /* The upper half is NOT empty: split_chunk() moved the back half
             * of the full chunk into it, so the append point is its end. */
            ii = mt->chunks[ci].count;
        }
    }
    rc = insert_at(mt, ci, ii, koff, key.size, voff, val.size);
    if (rc != YQ_OK) return rc;
    mt->used_bytes += cost;
    return YQ_OK;
}

int yq_memtable_del(yq_memtable *mt, yq_slice key) {
    if (!mt || key.size == 0 || key.size > 1024) return YQ_ERR_INVAL;

    size_t ci = 0, ii = 0;
    if (locate(mt, &key, &ci, &ii)) {
        mt_chunk *c = &mt->chunks[ci];
        if (ii < c->count) {
            const uint8_t *base = (const uint8_t *)yq_memblk_base(mt->arena);
            if (entry_cmp_raw(base, &c->entries[ii], key.data, key.size) == 0) {
                mt_entry *e = &c->entries[ii];
                if (e->tombstone) return YQ_OK;
                mt->used_bytes -= entry_cost(e->key_len, e->val_len);
                e->tombstone = 1;
                e->val_offset = 0;
                e->val_len = 0;
                mt->used_bytes += entry_cost(e->key_len, 0);
                return YQ_OK;
            }
        }
    }

    /*
     * Deleting an absent key installs a tombstone, as the single-array
     * version did: later reads must not fall through to the B+Tree behind
     * this memtable. Route through yq_memtable_put() for the placement (it
     * owns chunking and splitting), then restate the entry as a tombstone.
     * The accounting comes out identical to the array version: put() with
     * an empty value charges key + 0 + 40, exactly what a tombstone costs,
     * both when the key was live and when it was absent.
     */
    yq_slice empty;
    yq_slice_set(&empty, NULL, 0);

    int rc = yq_memtable_put(mt, key, empty);
    if (rc != YQ_OK) return rc;

    if (!locate(mt, &key, &ci, &ii)) return YQ_ERR_PANIC;
    mt_chunk *c2 = &mt->chunks[ci];
    if (ii >= c2->count) return YQ_ERR_PANIC;
    const uint8_t *base = (const uint8_t *)yq_memblk_base(mt->arena);
    if (entry_cmp_raw(base, &c2->entries[ii], key.data, key.size) != 0) {
        return YQ_ERR_PANIC;
    }
    c2->entries[ii].tombstone = 1;
    return YQ_OK;
}

int yq_memtable_get(yq_memtable *mt, yq_slice key, yq_slice *out) {
    if (!mt || !out) return YQ_ERR_INVAL;

    size_t ci = 0, ii = 0;
    if (!locate(mt, &key, &ci, &ii)) return YQ_ERR_NOTFOUND;

    mt_chunk *c = &mt->chunks[ci];
    const uint8_t *base = (const uint8_t *)yq_memblk_base(mt->arena);
    if (entry_cmp_raw(base, &c->entries[ii], key.data, key.size) != 0) {
        return YQ_ERR_NOTFOUND;
    }
    mt_entry *e = &c->entries[ii];
    if (e->tombstone) return YQ_ERR_NOTFOUND;

    yq_slice_set(out, base + e->val_offset, e->val_len);
    return YQ_OK;
}

int yq_memtable_size(yq_memtable *mt) {
    if (!mt) return 0;
    return (int)mt->num_entries;
}

int yq_memtable_full(yq_memtable *mt) {
    if (!mt) return 0;
    return mt->used_bytes >= mt->max_bytes;
}

size_t yq_memtable_bytes(yq_memtable *mt) {
    if (!mt) return 0;
    return mt->used_bytes;
}

void yq_memtable_set_limit_enforcement(yq_memtable *mt, int enforce) {
    if (!mt) return;
    if (enforce) {
        /*
         * Re-armed after a replay that overflowed max_bytes: grant one more
         * threshold of headroom above the recovered size. Without this the
         * admission check (cost > max - used) would either compute an
         * astronomically large bound the moment used exceeds max (threshold
         * silently disabled forever) or, if the threshold were adopted as-is,
         * reject every subsequent write. Doubling keeps the used <= max
         * invariant the check relies on while leaving the database writable
         * -- which is the entire point of lifting the check during recovery.
         */
        if (mt->used_bytes > mt->max_bytes) {
            mt->max_bytes = mt->used_bytes + mt->max_bytes;
        }
        mt->enforce_limit = 1;
    } else {
        mt->enforce_limit = 0;
    }
}

int yq_memtable_reserve(yq_memtable *mt, size_t bytes) {
    if (!mt) return YQ_ERR_INVAL;

    size_t free_cap = mt->arena_cap - yq_memblk_used(mt->arena);
    if (bytes <= free_cap) return YQ_OK;

    size_t new_cap = mt->arena_cap ? mt->arena_cap : 4096;
    while (new_cap - yq_memblk_used(mt->arena) < bytes) {
        if (new_cap > SIZE_MAX / 2) return YQ_ERR_NOMEM;
        new_cap *= 2;
    }

    yq_memblk *grown = yq_memblk_grow(mt->arena, new_cap);
    if (!grown) return YQ_ERR_NOMEM;

    mt->arena = grown;
    mt->arena_cap = yq_memblk_size(grown);
    return YQ_OK;
}

int yq_memtable_bulk_load(yq_memtable *mt, const yq_bulk_entry *entries,
                          size_t n) {
    if (!mt || (!entries && n > 0)) return YQ_ERR_INVAL;
    if (n == 0) return YQ_OK;
    if (mt->num_entries != 0) return YQ_ERR_INVAL;   /* empty table only */

    /*
     * Validate the preconditions instead of trusting them.
     *
     * bulk_load lays the run out in key order across chunks and binary-searches
     * every chunk on the assumption that keys are strictly increasing. A caller
     * that hands over unsorted or duplicated keys does not get a rejected load
     * -- it gets a table whose chunks each look sorted to themselves while the
     * global order is broken, which surfaces much later as a key that cannot be
     * found or a chunk that grows without bound. That is the worst shape of bug
     * to chase back to here, and the check is a single O(n) pass with an early
     * exit, next to a call that has just sorted the same keys.
     */
    for (size_t i = 0; i < n; i++) {
        if (entries[i].key.size == 0 || entries[i].key.size > 1024) {
            return YQ_ERR_INVAL;
        }
        if (i > 0) {
            const yq_slice *prev = &entries[i - 1].key;
            const yq_slice *cur = &entries[i].key;
            size_t m = prev->size < cur->size ? prev->size : cur->size;
            int cmp = memcmp(prev->data, cur->data, m);
            if (cmp == 0 && prev->size != cur->size) cmp = prev->size < cur->size ? -1 : 1;
            /* >= 0 means "not strictly increasing": duplicate or out of order */
            if (cmp >= 0) return YQ_ERR_INVAL;
        }
    }

    /*
     * Lay out the chunk frame first: ceil(n / cap) chunks, all but the last
     * at full capacity. Chunk 0 already exists and keeps its small initial
     * capacity when the whole run fits in it (small recoveries stay cheap).
     * Note that the entries[] array must not move afterwards -- chunk_init /
     * chunks_reserve only reallocate our own arrays.
     */
    int rc = chunks_reserve(mt, (n + MT_CHUNK_CAP - 1) / MT_CHUNK_CAP);
    if (rc != YQ_OK) return rc;

    if (n > mt->chunks[0].cap) {
        chunk_free(&mt->chunks[0]);
        rc = chunk_init(&mt->chunks[0], n < MT_CHUNK_CAP ? n : MT_CHUNK_CAP);
        if (rc != YQ_OK) return rc;
    }
    mt->num_chunks = 1;

    size_t done = 0;
    while (done < n) {
        mt_chunk *c = &mt->chunks[mt->num_chunks - 1];
        if (c->count >= c->cap) {
            /* Chunk full: open the next one. (c is re-fetched at the top of
             * the loop, so a realloc of the chunk array is safe.) */
            size_t want = n - done;
            if (want > MT_CHUNK_CAP) want = MT_CHUNK_CAP;
            rc = chunks_reserve(mt, mt->num_chunks + 1);
            if (rc != YQ_OK) return rc;
            rc = chunk_init(&mt->chunks[mt->num_chunks], want);
            if (rc != YQ_OK) return rc;
            mt->num_chunks++;
            continue;
        }

        size_t take = n - done;
        size_t room = c->cap - c->count;
        if (take > room) take = room;

        for (size_t i = 0; i < take; i++) {
            const yq_bulk_entry *e = &entries[done + i];

            size_t koff = 0, voff = 0;
            rc = alloc_copy(mt, e->key.data, e->key.size, &koff);
            if (rc != YQ_OK) return rc;
            if (!e->tombstone) {
                rc = alloc_copy(mt, e->val.data, e->val.size, &voff);
                if (rc != YQ_OK) return rc;
            }

            mt_entry *m = &c->entries[c->count];
            m->key_offset = koff;
            m->key_len = e->key.size;
            m->val_offset = voff;
            m->val_len = e->tombstone ? 0 : e->val.size;
            m->tombstone = e->tombstone ? 1 : 0;
            c->count++;
            mt->num_entries++;
            mt->used_bytes += entry_cost(m->key_len, m->val_len);
        }
        done += take;
    }
    return YQ_OK;
}

/* ── iteration ────────────────────────────────────────────────────────── */

/* A cursor positioned before a reset(), or never positioned, is unusable. */
static int iter_stale(yq_memtable_iter *it) {
    return !it || !it->mt || it->gen != it->mt->gen;
}

/*
 * Refresh the cached (entries, count) of the current chunk, and cache the
 * end sentinel as (NULL, 0).
 *
 * The cache is what keeps a full scan off the chunks[] -> chunk -> entries
 * indirection: the single-array memtable this replaced needed two loads per
 * step (num_entries, entry.tombstone) and the chunked one needed four
 * (num_chunks, chunks[chunk], c.count, c.entries), which measured 4.7 ->
 * 8.1 ns per entry on a 100k-key scan. With the cache it is two loads again.
 *
 * Holding the base pointer across inserts is safe: chunk_init allocates an
 * entries array once and only chunk_free releases it, which happens on
 * destroy or reset -- and reset bumps gen, so every iterator that could be
 * holding the pointer fails iter_stale() first. insert_at() memmoves inside
 * the array it already owns, and split_chunk() halves into a NEW array rather
 * than growing the old one, so an existing chunk's base never moves. Only the
 * cached count can go stale, and only as stale as the (chunk, idx) position
 * already is for a cursor held across a write.
 */
static void iter_sync(yq_memtable_iter *it) {
    if (it->chunk < it->mt->num_chunks) {
        it->ent    = it->mt->chunks[it->chunk].entries;
        it->ent_cnt = it->mt->chunks[it->chunk].count;
    } else {
        it->ent    = NULL;   /* end sentinel */
        it->ent_cnt = 0;
    }
}

/* Enter chunk n and cache it. Assumes n < num_chunks. */
static void iter_goto_chunk(yq_memtable_iter *it, size_t chunk, size_t idx) {
    it->chunk = chunk;
    it->idx   = idx;
    it->ent    = it->mt->chunks[chunk].entries;
    it->ent_cnt = it->mt->chunks[chunk].count;
}

/*
 * Advance (chunk, idx) to the next live entry; the end sentinel when none.
 * The cache is assumed in sync on entry and is kept in sync on exit, so a
 * caller that just set (chunk, idx) must call iter_sync() first.
 */
static void iter_next_live(yq_memtable_iter *it) {
    yq_memtable *mt = it->mt;
    while (it->chunk < mt->num_chunks) {
        if (it->idx < it->ent_cnt) {
            if (!it->ent[it->idx].tombstone) return;   /* the common case */
            it->idx++;
            continue;
        }
        /* Off the end of this chunk: step into the next one and cache it. */
        if (it->chunk + 1 < mt->num_chunks) {
            iter_goto_chunk(it, it->chunk + 1, 0);
            continue;
        }
        it->chunk++;      /* no further chunk: park at the end sentinel */
        it->idx   = 0;
        it->ent    = NULL;
        it->ent_cnt = 0;
        return;
    }
    it->idx = 0;
}

/*
 * Step back to the previous live entry. The end sentinel (chunk ==
 * num_chunks) starts the walk from the very last entry; when no live entry
 * exists at or before the position, the cursor is parked at the end
 * sentinel, which valid() rejects.
 */
static void iter_prev_live(yq_memtable_iter *it) {
    yq_memtable *mt = it->mt;
    if (mt->num_chunks == 0) {
        it->chunk = 0; it->idx = 0; it->ent = NULL; it->ent_cnt = 0;
        return;
    }

    if (it->chunk >= mt->num_chunks) {
        iter_goto_chunk(it, mt->num_chunks - 1, mt->chunks[mt->num_chunks - 1].count);
    }

    while (1) {
        while (it->idx > 0) {
            it->idx--;
            if (!it->ent[it->idx].tombstone) return;
        }
        if (it->chunk == 0) break;
        iter_goto_chunk(it, it->chunk - 1, mt->chunks[it->chunk - 1].count);
    }

    it->chunk = mt->num_chunks;   /* nothing at or before it: end sentinel */
    it->idx  = 0;
    it->ent    = NULL;
    it->ent_cnt = 0;
}

int yq_memtable_iter_open(yq_memtable *mt, yq_memtable_iter **out) {
    if (!mt || !out) return YQ_ERR_INVAL;

    yq_memtable_iter *it = calloc(1, sizeof(yq_memtable_iter));
    if (!it) return YQ_ERR_NOMEM;

    it->mt = mt;
    it->chunk = 0;
    it->idx = 0;
    it->gen = mt->gen;
    iter_sync(it);
    iter_next_live(it);   /* positions at the first live entry */

    *out = it;
    return YQ_OK;
}

void yq_memtable_iter_close(yq_memtable_iter *it) {
    free(it);
}

int yq_memtable_iter_first(yq_memtable_iter *it) {
    if (!it || !it->mt) return YQ_ERR_INVAL;

    it->chunk = 0;
    it->idx = 0;
    it->gen = it->mt->gen;
    iter_sync(it);
    iter_next_live(it);
    if (!yq_memtable_iter_valid(it)) return YQ_ERR_NOTFOUND;
    return YQ_OK;
}

int yq_memtable_iter_next(yq_memtable_iter *it) {
    if (iter_stale(it)) return YQ_ERR_CURSOR;

    it->idx++;                /* leave the current entry either way */
    iter_next_live(it);
    if (!yq_memtable_iter_valid(it)) return YQ_ERR_NOTFOUND;
    return YQ_OK;
}

int yq_memtable_iter_last(yq_memtable_iter *it) {
    if (!it || !it->mt) return YQ_ERR_INVAL;

    it->chunk = it->mt->num_chunks;   /* start from the end sentinel */
    it->idx = 0;
    it->gen = it->mt->gen;
    iter_prev_live(it);
    if (!yq_memtable_iter_valid(it)) return YQ_ERR_NOTFOUND;
    return YQ_OK;
}

int yq_memtable_iter_prev(yq_memtable_iter *it) {
    if (iter_stale(it)) return YQ_ERR_CURSOR;

    /*
     * Unlike next(), the pre-check here is semantic, not redundant:
     * iter_prev_live() treats the end sentinel as "start from the last
     * entry" (that is what yq_memtable_iter_last relies on), so stepping it
     * from an invalid position would resurrect the final entry instead of
     * reporting NOTFOUND. Guard it first.
     */
    if (!yq_memtable_iter_valid(it)) return YQ_ERR_NOTFOUND;

    iter_prev_live(it);
    if (!yq_memtable_iter_valid(it)) return YQ_ERR_NOTFOUND;
    return YQ_OK;
}

int yq_memtable_iter_seek(yq_memtable_iter *it, yq_slice key) {
    if (!it || !it->mt) return YQ_ERR_INVAL;

    /*
     * locate() is the same descent the point lookup uses, so this is
     * O(log n) rather than the O(n) scan a "next until >= target" loop
     * would need. Tombstones keep their slot in the order, so the lower
     * bound is correct across live and deleted entries alike; stepping
     * forward from it lands on the first *live* key >= target.
     */
    size_t ci = 0, ii = 0;
    if (locate(it->mt, &key, &ci, &ii)) {
        it->chunk = ci;
        it->idx = ii;
    } else {
        it->chunk = it->mt->num_chunks;   /* past the end */
        it->idx = 0;
    }
    it->gen = it->mt->gen;
    iter_sync(it);
    iter_next_live(it);
    if (!yq_memtable_iter_valid(it)) return YQ_ERR_NOTFOUND;
    return YQ_OK;
}

static int iter_at(yq_memtable_iter *it, yq_slice *out, int want_key) {
    if (!out) return YQ_ERR_INVAL;
    if (iter_stale(it) || !yq_memtable_iter_valid(it)) return YQ_ERR_CURSOR;

    const mt_entry *e = &it->ent[it->idx];
    const uint8_t *base = (const uint8_t *)yq_memblk_base(it->mt->arena);
    if (want_key) yq_slice_set(out, base + e->key_offset, e->key_len);
    else          yq_slice_set(out, base + e->val_offset, e->val_len);
    return YQ_OK;
}

int yq_memtable_iter_key(yq_memtable_iter *it, yq_slice *out) {
    return iter_at(it, out, 1);
}

int yq_memtable_iter_val(yq_memtable_iter *it, yq_slice *out) {
    return iter_at(it, out, 0);
}

int yq_memtable_iter_valid(yq_memtable_iter *it) {
    if (iter_stale(it)) return 0;
    return it->ent != NULL && it->idx < it->ent_cnt;
}

void yq_memtable_reset(yq_memtable *mt) {
    if (!mt) return;
    /*
     * Drop entry storage but keep the chunk frame (chunk 0), then rewind the
     * arena and bump the generation so cursors opened before this point fail
     * instead of reading discarded bytes.
     */
    for (size_t i = 1; i < mt->num_chunks; i++) chunk_free(&mt->chunks[i]);
    mt->num_chunks = 1;
    mt->chunks[0].count = 0;
    yq_memblk_reset(mt->arena);
    mt->num_entries = 0;
    mt->used_bytes = 0;
    mt->gen++;
}
