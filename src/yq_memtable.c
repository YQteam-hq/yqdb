#include "yq_memtable.h"
#include "yq_memblk.h"
#include "yq_enc.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define TOMBSTONE_VAL 0xFF

typedef struct mt_entry {
    size_t key_offset;
    size_t key_len;
    size_t val_offset;
    size_t val_len;
    int tombstone;
} mt_entry;

struct yq_memtable {
    yq_memblk *arena;
    mt_entry *entries;
    size_t num_entries;
    size_t cap_entries;
    size_t max_bytes;
    size_t used_bytes;
    /*
     * Block summary used by the cursor to skip tombstone runs in O(log n).
     * run_live[b] is the number of live slots in entries[b*RUN, ...).
     * Lazily (re)built by refresh_runs(); a NULL array only costs the old
     * linear-walk speed, never correctness.
     */
    uint32_t *run_live;
    size_t run_cap;
    int run_valid;
};

struct yq_memtable_iter {
    yq_memtable *mt;
    size_t pos;
};

static size_t entry_cost(size_t key_len, size_t val_len) {
    return key_len + val_len + sizeof(mt_entry);
}

static int alloc_copy(yq_memtable *mt, const void *data, size_t len, size_t *out_off) {
    size_t need = len ? len : 1;
    uint8_t *p = (uint8_t *)yq_memblk_alloc(mt->arena, need);
    if (!p) return YQ_ERR_NOMEM;
    if (len) memcpy(p, data, len);
    *out_off = (size_t)(p - (uint8_t *)yq_memblk_base(mt->arena));
    return YQ_OK;
}

static int search_entry(yq_memtable *mt, const yq_slice *key, size_t *idx) {
    size_t lo = 0, hi = mt->num_entries;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const mt_entry *e = &mt->entries[mid];
        const uint8_t *ek = (const uint8_t *)yq_memblk_base(mt->arena) + e->key_offset;
        yq_slice ek_slice;
        yq_slice_set(&ek_slice, ek, e->key_len);
        int cmp = yq_slice_compare(key, &ek_slice);
        if (cmp == 0) {
            *idx = mid;
            return 1;
        } else if (cmp < 0) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    *idx = lo;
    return 0;
}

/*
 * Locate the leftmost non-tombstone slot at or after `from`, or num_entries
 * when there is none.
 *
 * Tombstones are deleted keys that stay in the sorted array until the next
 * reset, so a cursor walking a memtable with many deletions used to advance
 * one slot at a time through them: O(n) for a single step, O(n^2) for a full
 * scan. The entries array is sorted and monotone -- "tombstone" is not a
 * property of the key, so the live slots are *not* contiguous -- which means
 * the first live slot can only be found by scanning. It can still be found in
 * O(log n): the predicate "there exists a live slot in [from, i]" is monotone
 * in i, so binary search over that predicate lands exactly on the first live
 * slot.
 *
 * The predicate is evaluated with a small precomputed summary rather than a
 * linear scan: the array is covered by blocks of YQ_MT_RUN slots, and a
 * prefix sum over per-block live counts turns "are there any live slots in
 * [from, i]" into two lookups. The summary is rebuilt lazily whenever the
 * entry count changes.
 */
#define YQ_MT_RUN 64

/*
 * Number of live (non-tombstone) slots in entries[begin, begin+count).
 */
static size_t count_live(yq_memtable *mt, size_t begin, size_t count) {
    size_t live = 0;
    size_t end = begin + count;
    for (size_t i = begin; i < end; i++) {
        if (!mt->entries[i].tombstone) live++;
    }
    return live;
}

/*
 * Rebuild the block summary with a prefix sum over live counts.
 *
 * The summary is rebuilt lazily and only when a cursor actually needs it.
 * Rebuilding costs one pass over the entries array, so the cursor pays it at
 * most once per batch of mutations -- a single linear pass -- rather than
 * paying a linear walk per step.
 *
 * run_live[b] is the cumulative live count in entries[0, ...] up to and
 * including block b, so live_before() is one load plus a partial count of one
 * block instead of a sum over blocks.
 */
