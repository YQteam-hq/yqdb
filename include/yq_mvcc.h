#ifndef YQ_MVCC_H
#define YQ_MVCC_H
#include <stddef.h>
#include <stdint.h>
#include "yq.h"
#include "yq_vfs.h"

typedef struct yq_mvcc yq_mvcc;

int yq_mvcc_open(yq_mvcc **out, yq_file *db_file, yq_file *shm_file, yq_file *lock_file, uint32_t max_readers);
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
int yq_mvcc_meta_write(yq_mvcc *mvcc, uint64_t txn_id, uint64_t root_page, uint64_t free_head, uint64_t npages, uint64_t ckpt_lsn);
int yq_mvcc_meta_pwrite(yq_mvcc *mvcc, uint8_t meta_index);
int yq_mvcc_meta_pwrite_full(yq_mvcc *mvcc, uint64_t txn_id, uint64_t root_page,
                              uint64_t free_head, uint64_t npages, uint64_t ckpt_lsn);
int yq_mvcc_elect_writer(yq_mvcc *mvcc, int wait_ms, int *got_it);
int yq_mvcc_release_writer(yq_mvcc *mvcc);
int yq_mvcc_seqlock_read(yq_mvcc *mvcc, void *dst, size_t off, size_t len);

#endif
