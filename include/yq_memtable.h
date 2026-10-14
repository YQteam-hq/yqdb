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
/*
 * Enable or disable the max_bytes admission check on the write path.
 *
 * Crash recovery runs with the check disabled (see yq_recover.c): the log is
 * replayed into a fresh memtable, and a live dataset larger than
 * memtable_bytes must still be replayable. Before this switch existed such a
 * database failed to open at all -- a data-availability loss caused by a
 * memory-accounting threshold, which is strictly worse than exceeding the
 * threshold. Re-enabling after an overloaded replay grants one more threshold
 * of headroom, so the invariant the check relies on (used <= max) holds and
 * the database stays writable.
 */
void yq_memtable_set_limit_enforcement(yq_memtable *mt, int enforce);
/*
 * Bulk-load a run of entries that the caller has ALREADY ordered by key with
 * duplicates resolved, building the chunk layout directly instead of paying
 * a search + memmove per record. Crash recovery uses this: it scans the whole
 * log, sorts the committed operations once, and hands the engine the result
 * in a single call -- O(n log n) for the sort instead of O(n^2) worst case
 * for replaying into the chunked array one record at a time.
 *
 * Contract: the memtable must be empty; entries must be strictly increasing
 * by key with no duplicates (the recovery path guarantees this by keeping
 * the last operation per key). Keys are validated like yq_memtable_put().
 * The max_bytes admission check is not applied here -- recovery runs with it
 * lifted (see yq_memtable_set_limit_enforcement). Byte accounting uses the
 * clean per-entry formula, which for a deduplicated run is exactly the sum
 * the replay path would converge to.
 */
typedef struct yq_bulk_entry {
    yq_slice key;
    yq_slice val;        /* ignored when tombstone is set */
    int tombstone;
} yq_bulk_entry;

int yq_memtable_bulk_load(yq_memtable *mt, const yq_bulk_entry *entries,
                          size_t n);
/*
 * Ensure the arena can hold `bytes` more key/value data, growing it if
 * needed. Stored entries survive the grow (their offsets are preserved), so
 * this is safe to call on a memtable that already holds data.
 *
 * Crash recovery uses this to make room for the whole replayed log before
 * lifting the admission check: the arena starts at 2x memtable_bytes, and a
 * live set larger than that would otherwise fail the replay with
 * YQ_ERR_NOMEM even with the check lifted -- the database would stay
 * unopenable, which is exactly what this fix removes.
 */
int yq_memtable_reserve(yq_memtable *mt, size_t bytes);
/*
 * The memtable is an ordered skip list: O(log n) point operations and
 * O(log n) inserts, ordered iteration in both directions. It replaced a
 * sorted array whose inserts were O(n) memmoves.
 *
 * Iterator lifetime: a cursor stays usable across mutations of the table,
 * EXCEPT across yq_memtable_reset(), which discards every node. Iterators
 * record the table generation when they position and re-check it on every
 * access; after a reset() the accessors return YQ_ERR_CURSOR instead of
 * handing back bytes that no longer exist.
 */
int yq_memtable_iter_open(yq_memtable *mt, yq_memtable_iter **out);
void yq_memtable_iter_close(yq_memtable_iter *it);
int yq_memtable_iter_first(yq_memtable_iter *it);
int yq_memtable_iter_next(yq_memtable_iter *it);
int yq_memtable_iter_last(yq_memtable_iter *it);
int yq_memtable_iter_prev(yq_memtable_iter *it);
/*
 * Position the iterator at the first live key >= target (lower bound). On
 * success the iterator points at that entry; if every key is smaller than the
 * target the iterator is left invalid and YQ_ERR_NOTFOUND is returned. Uses
 * the same skip list descent as lookups, so it is O(log n) rather than the
 * O(n) linear scan a successive yq_memtable_iter_next() loop would need.
 * Tombstoned (deleted) entries at/after the position are skipped.
 */
int yq_memtable_iter_seek(yq_memtable_iter *it, yq_slice key);
int yq_memtable_iter_key(yq_memtable_iter *it, yq_slice *out);
int yq_memtable_iter_val(yq_memtable_iter *it, yq_slice *out);
int yq_memtable_iter_valid(yq_memtable_iter *it);
void yq_memtable_reset(yq_memtable *mt);

#endif
