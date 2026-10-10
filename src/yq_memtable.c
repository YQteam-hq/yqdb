#include "yq_memtable.h"
#include "yq_memblk.h"
#include "yq_enc.h"
#include "yq_slice.h"
#include <stdlib.h>
#include <string.h>

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

/*
 * Binary search for `key` over the sorted entry array.
 *
 * Returns 1 with *idx set to the matching entry, or 0 with *idx set to the
 * position where the key would be inserted.
 *
 * The comparison is done on raw key spans rather than by wrapping each entry
 * key in a yq_slice and calling yq_slice_compare(): this runs once per probe,
 * so an out-of-line call and two temporary slices per comparison dominate the
 * search. The arena base is hoisted out of the loop for the same reason.
 */
static int search_entry(yq_memtable *mt, const yq_slice *key, size_t *idx) {
    const uint8_t *base = (const uint8_t *)yq_memblk_base(mt->arena);
    const mt_entry *entries = mt->entries;
    size_t lo = 0, hi = mt->num_entries;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const mt_entry *e = &entries[mid];
        int cmp = yq_slice_compare_raw(key->data, key->size,
                                       base + e->key_offset, e->key_len);
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
    free(mt);
}

/*
 * Make room for one more entry. Called on the insert path of both put() and
 * del(), which had the same doubling logic inline.
 */
static int reserve_entry(yq_memtable *mt) {
    if (mt->num_entries < mt->cap_entries) return YQ_OK;

    size_t new_cap = mt->cap_entries * 2;
    mt_entry *new_entries = realloc(mt->entries, new_cap * sizeof(mt_entry));
    if (!new_entries) return YQ_ERR_NOMEM;

    mt->entries = new_entries;
    mt->cap_entries = new_cap;
    return YQ_OK;
}

/*
 * Open a gap at `idx` by shifting everything above it up one slot.
 *
 * memmove() rather than an element-by-element loop: the compiler cannot
 * assume the regions do not overlap here, and a hand-rolled loop of struct
 * assignments is what the insert path of a large memtable spends its time on.
 */
static void shift_entries_up(mt_entry *entries, size_t idx, size_t num_entries) {
    if (idx >= num_entries) return;
    memmove(&entries[idx + 1], &entries[idx],
            (num_entries - idx) * sizeof(*entries));
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
        return YQ_OK;
    }

    int rc = reserve_entry(mt);
    if (rc != YQ_OK) return rc;

    size_t koff = 0, voff = 0;
    rc = alloc_copy(mt, key.data, key.size, &koff);
    if (rc != YQ_OK) return rc;
    rc = alloc_copy(mt, val.data, val.size, &voff);
    if (rc != YQ_OK) return rc;

    shift_entries_up(mt->entries, idx, mt->num_entries);
    mt->num_entries++;

    mt_entry *e = &mt->entries[idx];
    e->key_offset = koff;
    e->key_len = key.size;
    e->val_offset = voff;
    e->val_len = val.size;
    e->tombstone = 0;
    mt->used_bytes += cost;

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
        return YQ_OK;
    }

    int rc = reserve_entry(mt);
    if (rc != YQ_OK) return rc;

    size_t koff = 0;
    rc = alloc_copy(mt, key.data, key.size, &koff);
    if (rc != YQ_OK) return rc;

    shift_entries_up(mt->entries, idx, mt->num_entries);
    mt->num_entries++;

    mt_entry *e = &mt->entries[idx];
    e->key_offset = koff;
    e->key_len = key.size;
    e->val_offset = 0;
    e->val_len = 0;
    e->tombstone = 1;
    mt->used_bytes += entry_cost(key.size, 0);

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
    it->pos = 0;

    while (it->pos < mt->num_entries && mt->entries[it->pos].tombstone) {
        it->pos++;
    }

    *out = it;
    return YQ_OK;
}

void yq_memtable_iter_close(yq_memtable_iter *it) {
    free(it);
}

int yq_memtable_iter_first(yq_memtable_iter *it) {
    if (!it || !it->mt) return YQ_ERR_INVAL;

    it->pos = 0;
    while (it->pos < it->mt->num_entries && it->mt->entries[it->pos].tombstone) {
        it->pos++;
    }

    if (it->pos >= it->mt->num_entries) {
        return YQ_ERR_NOTFOUND;
    }
    return YQ_OK;
}

int yq_memtable_iter_next(yq_memtable_iter *it) {
    if (!it || !it->mt) return YQ_ERR_INVAL;

    it->pos++;
    while (it->pos < it->mt->num_entries && it->mt->entries[it->pos].tombstone) {
        it->pos++;
    }

    if (it->pos >= it->mt->num_entries) {
        return YQ_ERR_NOTFOUND;
    }
    return YQ_OK;
}

int yq_memtable_iter_last(yq_memtable_iter *it) {
    if (!it || !it->mt) return YQ_ERR_INVAL;
    if (it->mt->num_entries == 0) { it->pos = 0; return YQ_ERR_NOTFOUND; }

    it->pos = it->mt->num_entries - 1;
    while (it->mt->entries[it->pos].tombstone) {
        if (it->pos == 0) { it->pos = it->mt->num_entries; return YQ_ERR_NOTFOUND; }
        it->pos--;
    }
    return YQ_OK;
}

int yq_memtable_iter_prev(yq_memtable_iter *it) {
    if (!it || !it->mt) return YQ_ERR_INVAL;
    if (it->pos == 0) return YQ_ERR_NOTFOUND;

    it->pos--;
    while (it->mt->entries[it->pos].tombstone) {
        if (it->pos == 0) return YQ_ERR_NOTFOUND;
        it->pos--;
    }
    return YQ_OK;
}

/*
 * Lower bound: first entry with key >= target, then past any tombstones.
 *
 * search_entry() returns the insertion position, which is the lowest index
 * whose key is >= target -- tombstones included, since they carry their key
 * and sit at their sorted position. Every index below it holds a smaller key,
 * so no entry there can satisfy the seek no matter whether it is a tombstone;
 * starting the tombstone skip from there therefore lands on exactly the entry
 * a linear scan from iter_first() would have stopped at.
 */
int yq_memtable_iter_seek(yq_memtable_iter *it, yq_slice target) {
    if (!it || !it->mt) return YQ_ERR_INVAL;

    size_t idx;
    search_entry(it->mt, &target, &idx);

    it->pos = idx;
    while (it->pos < it->mt->num_entries && it->mt->entries[it->pos].tombstone) {
        it->pos++;
    }

    if (it->pos >= it->mt->num_entries) {
        return YQ_ERR_NOTFOUND;
    }
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
}
