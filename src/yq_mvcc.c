#if !defined(_WIN32)
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "yq_mvcc.h"
#include "yq_vfs.h"
#include "yq_enc.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

#if defined(_WIN32)
#include <windows.h>
#define barrier() _ReadWriteBarrier()
static uint32_t yq_mvcc_current_pid(void) { return (uint32_t)GetCurrentProcessId(); }
static uint32_t yq_mvcc_current_tid(void) { return (uint32_t)GetCurrentThreadId(); }
#else
#include <stdatomic.h>
#include <unistd.h>
#define barrier() __asm__ __volatile__("" ::: "memory")
static uint32_t yq_mvcc_current_pid(void) { return (uint32_t)getpid(); }
static uint32_t yq_mvcc_current_tid(void) { return 0; }
#endif

#define YQ_SHM_MAGIC 0x59514853U
#define YQ_SHM_VERSION 1U
#define YQ_SHM_HEADER_SIZE 64
#define YQ_SLOT_SIZE 64
#define YQ_META_BLOCK_OFF 16
#define YQ_META_BLOCK_LEN 48

#define YQ_MAGIC_0 0x42445159
#define YQ_MAGIC_1 0x00010A1A

#pragma pack(push, 1)
typedef struct {
    uint32_t shm_magic;
    uint32_t shm_version;
    uint64_t shm_epoch;
    uint64_t slot_count;
    uint8_t  reserved[40];
} shm_header;
#pragma pack(pop)

typedef struct {
    uint32_t pid;
    uint32_t tid;
    uint64_t snapshot_txn;
    uint64_t snapshot_root_page;
    uint32_t active;
    uint32_t slot_epoch;
    uint8_t  reserved[32];
} shm_slot;

#pragma pack(push, 1)
typedef struct {
    uint64_t magic;
    uint32_t format_version;
    uint32_t page_size;
    uint64_t txn_id;
    uint64_t root_page;
    uint64_t free_head;
    uint64_t npages;
    uint64_t ckpt_lsn;
    uint64_t log_trunc_lsn;
    uint64_t meta_seq;
    uint64_t reserved0;
    uint32_t node_encoding;
    uint32_t flags;
    uint64_t reserved1;
    uint32_t header_crc32c;
} meta_block;
#pragma pack(pop)

struct yq_mvcc {
    yq_file *db_file;
    yq_file *shm_file;
    yq_file *lock_file;
    void *shm_base;
    size_t shm_size;
    uint32_t max_readers;
    uint32_t page_size;
    volatile uint32_t meta_seq;
    uint8_t meta_buf[4096];
};

int yq_mvcc_open(yq_mvcc **out, yq_file *db_file, yq_file *shm_file, yq_file *lock_file, uint32_t max_readers) {
    yq_mvcc *mvcc = calloc(1, sizeof(yq_mvcc));
    if (!mvcc) return YQ_ERR_NOMEM;

    mvcc->db_file = db_file;
    mvcc->shm_file = shm_file;
    mvcc->lock_file = lock_file;
    mvcc->max_readers = max_readers;
    mvcc->page_size = 4096;
    mvcc->meta_seq = 0;

    size_t shm_size = YQ_SHM_HEADER_SIZE + (size_t)max_readers * YQ_SLOT_SIZE;
    uint64_t fsize = yq_file_size(shm_file);
    int shm_created = 0;

    if (fsize == 0) {
        shm_created = 1;
        if (yq_file_truncate(shm_file, shm_size) != YQ_OK) { free(mvcc); return YQ_ERR_IO; }
    } else if ((size_t)fsize < shm_size) {
        if (yq_file_truncate(shm_file, shm_size) != YQ_OK) { free(mvcc); return YQ_ERR_IO; }
    }

    mvcc->shm_size = shm_size;
    mvcc->shm_base = yq_file_mmap(shm_file, 0, shm_size);
    if (!mvcc->shm_base) { free(mvcc); return YQ_ERR_IO; }

    if (shm_created) {
        shm_header *hdr = (shm_header *)mvcc->shm_base;
        hdr->shm_magic = YQ_SHM_MAGIC;
        hdr->shm_version = YQ_SHM_VERSION;
        hdr->shm_epoch = 1;
        hdr->slot_count = max_readers;
        memset(hdr->reserved, 0, sizeof(hdr->reserved));
        shm_slot *slots = (shm_slot *)((uint8_t *)mvcc->shm_base + YQ_SHM_HEADER_SIZE);
        for (uint32_t i = 0; i < max_readers; i++) {
            slots[i].pid = 0;
            slots[i].tid = 0;
            slots[i].snapshot_txn = UINT64_MAX;
            slots[i].snapshot_root_page = 0;
            slots[i].active = 0;
            slots[i].slot_epoch = 1;
            memset(slots[i].reserved, 0, sizeof(slots[i].reserved));
        }
    } else {
        shm_header *hdr = (shm_header *)mvcc->shm_base;
        if (hdr->shm_magic != YQ_SHM_MAGIC) { yq_file_munmap(mvcc->shm_base, mvcc->shm_size); free(mvcc); return YQ_ERR_CORRUPT; }
        if (hdr->shm_version != YQ_SHM_VERSION) { yq_file_munmap(mvcc->shm_base, mvcc->shm_size); free(mvcc); return YQ_ERR_VERSION; }
    }

    *out = mvcc;
    return YQ_OK;
}

