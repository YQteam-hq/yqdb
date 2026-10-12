#ifndef YQ_MVCC_H
#define YQ_MVCC_H
#include <stddef.h>
#include <stdint.h>
#include "yq.h"
#include "yq_vfs.h"

typedef struct yq_mvcc yq_mvcc;

/*
 * page_size is the database page size from yq_opts. The two meta blocks live
 * in page 0 and page 1 of the database file, so their offsets depend on it;
 * passing a hardcoded 4096 put meta block 1 in the middle of page 0 for every
 * other page size.
 */
int yq_mvcc_open(yq_mvcc **out, yq_file *db_file, yq_file *shm_file, yq_file *lock_file,
                 uint32_t max_readers, uint32_t page_size);
int yq_mvcc_close(yq_mvcc *mvcc);
int yq_mvcc_acquire_snapshot(yq_mvcc *mvcc, uint64_t txn_id, uint64_t root_page,
    uint64_t *snapshot_txn, uint64_t *snapshot_root, int *slot_idx);
int yq_mvcc_release_snapshot(yq_mvcc *mvcc, int slot_idx);
uint64_t yq_mvcc_reclaim_watermark(yq_mvcc *mvcc);
int yq_mvcc_scan_slots(yq_mvcc *mvcc, void (*cb)(void *ctx, int slot_idx, uint64_t txn_id), void *ctx);
uint32_t yq_mvcc_active_readers(yq_mvcc *mvcc);
uint64_t yq_mvcc_txn_id(yq_mvcc *mvcc);
int yq_mvcc_increment_txn_id(yq_mvcc *mvcc, uint64_t *out);
void yq_mvcc_set_base_txn(yq_mvcc *mvcc, uint64_t base_txn);
int yq_mvcc_meta_read(yq_mvcc *mvcc, uint64_t *txn_id, uint64_t *root_page, uint64_t *free_head, uint64_t *npages, uint64_t *ckpt_lsn);
/*
 * Single-pass variant of yq_mvcc_meta_read() for the commit path. It reads the
 * two meta pages once, fills the same output fields, and additionally reports
 * *write_index -- the slot the following meta write should target (the one that
 * is not the newest valid copy). yq_txn_commit previously called
 * yq_mvcc_meta_read() and then yq_mvcc_meta_pwrite_full(), which each read both
 * pages, i.e. four page reads per commit; this collapses them to two.
 * write_index is set even when the pages are unreadable (both bad -> slot 0).
 */
int yq_mvcc_meta_read_select(yq_mvcc *mvcc, uint64_t *txn_id, uint64_t *root_page,
                             uint64_t *free_head, uint64_t *npages, uint64_t *ckpt_lsn,
                             uint8_t *write_index);
int yq_mvcc_meta_write(yq_mvcc *mvcc, uint64_t txn_id, uint64_t root_page, uint64_t free_head, uint64_t npages, uint64_t ckpt_lsn);
int yq_mvcc_meta_pwrite(yq_mvcc *mvcc, uint8_t meta_index);
int yq_mvcc_meta_pwrite_full(yq_mvcc *mvcc, uint64_t txn_id, uint64_t root_page,
                              uint64_t free_head, uint64_t npages, uint64_t ckpt_lsn);
/* Write the meta block to an explicit slot, skipping the read/select step. */
int yq_mvcc_meta_pwrite_at(yq_mvcc *mvcc, uint8_t meta_index, uint64_t txn_id, uint64_t root_page,
                           uint64_t free_head, uint64_t npages, uint64_t ckpt_lsn);
int yq_mvcc_elect_writer(yq_mvcc *mvcc, int wait_ms, int *got_it);
int yq_mvcc_release_writer(yq_mvcc *mvcc);
int yq_mvcc_seqlock_read(yq_mvcc *mvcc, void *dst, size_t off, size_t len);

#endif
