#ifndef YQ_WAL_H
#define YQ_WAL_H
#include <stddef.h>
#include <stdint.h>
#include "yq.h"
#include "yq_vfs.h"

typedef struct yq_wal yq_wal;

int yq_wal_open(yq_wal **out, const char *db_path, uint64_t default_page_size);
int yq_wal_close(yq_wal *wal);
int yq_wal_append_begin(yq_wal *wal, uint64_t txn_id);
int yq_wal_append_put(yq_wal *wal, uint64_t txn_id, yq_slice key, yq_slice val);
int yq_wal_append_del(yq_wal *wal, uint64_t txn_id, yq_slice key);
int yq_wal_append_commit(yq_wal *wal, uint64_t txn_id);
int yq_wal_append_abort(yq_wal *wal, uint64_t txn_id);
int yq_wal_append_ckpt_begin(yq_wal *wal, uint64_t root_page, uint64_t txn_id);
int yq_wal_append_ckpt_end(yq_wal *wal, uint64_t ckpt_lsn);
int yq_wal_flush(yq_wal *wal);
int yq_wal_truncate(yq_wal *wal, uint64_t lsn);
uint64_t yq_wal_size(yq_wal *wal);
uint64_t yq_wal_last_lsn(yq_wal *wal);

/*
 * 扫描回调。
 *
 * 契约（较旧实现有变化）：payload 指向扫描内部复用的交换缓冲，
 * **仅在本次回调期间有效**。回调返回后该内存可能立刻被下一条记录覆盖，
 * 也可能在扫描结束时被释放；需要保留请在回调内自行 memcpy。
 * paylen 为 0 时 payload 为 NULL。
 */
typedef int (*yq_wal_visitor)(void *ctx, uint64_t lsn, uint64_t txn_id, int rec_type,
    const uint8_t *payload, size_t paylen);
int yq_wal_scan(yq_wal *wal, uint64_t from_lsn, yq_wal_visitor visit, void *ctx);

#endif