int yq_mvcc_close(yq_mvcc *mvcc) {
    if (!mvcc) return YQ_OK;
    if (mvcc->shm_base) yq_file_munmap(mvcc->shm_base, mvcc->shm_size);
    free(mvcc);
    return YQ_OK;
}

int yq_mvcc_acquire_snapshot(yq_mvcc *mvcc, uint64_t txn_id, uint64_t root_page,
    uint64_t *snapshot_txn, uint64_t *snapshot_root, int *slot_idx) {
    shm_slot *slots = (shm_slot *)((uint8_t *)mvcc->shm_base + YQ_SHM_HEADER_SIZE);
    shm_header *hdr = (shm_header *)mvcc->shm_base;

    for (uint32_t i = 0; i < mvcc->max_readers; i++) {
        uint32_t expected = 0;
        int acquired = 0;
#if defined(_WIN32)
        /* InterlockedCompareExchange returns the *previous* value, so a match
         * with the expected (free) value means this caller claimed the slot. */
        LONG prev = InterlockedCompareExchange((volatile LONG *)&slots[i].active, 1, (LONG)expected);
        acquired = (prev == (LONG)expected);
#else
        /*
         * atomic_compare_exchange_strong returns a bool telling whether the
         * swap happened -- it does NOT return the previous value. Comparing
         * that bool against `expected` (which still holds the pre-swap value
         * 0 on success) inverted the test, so the success branch was never
         * entered and every snapshot acquisition fell through to
         * YQ_ERR_READER_FULL.
         */
        acquired = atomic_compare_exchange_strong((volatile atomic_uint *)&slots[i].active,
                                                  &expected, 1) ? 1 : 0;
#endif
        if (acquired) {
            slots[i].pid = yq_mvcc_current_pid();
            slots[i].tid = yq_mvcc_current_tid();
            slots[i].snapshot_txn = txn_id;
            slots[i].snapshot_root_page = root_page;
            slots[i].slot_epoch = (uint32_t)hdr->shm_epoch;

            *snapshot_txn = txn_id;
            *snapshot_root = root_page;
            *slot_idx = (int)i;
            return YQ_OK;
        }
    }
    return YQ_ERR_READER_FULL;
}

int yq_mvcc_release_snapshot(yq_mvcc *mvcc, int slot_idx) {
    if (slot_idx < 0 || (uint32_t)slot_idx >= mvcc->max_readers) return YQ_ERR_INVAL;

    shm_slot *slots = (shm_slot *)((uint8_t *)mvcc->shm_base + YQ_SHM_HEADER_SIZE);
    slots[slot_idx].snapshot_txn = UINT64_MAX;
    slots[slot_idx].active = 0;
    return YQ_OK;
}

uint64_t yq_mvcc_reclaim_watermark(yq_mvcc *mvcc) {
    uint64_t min_txn = UINT64_MAX;
    shm_slot *slots = (shm_slot *)((uint8_t *)mvcc->shm_base + YQ_SHM_HEADER_SIZE);

    for (uint32_t i = 0; i < mvcc->max_readers; i++) {
        if (slots[i].active && slots[i].snapshot_txn != UINT64_MAX) {
            if (slots[i].snapshot_txn < min_txn) {
                min_txn = slots[i].snapshot_txn;
            }
        }
    }
    return min_txn;
}

int yq_mvcc_scan_slots(yq_mvcc *mvcc, void (*cb)(void *ctx, int slot_idx, uint64_t txn_id), void *ctx) {
    shm_slot *slots = (shm_slot *)((uint8_t *)mvcc->shm_base + YQ_SHM_HEADER_SIZE);

    for (uint32_t i = 0; i < mvcc->max_readers; i++) {
        if (slots[i].active) {
            cb(ctx, (int)i, slots[i].snapshot_txn);
        }
    }
    return YQ_OK;
}

uint32_t yq_mvcc_active_readers(yq_mvcc *mvcc) {
    uint32_t count = 0;
    shm_slot *slots = (shm_slot *)((uint8_t *)mvcc->shm_base + YQ_SHM_HEADER_SIZE);

    for (uint32_t i = 0; i < mvcc->max_readers; i++) {
        if (slots[i].active) count++;
    }
    return count;
}