static void refresh_runs(yq_memtable *mt) {
    size_t nblocks = (mt->num_entries + YQ_MT_RUN - 1) / YQ_MT_RUN;

    if (nblocks > mt->run_cap) {
        size_t ncap = mt->run_cap ? mt->run_cap : 16;
        while (ncap < nblocks) ncap *= 2;
        uint32_t *nr = (uint32_t *)realloc(mt->run_live, ncap * sizeof(uint32_t));
        if (!nr) return; /* summary is optional: fall back to a linear walk */
        mt->run_live = nr;
        mt->run_cap = ncap;
        mt->run_valid = 0;
    }

    if (mt->run_valid) return;

    size_t acc = 0;
    for (size_t b = 0; b < nblocks; b++) {
        size_t begin = b * YQ_MT_RUN;
        size_t count = mt->num_entries - begin;
        if (count > YQ_MT_RUN) count = YQ_MT_RUN;
        acc += count_live(mt, begin, count);
        mt->run_live[b] = (uint32_t)acc;
    }
    mt->run_valid = 1;
}

/*
 * Live slots in entries[0, up_to). One prefix-sum load plus a partial count
 * of the one block that straddles up_to.
 */
static size_t live_before(yq_memtable *mt, size_t up_to) {
    if (up_to > mt->num_entries) up_to = mt->num_entries;
    refresh_runs(mt);

    if (!mt->run_valid) {
        /* No summary (allocation failed): count directly. */
        return count_live(mt, 0, up_to);
    }

    size_t block = up_to / YQ_MT_RUN;
    size_t sum = block > 0 ? mt->run_live[block - 1] : 0;
    sum += count_live(mt, block * YQ_MT_RUN, up_to - block * YQ_MT_RUN);
    return sum;
}

/*
 * Short linear probe used before falling back to binary search.
 *
 * Real memtables are mostly live: a cursor step normally lands on a live slot
 * immediately, and even across a delete run the next live slot is only a few
 * entries away. Walking a handful of slots costs less than the ~log2(n)
 * summary lookups a binary search needs (each of which touches a different
 * cache line), so probe first and only search when the run is long.
 */
#define YQ_MT_LINEAR_PROBE 16

static size_t next_live_from(yq_memtable *mt, size_t from) {
    if (from >= mt->num_entries) return mt->num_entries;
    if (!mt->entries[from].tombstone) return from;

    /* Fast path: short tombstone run. */
    size_t probe_end = from + YQ_MT_LINEAR_PROBE;
    if (probe_end > mt->num_entries) probe_end = mt->num_entries;
    for (size_t i = from; i < probe_end; i++) {
        if (!mt->entries[i].tombstone) return i;
    }
    if (probe_end == mt->num_entries) return mt->num_entries;

    refresh_runs(mt);

    if (mt->run_valid) {
        /*
         * Long run: binary search the first index i in [probe_end, n) that is
         * live. "Some live slot exists in [probe_end, i]" is monotone in i,
         * so the search lands on the first live slot at or after probe_end
         * (which the probe above already proved is past the run start).
         */
        size_t base = live_before(mt, probe_end);
        size_t lo = probe_end, hi = mt->num_entries - 1;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (live_before(mt, mid + 1) > base) hi = mid;
            else lo = mid + 1;
        }
        return mt->entries[lo].tombstone ? mt->num_entries : lo;
    }

    /* No summary: continue the linear walk. */
    size_t i = probe_end;
    while (i < mt->num_entries && mt->entries[i].tombstone) i++;
    return i;
}

