#include "yq_memtable.h"
#include "yq_memblk.h"
#include "yq_enc.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
#define memory_barrier() MemoryBarrier()
#else
#include <stdatomic.h>
#include <unistd.h>
#define memory_barrier() __sync_synchronize()
#endif

#define TOMBSTONE_VAL 0xFF

typedef struct mt_entry {
    size_t key_offset;
    size_t key_len;
    size_t val_offset;
    size_t val_len;
    int tombstone;
} mt_entry;

/* Performance optimization: cache-friendly index for iteration */
typedef struct memtable_index {
    mt_entry **entry_pointers;  /* Array of pointers for sequential access */
    size_t *key_offsets;        /* Precomputed key offsets for sorting */
    size_t sorted_count;        /* Number of valid entries in index */
    size_t capacity;           /* Current capacity of index arrays */
    uint32_t generation;        /* Generation counter for cache invalidation */
} memtable_index;

struct yq_memtable {
    yq_memblk *arena;
    mt_entry *entries;
    atomic_size_t num_entries;
    atomic_size_t cap_entries;
    atomic_size_t max_bytes;
    atomic_size_t used_bytes;
    volatile uint32_t generation;
    volatile uint32_t active_writers;
    
    /* Performance optimization: iteration index */
    memtable_index index;
    int index_valid;            /* Flag to track index validity */
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

yq_memtable *yq_memtable_create(size_t max_bytes) {
    if (max_bytes == 0) max_bytes = 64 * 1024 * 1024;
    if (max_bytes > (1ULL << 30)) return NULL; /* 1GB limit */

    yq_memtable *mt = calloc(1, sizeof(yq_memtable));
    if (!mt) return NULL;

    mt->arena = yq_memblk_create(max_bytes * 2);
    if (!mt->arena) {
        free(mt);
        return NULL;
    }

    atomic_init(&mt->max_bytes, max_bytes);
    atomic_init(&mt->num_entries, 0);
    atomic_init(&mt->cap_entries, 256);
    mt->generation = 1;
    mt->active_writers = 0;
    
    mt->entries = calloc(256, sizeof(mt_entry));
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

int yq_memtable_put(yq_memtable *mt, yq_slice key, yq_slice val) {
    if (!mt || !key.data || key.size == 0 || key.size > 1024) return YQ_ERR_INVAL;
    if (val.size > (1ULL << 30)) return YQ_ERR_TOOBIG; /* 1GB limit */
    
    /* Thread safety: acquire writer lock */
    if (atomic_fetch_add(&mt->active_writers, 1) != 0) {
        /* Another writer is active, wait or fail */
        atomic_fetch_add(&mt->active_writers, -1);
        return YQ_ERR_BUSY;
    }

    size_t cost = entry_cost(key.size, val.size);
    size_t current_used = atomic_load(&mt->used_bytes);
    size_t current_max = atomic_load(&mt->max_bytes);
    if (current_used + cost > current_max) {
        atomic_fetch_add(&mt->active_writers, -1);
        return YQ_ERR_NOMEM;
    }

    size_t idx;
    int found = search_entry(mt, &key, &idx);

    if (found) {
        mt_entry *e = &mt->entries[idx];
        size_t old_cost = 0;
        if (!e->tombstone) {
            old_cost = entry_cost(e->key_len, e->val_len);
            current_used -= old_cost;
        }
        size_t voff = 0;
        int rc = alloc_copy(mt, val.data, val.size, &voff);
        if (rc != YQ_OK) {
            atomic_fetch_add(&mt->active_writers, -1);
            return rc;
        }
        e->val_offset = voff;
        e->val_len = val.size;
        e->tombstone = 0;
        current_used += cost;
        atomic_store(&mt->used_bytes, current_used);
        mt->generation++;
        invalidate_memtable_index(mt);  /* Invalidate index on modification */
        memory_barrier();
        atomic_fetch_add(&mt->active_writers, -1);
        return YQ_OK;
    }

    size_t current_num = atomic_load(&mt->num_entries);
    size_t current_cap = atomic_load(&mt->cap_entries);
    if (current_num >= current_cap) {
        size_t new_cap = current_cap * 2;
        if (new_cap < current_cap || new_cap > SIZE_MAX / sizeof(mt_entry)) {
            atomic_fetch_add(&mt->active_writers, -1);
            return YQ_ERR_NOMEM;
        }
        mt_entry *new_entries = realloc(mt->entries, new_cap * sizeof(mt_entry));
        if (!new_entries) {
            atomic_fetch_add(&mt->active_writers, -1);
            return YQ_ERR_NOMEM;
        }
        mt->entries = new_entries;
        atomic_store(&mt->cap_entries, new_cap);
    }

    size_t koff = 0, voff = 0;
    int rc = alloc_copy(mt, key.data, key.size, &koff);
    if (rc != YQ_OK) {
        atomic_fetch_add(&mt->active_writers, -1);
        return rc;
    }
    rc = alloc_copy(mt, val.data, val.size, &voff);
    if (rc != YQ_OK) {
        atomic_fetch_add(&mt->active_writers, -1);
        return rc;
    }

    /* Insert with proper bounds checking */
    if (current_num < mt->cap_entries) {
        for (size_t i = current_num; i > idx; i--) {
            mt->entries[i] = mt->entries[i - 1];
        }
        mt->entries[idx].key_offset = koff;
        mt->entries[idx].key_len = key.size;
        mt->entries[idx].val_offset = voff;
        mt->entries[idx].val_len = val.size;
        mt->entries[idx].tombstone = 0;
        atomic_store(&mt->num_entries, current_num + 1);
        current_used += cost;
        atomic_store(&mt->used_bytes, current_used);
        mt->generation++;
        invalidate_memtable_index(mt);  /* Invalidate index on modification */
        memory_barrier();
        atomic_fetch_add(&mt->active_writers, -1);
        return YQ_OK;
    }

    atomic_fetch_add(&mt->active_writers, -1);
    return YQ_ERR_NOMEM;
}

int yq_memtable_del(yq_memtable *mt, yq_slice key) {
    if (!mt || !key.data || key.size == 0 || key.size > 1024) return YQ_ERR_INVAL;
    
    /* Thread safety: acquire writer lock */
    if (atomic_fetch_add(&mt->active_writers, 1) != 0) {
        /* Another writer is active, wait or fail */
        atomic_fetch_add(&mt->active_writers, -1);
        return YQ_ERR_BUSY;
    }

    size_t idx;
    int found = search_entry(mt, &key, &idx);

    if (found) {
        mt_entry *e = &mt->entries[idx];
        if (e->tombstone) {
            atomic_fetch_add(&mt->active_writers, -1);
            return YQ_OK;
        }
        size_t old_cost = entry_cost(e->key_len, e->val_len);
        size_t current_used = atomic_load(&mt->used_bytes);
        current_used -= old_cost;
        e->tombstone = 1;
        e->val_offset = 0;
        e->val_len = 0;
        current_used += entry_cost(e->key_len, 0);
        atomic_store(&mt->used_bytes, current_used);
        mt->generation++;
        memory_barrier();
        atomic_fetch_add(&mt->active_writers, -1);
        return YQ_OK;
    }

    size_t current_num = atomic_load(&mt->num_entries);
    size_t current_cap = atomic_load(&mt->cap_entries);
    if (current_num >= current_cap) {
        size_t new_cap = current_cap * 2;
        if (new_cap < current_cap || new_cap > SIZE_MAX / sizeof(mt_entry)) {
            atomic_fetch_add(&mt->active_writers, -1);
            return YQ_ERR_NOMEM;
        }
        mt_entry *new_entries = realloc(mt->entries, new_cap * sizeof(mt_entry));
        if (!new_entries) {
            atomic_fetch_add(&mt->active_writers, -1);
            return YQ_ERR_NOMEM;
        }
        mt->entries = new_entries;
        atomic_store(&mt->cap_entries, new_cap);
    }

    size_t koff = 0;
    int rc = alloc_copy(mt, key.data, key.size, &koff);
    if (rc != YQ_OK) {
        atomic_fetch_add(&mt->active_writers, -1);
        return rc;
    }

    /* Insert with proper bounds checking */
    if (current_num < mt->cap_entries) {
        for (size_t i = current_num; i > idx; i--) {
            mt->entries[i] = mt->entries[i - 1];
        }
        mt->entries[idx].key_offset = koff;
        mt->entries[idx].key_len = key.size;
        mt->entries[idx].val_offset = 0;
        mt->entries[idx].val_len = 0;
        mt->entries[idx].tombstone = 1;
        atomic_store(&mt->num_entries, current_num + 1);
        size_t current_used = atomic_load(&mt->used_bytes);
        current_used += entry_cost(key.size, 0);
        atomic_store(&mt->used_bytes, current_used);
        mt->generation++;
        memory_barrier();
        atomic_fetch_add(&mt->active_writers, -1);
        return YQ_OK;
    }

    atomic_fetch_add(&mt->active_writers, -1);
    return YQ_ERR_NOMEM;
}

int yq_memtable_get(yq_memtable *mt, yq_slice key, yq_slice *out) {
    if (!mt || !out || !key.data || key.size == 0) return YQ_ERR_INVAL;
    if (key.size > 1024) return YQ_ERR_INVAL;
    
    /* Thread safety: acquire reader lock (check for active writers) */
    size_t writers = atomic_load(&mt->active_writers);
    while (writers > 0) {
        memory_barrier();
        writers = atomic_load(&mt->active_writers);
    }
    
    size_t idx;
    int found = search_entry(mt, &key, &idx);
    if (!found) return YQ_ERR_NOTFOUND;

    mt_entry *e = &mt->entries[idx];
    if (e->tombstone) return YQ_ERR_NOTFOUND;

    uint8_t *base = (uint8_t *)yq_memblk_base(mt->arena);
    if (!base) return YQ_ERR_CORRUPT;
    
    /* Validate pointer bounds */
    if (e->val_offset + e->val_len > yq_memblk_size(mt->arena)) {
        return YQ_ERR_CORRUPT;
    }
    
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
    if (!mt || !out) return YQ_ERR_INVAL;

    /* Thread safety: acquire reader lock */
    size_t writers = atomic_load(&mt->active_writers);
    while (writers > 0) {
        memory_barrier();
        writers = atomic_load(&mt->active_writers);
    }

    yq_memtable_iter *it = calloc(1, sizeof(yq_memtable_iter));
    if (!it) return YQ_ERR_NOMEM;

    it->mt = mt;
    it->pos = 0;

    /* Skip tombstones with bounds checking */
    size_t num_entries = atomic_load(&mt->num_entries);
    while (it->pos < num_entries && mt->entries[it->pos].tombstone) {
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
    
    /* Re-check for active writers */
    size_t writers = atomic_load(&it->mt->active_writers);
    while (writers > 0) {
        memory_barrier();
        writers = atomic_load(&it->mt->active_writers);
    }

    size_t num_entries = atomic_load(&it->mt->num_entries);
    if (it->pos >= num_entries) return YQ_ERR_NOTFOUND;

    it->pos = 0;
    while (it->pos < num_entries && it->mt->entries[it->pos].tombstone) {
        it->pos++;
    }

    if (it->pos >= num_entries) {
        return YQ_ERR_NOTFOUND;
    }
    return YQ_OK;
}

int yq_memtable_iter_next(yq_memtable_iter *it) {
    if (!it || !it->mt) return YQ_ERR_INVAL;
    if (it->pos >= it->mt->num_entries) return YQ_ERR_NOTFOUND;

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
    if (it->pos >= it->mt->num_entries) return YQ_ERR_NOTFOUND;

    it->pos = it->mt->num_entries - 1;
    while (it->mt->entries[it->pos].tombstone) {
        if (it->pos == 0) { it->pos = it->mt->num_entries; return YQ_ERR_NOTFOUND; }
        it->pos--;
    }
    return YQ_OK;
}

int yq_memtable_iter_prev(yq_memtable_iter *it) {
    if (!it || !it->mt) return YQ_ERR_INVAL;
    if (it->pos >= it->mt->num_entries) return YQ_ERR_NOTFOUND;
    if (it->pos == 0) return YQ_ERR_NOTFOUND;

    it->pos--;
    while (it->mt->entries[it->pos].tombstone) {
        if (it->pos == 0) return YQ_ERR_NOTFOUND;
        it->pos--;
    }
    return YQ_OK;
}

int yq_memtable_iter_key(yq_memtable_iter *it, yq_slice *out) {
    if (!it || !it->mt || !out) return YQ_ERR_INVAL;
    
    size_t num_entries = atomic_load(&it->mt->num_entries);
    if (it->pos >= num_entries) {
        return YQ_ERR_CURSOR;
    }

    mt_entry *e = &it->mt->entries[it->pos];
    uint8_t *base = (uint8_t *)yq_memblk_base(it->mt->arena);
    if (!base) return YQ_ERR_CORRUPT;
    
    /* Validate pointer bounds */
    if (e->key_offset + e->key_len > yq_memblk_size(it->mt->arena)) {
        return YQ_ERR_CORRUPT;
    }
    
    yq_slice_set(out, base + e->key_offset, e->key_len);
    return YQ_OK;
}

int yq_memtable_iter_val(yq_memtable_iter *it, yq_slice *out) {
    if (!it || !it->mt || !out) return YQ_ERR_INVAL;
    
    size_t num_entries = atomic_load(&it->mt->num_entries);
    if (it->pos >= num_entries) {
        return YQ_ERR_CURSOR;
    }

    mt_entry *e = &it->mt->entries[it->pos];
    uint8_t *base = (uint8_t *)yq_memblk_base(it->mt->arena);
    if (!base) return YQ_ERR_CORRUPT;
    
    /* Validate pointer bounds */
    if (e->val_offset + e->val_len > yq_memblk_size(it->mt->arena)) {
        return YQ_ERR_CORRUPT;
    }
    
    yq_slice_set(out, base + e->val_offset, e->val_len);
    return YQ_OK;
}

int yq_memtable_iter_valid(yq_memtable_iter *it) {
    if (!it || !it->mt) return 0;
    return it->pos < it->mt->num_entries;
}

int yq_memtable_iter_seek(yq_memtable_iter *it, yq_slice key) {
    if (!it || !it->mt || !key.data || key.size == 0) return YQ_ERR_INVAL;
    if (key.size > 1024) return YQ_ERR_INVAL;
    
    /* Re-check for active writers */
    size_t writers = atomic_load(&it->mt->active_writers);
    while (writers > 0) {
        memory_barrier();
        writers = atomic_load(&it->mt->active_writers);
    }

    size_t num_entries = atomic_load(&it->mt->num_entries);
    if (it->pos >= num_entries) return YQ_ERR_NOTFOUND;
    
    size_t lo = 0, hi = num_entries;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const mt_entry *e = &it->mt->entries[mid];
        const uint8_t *base = (const uint8_t *)yq_memblk_base(it->mt->arena);
        if (!base) return YQ_ERR_CORRUPT;
        
        /* Validate pointer bounds */
        if (e->key_offset + e->key_len > yq_memblk_size(it->mt->arena)) {
            return YQ_ERR_CORRUPT;
        }
        
        const uint8_t *ek = base + e->key_offset;
        yq_slice ek_slice;
        yq_slice_set(&ek_slice, ek, e->key_len);
        int cmp = yq_slice_compare(&key, &ek_slice);
        if (cmp == 0) {
            it->pos = mid;
            return YQ_OK;
        } else if (cmp < 0) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    it->pos = lo;
    if (it->pos >= num_entries) return YQ_ERR_NOTFOUND;
    if (it->mt->entries[it->pos].tombstone) return YQ_ERR_NOTFOUND;
    return YQ_OK;
}

void yq_memtable_reset(yq_memtable *mt) {
    if (!mt) return;
    
    /* Thread safety: acquire writer lock */
    if (atomic_fetch_add(&mt->active_writers, 1) != 0) {
        /* Another writer is active, wait or fail */
        atomic_fetch_add(&mt->active_writers, -1);
        return;
    }
    
    yq_memblk_reset(mt->arena);
    atomic_store(&mt->num_entries, 0);
    atomic_store(&mt->used_bytes, 0);
    mt->generation++;
    mt->index_valid = 0;  /* Invalidate index on reset */
    memory_barrier();
    atomic_fetch_add(&mt->active_writers, -1);
}

/* Performance optimization: comparison function for sorting entries */
static int compare_entries_by_key(const void *a, const void *b) {
    const mt_entry *entry_a = *(const mt_entry **)a;
    const mt_entry *entry_b = *(const mt_entry **)b;
    uint8_t *base = (uint8_t *)yq_memblk_base(((yq_memtable *)entry_a)->arena);
    
    size_t offset_a = entry_a->key_offset;
    size_t offset_b = entry_b->key_offset;
    size_t len_a = entry_a->key_len;
    size_t len_b = entry_b->key_len;
    
    /* Compare by key content */
    size_t min_len = len_a < len_b ? len_a : len_b;
    int cmp = memcmp(base + offset_a, base + offset_b, min_len);
    if (cmp != 0) return cmp;
    return (int)len_a - (int)len_b;
}

/* Performance optimization: build index for efficient iteration */
static void build_memtable_index(yq_memtable *mt) {
    size_t num_entries = atomic_load(&mt->num_entries);
    
    if (num_entries == 0) {
        mt->index.sorted_count = 0;
        mt->index_valid = 1;
        return;
    }
    
    /* Resize index arrays if needed */
    if (num_entries > mt->index.capacity) {
        size_t new_capacity = num_entries * 2;
        
        mt_entry **new_pointers = realloc(mt->index.entry_pointers, 
                                        new_capacity * sizeof(mt_entry *));
        size_t *new_offsets = realloc(mt->index.key_offsets,
                                    new_capacity * sizeof(size_t));
        
        if (!new_pointers || !new_offsets) {
            /* Allocation failed, keep old index */
            return;
        }
        
        mt->index.entry_pointers = new_pointers;
        mt->index.key_offsets = new_offsets;
        mt->index.capacity = new_capacity;
    }
    
    /* Build index arrays */
    for (size_t i = 0; i < num_entries; i++) {
        mt->index.entry_pointers[i] = &mt->entries[i];
        mt->index.key_offsets[i] = mt->entries[i].key_offset;
    }
    
    /* Sort entries by key for sequential access */
    qsort(mt->index.entry_pointers, num_entries, sizeof(mt_entry *), 
          compare_entries_by_key);
    
    mt->index.sorted_count = num_entries;
    mt->index.generation = mt->generation;
    mt->index_valid = 1;
}

/* Performance optimization: invalidate index when entries are modified */
static void invalidate_memtable_index(yq_memtable *mt) {
    mt->index_valid = 0;
}

/* Performance optimization: get entry from sorted index */
static mt_entry* get_indexed_entry(yq_memtable *mt, size_t index) {
    if (!mt->index_valid || index >= mt->index.sorted_count) {
        return NULL;
    }
    
    /* Check if index is still valid */
    if (mt->index.generation != mt->generation) {
        return NULL;
    }
    
    return mt->index.entry_pointers[index];
}