uint64_t yq_mvcc_txn_id(yq_mvcc *mvcc) {
    (void)mvcc;
    return 0;
}

void yq_mvcc_set_base_txn(yq_mvcc *mvcc, uint64_t base_txn) {
    uint32_t b = (uint32_t)base_txn;
    if (b > mvcc->meta_seq) mvcc->meta_seq = b;
}

int yq_mvcc_increment_txn_id(yq_mvcc *mvcc, uint64_t *out) {
    uint32_t old_seq = mvcc->meta_seq;
    uint32_t new_seq = old_seq + 1;
#if defined(_WIN32)
    old_seq = InterlockedCompareExchange((volatile LONG *)&mvcc->meta_seq, new_seq, old_seq);
    while (old_seq != new_seq - 1) {
        new_seq = old_seq + 1;
        old_seq = InterlockedCompareExchange((volatile LONG *)&mvcc->meta_seq, new_seq, old_seq);
    }
#else
    old_seq = atomic_fetch_add((volatile atomic_uint *)&mvcc->meta_seq, 1);
#endif
    *out = (uint64_t)new_seq;
    return YQ_OK;
}

static int read_meta_page(yq_mvcc *mvcc, uint32_t page_idx, meta_block *out) {
    uint8_t buf[4096];
    uint64_t off = (uint64_t)page_idx * mvcc->page_size;

    if (yq_file_pread(mvcc->db_file, buf, mvcc->page_size, off) != YQ_OK) {
        return YQ_ERR_IO;
    }

    meta_block *mb = (meta_block *)buf;

    uint32_t stored_crc = mb->header_crc32c;
    mb->header_crc32c = 0;
    uint32_t calc_crc = yq_crc32c(buf, 96);
    mb->header_crc32c = stored_crc;

    if (calc_crc != stored_crc) {
        return YQ_ERR_CORRUPT;
    }

    memcpy(out, buf, sizeof(meta_block));
    return YQ_OK;
}

int yq_mvcc_meta_read(yq_mvcc *mvcc, uint64_t *txn_id, uint64_t *root_page, uint64_t *free_head, uint64_t *npages, uint64_t *ckpt_lsn) {
    meta_block meta_a, meta_b;
    int rc_a = read_meta_page(mvcc, 0, &meta_a);
    int rc_b = read_meta_page(mvcc, 1, &meta_b);

    meta_block *chosen = NULL;

    if (rc_a == YQ_OK && rc_b == YQ_OK) {
        if (meta_a.txn_id > meta_b.txn_id ||
            (meta_a.txn_id == meta_b.txn_id && meta_a.meta_seq > meta_b.meta_seq)) {
            chosen = &meta_a;
        } else {
            chosen = &meta_b;
        }
    } else if (rc_a == YQ_OK) {
        chosen = &meta_a;
    } else if (rc_b == YQ_OK) {
        chosen = &meta_b;
    } else {
        return YQ_ERR_CORRUPT;
    }

    if (chosen->txn_id != 0) {
        if (txn_id) *txn_id = chosen->txn_id;
        if (root_page) *root_page = chosen->root_page;
        if (free_head) *free_head = chosen->free_head;
        if (npages) *npages = chosen->npages;
        if (ckpt_lsn) *ckpt_lsn = chosen->ckpt_lsn;
    } else {
        if (txn_id) *txn_id = 0;
        if (root_page) *root_page = 0;
        if (free_head) *free_head = 0;
        if (npages) *npages = 2;
        if (ckpt_lsn) *ckpt_lsn = 0;
    }

    return YQ_OK;
}

int yq_mvcc_meta_write(yq_mvcc *mvcc, uint64_t txn_id, uint64_t root_page, uint64_t free_head, uint64_t npages, uint64_t ckpt_lsn) {
    memset(mvcc->meta_buf, 0, sizeof(mvcc->meta_buf));
    meta_block *mb = (meta_block *)mvcc->meta_buf;
    mb->magic = (uint64_t)YQ_MAGIC_0 | ((uint64_t)YQ_MAGIC_1 << 32);
    mb->format_version = 1;
    mb->page_size = mvcc->page_size;
    mb->txn_id = txn_id;
    mb->root_page = root_page;
    mb->free_head = free_head;
    mb->npages = npages;
    mb->ckpt_lsn = ckpt_lsn;
    mb->log_trunc_lsn = 0;
    mb->meta_seq = mvcc->meta_seq;
    mb->reserved0 = 0;
    mb->node_encoding = 1;
    mb->flags = 0;
    mb->reserved1 = 0;
    return YQ_OK;
}