static size_t prev_live_from(yq_memtable *mt, size_t from) {
    if (mt->num_entries == 0) return mt->num_entries;
    if (from >= mt->num_entries) from = mt->num_entries - 1;
    if (!mt->entries[from].tombstone) return from;

    /* Fast path: short tombstone run. */
    size_t probe_end = from > YQ_MT_LINEAR_PROBE ? from - YQ_MT_LINEAR_PROBE : 0;
    for (size_t i = from; i > probe_end; i--) {
        if (!mt->entries[i - 1].tombstone) return i - 1;
    }
    if (probe_end == 0) {
        return mt->entries[0].tombstone ? mt->num_entries : 0;
    }

    refresh_runs(mt);

    if (mt->run_valid) {
        /*
         * Largest index i <= probe_end with a live slot at i, i.e. the last
         * live slot in [0, probe_end]. "There is a live slot in [i, probe_end]"
         * is monotone in i (false above the last live slot, true below it),
         * so binary search over [0, probe_end] with an upper mid finds it.
         */
        size_t top = live_before(mt, probe_end + 1);
        size_t lo = 0, hi = probe_end;
        while (lo < hi) {
            size_t mid = lo + (hi - lo + 1) / 2;
            if (top > live_before(mt, mid)) lo = mid;
            else hi = mid - 1;
        }
        return mt->entries[lo].tombstone ? mt->num_entries : lo;
    }

    size_t i = probe_end;
    for (;;) {
        if (!mt->entries[i].tombstone) return i;
        if (i == 0) return mt->num_entries;
        i--;
    }
}


/*
 * Lower bound: leftmost slot whose key is >= `key`, or num_entries.
 * Used to position the iterator for a seek without walking the array.
 */
static size_t lower_bound_slot(yq_memtable *mt, const yq_slice *key) {
    size_t idx = 0;
    search_entry(mt, key, &idx);
    return idx;
}

/* Invalidate the block summary after any structural change. */
static void runs_dirty(yq_memtable *mt) {
    mt->run_valid = 0;
}

/*
 * Debug-only consistency probe: recompute live_before() for a few positions
 * with a direct count and compare against the summarised version.
 * Enabled with -DYQ_MT_DEBUG_SUMMARY=1; compiled out entirely otherwise.
 */
#ifndef YQ_MT_DEBUG_SUMMARY
#define YQ_MT_DEBUG_SUMMARY 0
#endif

#if YQ_MT_DEBUG_SUMMARY
static void verify_summary(yq_memtable *mt, const char *where) {
    size_t probes[] = { 0, 1, mt->num_entries / 3, mt->num_entries / 2,
                        mt->num_entries, mt->num_entries + 1 };
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        size_t up = probes[i];
        if (up > mt->num_entries) up = mt->num_entries;
        size_t fast = live_before(mt, up);
        size_t slow = count_live(mt, 0, up);
        if (fast != slow) {
            fprintf(stderr, "[summary] %s: live_before(%zu)=%zu but direct count=%zu "
                            "(num_entries=%zu run_valid=%d)\n",
                    where, up, fast, slow, mt->num_entries, mt->run_valid);
        }
    }
}
#else
#define verify_summary(mt, where) ((void)0)
#endif

yq_memtable *yq_memtable_create(size_t max_bytes) {
    if (max_bytes == 0) max_bytes = 64 * 1024 * 1024;

    yq_memtable *mt = calloc(1, sizeof(yq_memtable));
    if (!mt) return NULL;

    mt->arena = yq_memblk_create(max_bytes * 2);
    if (!mt->arena) {
        free(mt);
        return NULL;
    }

    mt->max_bytes = max_bytes;
    mt->cap_entries = 256;
    mt->entries = calloc(mt->cap_entries, sizeof(mt_entry));
    if (!mt->entries) {
        yq_memblk_destroy(mt->arena);
        free(mt);
        return NULL;
    }

    return mt;
}

void yq_memtable_destroy(yq_memtable *mt) {
    if (!mt) return;
    if (mt->arena) yq_memblk_destroy(mt->arena);
    free(mt->entries);
    free(mt->run_live);
    free(mt);
}

