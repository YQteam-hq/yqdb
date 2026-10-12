#ifndef YQ_MEMTABLE_H
#define YQ_MEMTABLE_H
#include <stddef.h>
#include <stdint.h>
#include "yq.h"

typedef struct yq_memtable yq_memtable;
typedef struct yq_memtable_iter yq_memtable_iter;

yq_memtable *yq_memtable_create(size_t max_bytes);
void yq_memtable_destroy(yq_memtable *mt);
int yq_memtable_put(yq_memtable *mt, yq_slice key, yq_slice val);
int yq_memtable_del(yq_memtable *mt, yq_slice key);
int yq_memtable_get(yq_memtable *mt, yq_slice key, yq_slice *out);
int yq_memtable_size(yq_memtable *mt);
int yq_memtable_full(yq_memtable *mt);
size_t yq_memtable_bytes(yq_memtable *mt);
int yq_memtable_iter_open(yq_memtable *mt, yq_memtable_iter **out);
void yq_memtable_iter_close(yq_memtable_iter *it);
int yq_memtable_iter_first(yq_memtable_iter *it);
int yq_memtable_iter_next(yq_memtable_iter *it);
int yq_memtable_iter_last(yq_memtable_iter *it);
int yq_memtable_iter_prev(yq_memtable_iter *it);
/*
 * Position the iterator at the first live key >= target (lower bound). On
 * success the iterator points at that entry; if every key is smaller than the
 * target the iterator is left invalid and YQ_ERR_NOTFOUND is returned. Uses the
 * same binary search as lookups, so it is O(log n) rather than the O(n) linear
 * scan a successive yq_memtable_iter_next() loop would need. Tombstoned
 * (deleted) entries at/after the position are skipped.
 */
int yq_memtable_iter_seek(yq_memtable_iter *it, yq_slice key);
int yq_memtable_iter_key(yq_memtable_iter *it, yq_slice *out);
int yq_memtable_iter_val(yq_memtable_iter *it, yq_slice *out);
int yq_memtable_iter_valid(yq_memtable_iter *it);
void yq_memtable_reset(yq_memtable *mt);

#endif