int yq_mvcc_meta_pwrite(yq_mvcc *mvcc, uint8_t meta_index) {
    uint8_t buf[4096];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, mvcc->meta_buf, mvcc->page_size);
    uint32_t crc = yq_crc32c(buf, 96);
    ((meta_block*)buf)->header_crc32c = crc;
    uint64_t off = (uint64_t)meta_index * mvcc->page_size;
    if (yq_file_size(mvcc->db_file) < off + mvcc->page_size) {
        if (yq_file_truncate(mvcc->db_file, off + mvcc->page_size) != YQ_OK) return YQ_ERR_IO;
    }
    if (yq_file_pwrite(mvcc->db_file, buf, mvcc->page_size, off) != YQ_OK) return YQ_ERR_IO;
    return YQ_OK;
}

static int select_meta_index(yq_mvcc *mvcc) {
    meta_block meta_a, meta_b;
    int rc_a = read_meta_page(mvcc, 0, &meta_a);
    int rc_b = read_meta_page(mvcc, 1, &meta_b);

    if (rc_a == YQ_OK && rc_b == YQ_OK) {
        if (meta_a.txn_id > meta_b.txn_id ||
            (meta_a.txn_id == meta_b.txn_id && meta_a.meta_seq > meta_b.meta_seq)) {
            return 1;
        }
        return 0;
    } else if (rc_a == YQ_OK) {
        return 1;
    } else if (rc_b == YQ_OK) {
        return 0;
    }
    return 0;
}

int yq_mvcc_meta_pwrite_full(yq_mvcc *mvcc, uint64_t txn_id, uint64_t root_page,
                              uint64_t free_head, uint64_t npages, uint64_t ckpt_lsn) {
    int target_idx = select_meta_index(mvcc);

    uint8_t buf[4096];
    memset(buf, 0, sizeof(buf));

    meta_block *mb = (meta_block *)buf;
    mb->magic = (uint64_t)YQ_MAGIC_0 | ((uint64_t)YQ_MAGIC_1 << 32);
    mb->format_version = 1;
    mb->page_size = mvcc->page_size;
    mb->txn_id = txn_id;
    mb->root_page = root_page;
    mb->free_head = free_head;
    mb->npages = npages;
    mb->ckpt_lsn = ckpt_lsn;
    mb->log_trunc_lsn = 0;
    mb->meta_seq = mvcc->meta_seq;
    mb->reserved0 = 0;
    mb->node_encoding = 1;
    mb->flags = 0;
    mb->reserved1 = 0;

    uint32_t crc = yq_crc32c(buf, 96);
    mb->header_crc32c = crc;

    uint64_t total_size = (uint64_t)mvcc->page_size * 2;
    if (yq_file_size(mvcc->db_file) < total_size) {
        if (yq_file_truncate(mvcc->db_file, total_size) != YQ_OK) {
            return YQ_ERR_IO;
        }
    }

    uint64_t off = (uint64_t)target_idx * mvcc->page_size;
    if (yq_file_pwrite(mvcc->db_file, buf, mvcc->page_size, off) != YQ_OK) {
        return YQ_ERR_IO;
    }

    return YQ_OK;
}

int yq_mvcc_elect_writer(yq_mvcc *mvcc, int wait_ms, int *got_it) {
    int rc;
    int elapsed = 0;
    const int interval = 10;

    while (true) {
        rc = yq_file_lock(mvcc->lock_file, 1);
        if (rc == YQ_OK) {
            *got_it = 1;
            return YQ_OK;
        }

        if (wait_ms <= 0) {
            *got_it = 0;
            return YQ_ERR_BUSY;
        }

        if (wait_ms <= interval) {
            elapsed += wait_ms;
#if defined(_WIN32)
            Sleep((DWORD)wait_ms);
#else
            usleep((useconds_t)wait_ms * 1000);
#endif
            wait_ms = 0;
        } else {
            elapsed += interval;
#if defined(_WIN32)
            Sleep((DWORD)interval);
#else
            usleep((useconds_t)interval * 1000);
#endif
            wait_ms -= interval;
        }

        if (wait_ms <= 0 && elapsed >= 0) {
            *got_it = 0;
            return YQ_ERR_TIMEOUT;
        }
    }
}

int yq_mvcc_release_writer(yq_mvcc *mvcc) {
    return yq_file_unlock(mvcc->lock_file);
}

int yq_mvcc_seqlock_read(yq_mvcc *mvcc, void *dst, size_t off, size_t len) {
    uint8_t *base = (uint8_t *)mvcc->shm_base;
    volatile uint32_t *seq_ptr = (volatile uint32_t *)((uint8_t *)base + 8);
    uint32_t s1, s2;
    uint8_t tmp[256];

    if (len > sizeof(tmp)) return YQ_ERR_INVAL;

    do {
        s1 = *seq_ptr;
        barrier();
        memcpy(tmp, base + off, len);
        barrier();
        s2 = *seq_ptr;
    } while ((s1 & 1) || s1 != s2);

    memcpy(dst, tmp, len);
    return YQ_OK;
}