int yq_memtable_put(yq_memtable *mt, yq_slice key, yq_slice val) {
    if (!mt || key.size == 0 || key.size > 1024) return YQ_ERR_INVAL;

    size_t cost = entry_cost(key.size, val.size);
    if (mt->used_bytes + cost > mt->max_bytes) return YQ_ERR_NOMEM;

    size_t idx;
    int found = search_entry(mt, &key, &idx);

    if (found) {
        mt_entry *e = &mt->entries[idx];
        if (!e->tombstone) mt->used_bytes -= entry_cost(e->key_len, e->val_len);
        size_t voff = 0;
        int rc = alloc_copy(mt, val.data, val.size, &voff);
        if (rc != YQ_OK) return rc;
        e->val_offset = voff;
        e->val_len = val.size;
        e->tombstone = 0;
        mt->used_bytes += cost;
        runs_dirty(mt);
        verify_summary(mt, "put-overwrite");
        return YQ_OK;
    }

    if (mt->num_entries >= mt->cap_entries) {
        size_t new_cap = mt->cap_entries * 2;
        mt_entry *new_entries = realloc(mt->entries, new_cap * sizeof(mt_entry));
        if (!new_entries) return YQ_ERR_NOMEM;
        mt->entries = new_entries;
        mt->cap_entries = new_cap;
    }

    size_t koff = 0, voff = 0;
    int rc = alloc_copy(mt, key.data, key.size, &koff);
    if (rc != YQ_OK) return rc;
    rc = alloc_copy(mt, val.data, val.size, &voff);
    if (rc != YQ_OK) return rc;

    for (size_t i = mt->num_entries; i > idx; i--) {
        mt->entries[i] = mt->entries[i - 1];
    }
    mt->num_entries++;

    mt_entry *e = &mt->entries[idx];
    e->key_offset = koff;
    e->key_len = key.size;
    e->val_offset = voff;
    e->val_len = val.size;
    e->tombstone = 0;
    mt->used_bytes += cost;
    runs_dirty(mt);
    verify_summary(mt, "structural-insert");

    return YQ_OK;
}

int yq_memtable_del(yq_memtable *mt, yq_slice key) {
    if (!mt || key.size == 0 || key.size > 1024) return YQ_ERR_INVAL;

    size_t idx;
    int found = search_entry(mt, &key, &idx);

    if (found) {
        mt_entry *e = &mt->entries[idx];
        if (e->tombstone) return YQ_OK;
        mt->used_bytes -= entry_cost(e->key_len, e->val_len);
        e->tombstone = 1;
        e->val_offset = 0;
        e->val_len = 0;
        mt->used_bytes += entry_cost(e->key_len, 0);
        runs_dirty(mt);
        verify_summary(mt, "put-overwrite");
        return YQ_OK;
    }

    if (mt->num_entries >= mt->cap_entries) {
        size_t new_cap = mt->cap_entries * 2;
        mt_entry *new_entries = realloc(mt->entries, new_cap * sizeof(mt_entry));
        if (!new_entries) return YQ_ERR_NOMEM;
        mt->entries = new_entries;
        mt->cap_entries = new_cap;
    }

    size_t koff = 0;
    int rc = alloc_copy(mt, key.data, key.size, &koff);
    if (rc != YQ_OK) return rc;

    for (size_t i = mt->num_entries; i > idx; i--) {
        mt->entries[i] = mt->entries[i - 1];
    }
    mt->num_entries++;

    mt_entry *e = &mt->entries[idx];
    e->key_offset = koff;
    e->key_len = key.size;
    e->val_offset = 0;
    e->val_len = 0;
    e->tombstone = 1;
    mt->used_bytes += entry_cost(key.size, 0);
    runs_dirty(mt);
    verify_summary(mt, "structural-insert");

    return YQ_OK;
}

int yq_memtable_get(yq_memtable *mt, yq_slice key, yq_slice *out) {
    if (!mt || !out) return YQ_ERR_INVAL;
    size_t idx;
    int found = search_entry(mt, &key, &idx);
    if (!found) return YQ_ERR_NOTFOUND;

    mt_entry *e = &mt->entries[idx];
    if (e->tombstone) return YQ_ERR_NOTFOUND;

    uint8_t *base = (uint8_t *)yq_memblk_base(mt->arena);
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

int yq_memtable_iter_open(yq_memtable *mt, yq_memtable_iter **out) {
    if (!mt) return YQ_ERR_INVAL;

    yq_memtable_iter *it = calloc(1, sizeof(yq_memtable_iter));
    if (!it) return YQ_ERR_NOMEM;

    it->mt = mt;
    it->pos = next_live_from(mt, 0);

    *out = it;
    return YQ_OK;
}

void yq_memtable_iter_close(yq_memtable_iter *it) {
    free(it);
}

int yq_memtable_iter_first(yq_memtable_iter *it) {
    if (!it || !it->mt) return YQ_ERR_INVAL;

    it->pos = next_live_from(it->mt, 0);

    if (it->pos >= it->mt->num_entries) {
        return YQ_ERR_NOTFOUND;
    }
    return YQ_OK;
}

int yq_memtable_iter_next(yq_memtable_iter *it) {
    if (!it || !it->mt) return YQ_ERR_INVAL;

    /* Hot path: the overwhelmingly common case is that the next slot is live,
     * so test it here before paying for the tombstone-skipping helper. */
    size_t next = it->pos + 1;
    if (next >= it->mt->num_entries) {
        it->pos = it->mt->num_entries;
        return YQ_ERR_NOTFOUND;
    }
    it->pos = it->mt->entries[next].tombstone ? next_live_from(it->mt, next) : next;

    if (it->pos >= it->mt->num_entries) {
        return YQ_ERR_NOTFOUND;
    }
    return YQ_OK;
}

int yq_memtable_iter_last(yq_memtable_iter *it) {
    if (!it || !it->mt) return YQ_ERR_INVAL;
    if (it->mt->num_entries == 0) { it->pos = 0; return YQ_ERR_NOTFOUND; }

    it->pos = prev_live_from(it->mt, it->mt->num_entries - 1);
    if (it->pos >= it->mt->num_entries) return YQ_ERR_NOTFOUND;
    return YQ_OK;
}

int yq_memtable_iter_prev(yq_memtable_iter *it) {
    if (!it || !it->mt) return YQ_ERR_INVAL;
    if (it->pos == 0) return YQ_ERR_NOTFOUND;

    /* Hot path, mirroring yq_memtable_iter_next(). */
    size_t prev = it->pos - 1;
    it->pos = it->mt->entries[prev].tombstone ? prev_live_from(it->mt, prev) : prev;

    if (it->pos >= it->mt->num_entries) return YQ_ERR_NOTFOUND;
    return YQ_OK;
}

/*
 * Position the iterator at the first live entry whose key is >= `key`
 * ("lower bound"), or one past the end when every key is < `key`.
 *
 * The entries array is sorted, so this is a binary search -- the previous
 * implementation of yq_cur_seek() had to walk the iterator from the first
 * entry comparing keys one by one, which is O(n) per seek and O(n^2) for a
 * range scan that seeks per step.
 *
 * Returns YQ_OK when a live entry was found and YQ_ERR_NOTFOUND otherwise;
 * the iterator is positioned either way.
 */
int yq_memtable_iter_seek(yq_memtable_iter *it, yq_slice key) {
    if (!it || !it->mt) return YQ_ERR_INVAL;

    yq_memtable *mt = it->mt;
    it->pos = next_live_from(mt, lower_bound_slot(mt, &key));

    if (it->pos >= mt->num_entries) return YQ_ERR_NOTFOUND;
    return YQ_OK;
}

int yq_memtable_iter_key(yq_memtable_iter *it, yq_slice *out) {
    if (!it || !it->mt || it->pos >= it->mt->num_entries) {
        return YQ_ERR_CURSOR;
    }

    mt_entry *e = &it->mt->entries[it->pos];
    uint8_t *base = (uint8_t *)yq_memblk_base(it->mt->arena);
    yq_slice_set(out, base + e->key_offset, e->key_len);
    return YQ_OK;
}

int yq_memtable_iter_val(yq_memtable_iter *it, yq_slice *out) {
    if (!it || !it->mt || it->pos >= it->mt->num_entries) {
        return YQ_ERR_CURSOR;
    }

    mt_entry *e = &it->mt->entries[it->pos];
    uint8_t *base = (uint8_t *)yq_memblk_base(it->mt->arena);
    yq_slice_set(out, base + e->val_offset, e->val_len);
    return YQ_OK;
}

int yq_memtable_iter_valid(yq_memtable_iter *it) {
    if (!it || !it->mt) return 0;
    return it->pos < it->mt->num_entries;
}

void yq_memtable_reset(yq_memtable *mt) {
    if (!mt) return;
    yq_memblk_reset(mt->arena);
    mt->num_entries = 0;
    mt->used_bytes = 0;
    runs_dirty(mt);
}
