#include "yq_wal.h"
#include "yq_vfs.h"
#include "yq_enc.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define YQ_WAL_HEADER_SIZE 29
#define YQ_WAL_MAX_KEY_SIZE 1024
#define YQ_WAL_VARINT_MAX   10
#define YQ_WAL_STACK_ENC    512

/*
 * yq_wal_append_put() 的栈缓冲上限：payload 小于它就走栈，否则走堆。
 * 取 2 KiB 是为了覆盖"小 key + 小 value"这一热路径，同时不至于把栈压大。
 */
#define YQ_WAL_SMALL_PAYLOAD 2048

#define WAL_TYPE_BEGIN   1
#define WAL_TYPE_PUT     2
#define WAL_TYPE_DEL     3
#define WAL_TYPE_COMMIT  4
#define WAL_TYPE_ABORT   5
#define WAL_TYPE_CKPT_BEGIN 6
#define WAL_TYPE_CKPT_END   7

struct yq_wal {
    yq_file *file;
    uint8_t *buf;
    size_t buf_cap;
    size_t buf_used;
    uint64_t default_page_size;
    uint64_t file_size;
    uint64_t last_lsn;
    char log_path[YQ_MAX_PATH];
};

static int make_log_path(char *out, size_t out_cap, const char *db_path) {
    size_t base_len = strlen(db_path);
    if (base_len + 5 > out_cap) return YQ_ERR_INVAL;
    memcpy(out, db_path, base_len);
    memcpy(out + base_len, ".log", 5);
    return YQ_OK;
}

int yq_wal_open(yq_wal **out, const char *db_path, uint64_t default_page_size) {
    yq_wal *wal = calloc(1, sizeof(yq_wal));
    if (!wal) return YQ_ERR_NOMEM;

    int rc = make_log_path(wal->log_path, sizeof(wal->log_path), db_path);
    if (rc != YQ_OK) {
        free(wal);
        return rc;
    }

    wal->file = yq_file_open(wal->log_path, 1, 1);
    if (!wal->file) {
        free(wal);
        return YQ_ERR_IO;
    }

    wal->default_page_size = default_page_size ? default_page_size : 4096;
    wal->buf_cap = wal->default_page_size;
    wal->buf = malloc(wal->buf_cap);
    if (!wal->buf) {
        yq_file_close(wal->file);
        free(wal);
        return YQ_ERR_NOMEM;
    }

    wal->file_size = yq_file_size(wal->file);
    wal->last_lsn = 0;

    *out = wal;
    return YQ_OK;
}

int yq_wal_close(yq_wal *wal) {
    if (!wal) return YQ_OK;

    if (wal->buf && wal->buf_used > 0) {
        yq_wal_flush(wal);
    }
    if (wal->file) {
        yq_file_sync(wal->file);
        yq_file_close(wal->file);
    }
    free(wal->buf);
    free(wal);
    return YQ_OK;
}

static int ensure_buf_space(yq_wal *wal, size_t need) {
    if (wal->buf_used + need <= wal->buf_cap) return YQ_OK;

    size_t new_cap = wal->buf_cap;
    while (new_cap < wal->buf_used + need) {
        new_cap *= 2;
    }

    uint8_t *new_buf = realloc(wal->buf, new_cap);
    if (!new_buf) return YQ_ERR_NOMEM;

    wal->buf = new_buf;
    wal->buf_cap = new_cap;
    return YQ_OK;
}

static int append_record(yq_wal *wal, uint64_t txn_id, int rec_type,
                         const uint8_t *payload, size_t paylen) {
    size_t total = YQ_WAL_HEADER_SIZE + paylen;
    int rc = ensure_buf_space(wal, total);
    if (rc != YQ_OK) return rc;

    uint8_t *p = wal->buf + wal->buf_used;

    uint64_t lsn = 0;
    memcpy(p + 0, &lsn, 8);
    memcpy(p + 8, &txn_id, 8);
    p[16] = (uint8_t)rec_type;
    memcpy(p + 17, &paylen, 4);

    uint8_t header_for_crc[YQ_WAL_HEADER_SIZE];
    memcpy(header_for_crc + 0, p + 0, 8);
    memcpy(header_for_crc + 8, p + 8, 8);
    header_for_crc[16] = p[16];
    memcpy(header_for_crc + 17, p + 17, 4);
    memset(header_for_crc + 21, 0, 4);
    memset(header_for_crc + 25, 0, 4);

    uint32_t header_crc = yq_crc32c(header_for_crc, YQ_WAL_HEADER_SIZE);
    memcpy(p + 21, &header_crc, 4);

    uint32_t payload_crc = 0;
    if (paylen > 0 && payload) {
        payload_crc = yq_crc32c(payload, paylen);
    }
    memcpy(p + 25, &payload_crc, 4);

    if (paylen > 0 && payload) {
        memcpy(p + YQ_WAL_HEADER_SIZE, payload, paylen);
    }

    wal->buf_used += total;
    return YQ_OK;
}

int yq_wal_append_begin(yq_wal *wal, uint64_t txn_id) {
    return append_record(wal, txn_id, WAL_TYPE_BEGIN, NULL, 0);
}

/*
 * PUT 记录 payload = varint(key_len) + key + varint(val_len) + val。
 *
 * key 上限 1024 字节、val 上限 1 GiB（见 yq_put），所以 payload 不适合放在
 * 栈上定长数组里：旧实现用 uint8_t enc_buf[2048] 且 memcpy 前不做边界检查，
 * 任何 > ~2KB 的 value 都会写爆栈（ASan: stack-buffer-overflow @ yq_wal.c）。
 * 这里改为按需小缓冲：小 payload 走栈上的 2 KiB 缓冲避免堆分配，
 * 大 payload 回退到堆缓冲，两条路径都不再有溢出可能。
 */
#define YQ_WAL_SMALL_PAYLOAD 2048

int yq_wal_append_put(yq_wal *wal, uint64_t txn_id, yq_slice key, yq_slice val) {
    if (!wal) return YQ_ERR_INVAL;
    if (key.size == 0 || key.size > YQ_WAL_MAX_KEY_SIZE) return YQ_ERR_INVAL;
    if (!key.data) return YQ_ERR_INVAL;
    if (val.size > 0 && !val.data) return YQ_ERR_INVAL;

    /*
     * Payload layout is varint(key_len) key varint(val_len) val. Values are
     * bounded only by the engine limit (1 GiB), so a fixed stack buffer would
     * overflow as soon as a value exceeds it. Encode into a stack buffer when
     * the record is small -- the overwhelmingly common case -- and fall back
     * to the heap for anything larger.
     */
    uint8_t klen_buf[YQ_WAL_VARINT_MAX];
    uint8_t vlen_buf[YQ_WAL_VARINT_MAX];
    size_t nk = 0, nv = 0;

    if (yq_varint_encode(key.size, klen_buf, &nk) != YQ_OK) return YQ_ERR_INVAL;
    if (yq_varint_encode(val.size, vlen_buf, &nv) != YQ_OK) return YQ_ERR_INVAL;

    if (val.size > SIZE_MAX - (nk + key.size + nv)) return YQ_ERR_TOOBIG;
    size_t total = nk + key.size + nv + val.size;

    uint8_t stack_buf[YQ_WAL_STACK_ENC];
    uint8_t *enc = stack_buf;
    if (total > sizeof(stack_buf)) {
        enc = (uint8_t *)malloc(total);
        if (!enc) return YQ_ERR_NOMEM;
    }

    size_t pos = 0;
    memcpy(enc + pos, klen_buf, nk);
    pos += nk;
    memcpy(enc + pos, key.data, key.size);
    pos += key.size;
    memcpy(enc + pos, vlen_buf, nv);
    pos += nv;
    if (val.size) memcpy(enc + pos, val.data, val.size);

    int rc = append_record(wal, txn_id, WAL_TYPE_PUT, enc, total);
    if (enc != stack_buf) free(enc);
    return rc;
}

int yq_wal_append_del(yq_wal *wal, uint64_t txn_id, yq_slice key) {
    if (!wal) return YQ_ERR_INVAL;
    if (key.size == 0 || key.size > YQ_WAL_MAX_KEY_SIZE) return YQ_ERR_INVAL;
    if (!key.data) return YQ_ERR_INVAL;

    uint8_t enc_buf[YQ_WAL_MAX_KEY_SIZE + YQ_WAL_VARINT_MAX];
    size_t nk;
    if (yq_varint_encode(key.size, enc_buf, &nk) != YQ_OK) return YQ_ERR_INVAL;
    if (key.size > sizeof(enc_buf) - nk) return YQ_ERR_TOOBIG;
    memcpy(enc_buf + nk, key.data, key.size);

    return append_record(wal, txn_id, WAL_TYPE_DEL, enc_buf, nk + key.size);
}

int yq_wal_append_commit(yq_wal *wal, uint64_t txn_id) {
    return append_record(wal, txn_id, WAL_TYPE_COMMIT, NULL, 0);
}

int yq_wal_append_abort(yq_wal *wal, uint64_t txn_id) {
    return append_record(wal, txn_id, WAL_TYPE_ABORT, NULL, 0);
}

int yq_wal_append_ckpt_begin(yq_wal *wal, uint64_t root_page, uint64_t txn_id) {
    uint8_t payload[16];
    memcpy(payload + 0, &root_page, 8);
    memcpy(payload + 8, &txn_id, 8);
    return append_record(wal, txn_id, WAL_TYPE_CKPT_BEGIN, payload, 16);
}

int yq_wal_append_ckpt_end(yq_wal *wal, uint64_t ckpt_lsn) {
    uint8_t payload[8];
    memcpy(payload, &ckpt_lsn, 8);
    return append_record(wal, 0, WAL_TYPE_CKPT_END, payload, 8);
}

int yq_wal_flush(yq_wal *wal) {
    if (!wal || wal->buf_used == 0) return YQ_OK;

    size_t offset = wal->file_size;
    size_t pos = 0;

    while (pos < wal->buf_used) {
        uint8_t *rec = wal->buf + pos;
        uint64_t lsn = ++wal->last_lsn;
        memcpy(rec + 0, &lsn, 8);

        uint8_t header_for_crc[YQ_WAL_HEADER_SIZE];
        memcpy(header_for_crc + 0, rec + 0, 8);
        memcpy(header_for_crc + 8, rec + 8, 8);
        header_for_crc[16] = rec[16];
        memcpy(header_for_crc + 17, rec + 17, 4);
        memset(header_for_crc + 21, 0, 4);
        memset(header_for_crc + 25, 0, 4);

        uint32_t header_crc = yq_crc32c(header_for_crc, YQ_WAL_HEADER_SIZE);
        memcpy(rec + 21, &header_crc, 4);

        uint32_t payload_len;
        memcpy(&payload_len, rec + 17, 4);
        uint32_t payload_crc = 0;
        if (payload_len > 0) {
            payload_crc = yq_crc32c(rec + YQ_WAL_HEADER_SIZE, payload_len);
        }
        memcpy(rec + 25, &payload_crc, 4);

        int rc = yq_file_pwrite(wal->file, rec, YQ_WAL_HEADER_SIZE + payload_len, offset);
        if (rc != YQ_OK) return rc;

        offset += YQ_WAL_HEADER_SIZE + payload_len;
        pos += YQ_WAL_HEADER_SIZE + payload_len;
    }

    int rc = yq_file_sync(wal->file);
    if (rc != YQ_OK) return rc;

    wal->file_size += wal->buf_used;
    wal->buf_used = 0;

    return YQ_OK;
}

int yq_wal_truncate(yq_wal *wal, uint64_t lsn) {
    if (lsn == 0) return YQ_OK;

    uint8_t header[YQ_WAL_HEADER_SIZE];
    size_t pos = 0;
    uint64_t target_lsn = 0;
    uint64_t target_pos = 0;
    int found = 0;

    while (pos + YQ_WAL_HEADER_SIZE <= wal->file_size) {
        int rc = yq_file_pread(wal->file, header, YQ_WAL_HEADER_SIZE, pos);
        if (rc != YQ_OK) return rc;

        uint64_t rec_lsn;
        memcpy(&rec_lsn, header + 0, 8);

        uint32_t payload_len;
        memcpy(&payload_len, header + 17, 4);

        uint8_t header_for_crc[YQ_WAL_HEADER_SIZE];
        memcpy(header_for_crc + 0, header + 0, 8);
        memcpy(header_for_crc + 8, header + 8, 8);
        header_for_crc[16] = header[16];
        memcpy(header_for_crc + 17, header + 17, 4);
        memset(header_for_crc + 21, 0, 4);
        memset(header_for_crc + 25, 0, 4);

        uint32_t stored_crc;
        memcpy(&stored_crc, header + 21, 4);
        uint32_t calc_crc = yq_crc32c(header_for_crc, YQ_WAL_HEADER_SIZE);

        if (stored_crc != calc_crc) break;

        if (rec_lsn <= lsn) {
            target_lsn = rec_lsn;
            target_pos = pos + YQ_WAL_HEADER_SIZE + payload_len;
            found = 1;
        } else {
            break;
        }

        pos += YQ_WAL_HEADER_SIZE + payload_len;
    }

    if (!found) return YQ_OK;

    int rc = yq_file_truncate(wal->file, target_pos);
    if (rc != YQ_OK) return rc;

    wal->file_size = target_pos;
    wal->last_lsn = target_lsn;

    /* Truncate must follow yq_wal_flush(): clear any unflushed buffered
     * records so a later flush cannot re-append them past the new end. */
    wal->buf_used = 0;

    return YQ_OK;
}

uint64_t yq_wal_size(yq_wal *wal) {
    if (!wal) return 0;
    return wal->file_size + wal->buf_used;
}

uint64_t yq_wal_last_lsn(yq_wal *wal) {
    if (!wal) return 0;
    return wal->last_lsn;
}

/*
 * 扫描用的块缓冲：一次性读入 64 KiB 后在其上解析记录，
 * 把"每条记录两次 pread + 一次 malloc"降为"每 64 KiB 一次 pread"，
 * payload 复用同一块交换缓冲。
 */
#define YQ_WAL_SCAN_BLOCK (64 * 1024)

typedef struct {
    uint8_t  block[YQ_WAL_SCAN_BLOCK]; /* 已读入的块缓冲 */
    size_t   block_len;                /* 块内有效字节数 */
    size_t   block_pos;                /* 块内已消费字节数 */
    uint64_t block_off;                /* block[0] 对应的文件偏移 */
    uint8_t *payload;                  /* payload 交换缓冲（跨记录复用） */
    size_t   payload_cap;              /* 交换缓冲容量 */
    uint64_t file_size;                /* 本次扫描的文件大小 */
} yq_wal_scan_buf;

/*
 * 从块缓冲取走 n 字节写入 dst。
 * 返回 1 成功；0 表示文件已到尾部（调用方按截断处理）；-1 表示 IO 错误。
 */
static int yq_wal_scan_gather(yq_wal *wal, yq_wal_scan_buf *b, uint8_t *dst, size_t n) {
    size_t copied = 0;

    while (copied < n) {
        size_t avail = b->block_len - b->block_pos;

        if (avail == 0) {
            uint64_t next_off = b->block_off + b->block_len;
            uint64_t remain;
            size_t want;

            if (next_off >= b->file_size) return 0;

            remain = b->file_size - next_off;
            want = YQ_WAL_SCAN_BLOCK;
            if ((uint64_t)want > remain) want = (size_t)remain;

            if (yq_file_pread(wal->file, b->block, want, next_off) != YQ_OK) {
                return -1;
            }

            b->block_off = next_off;
            b->block_len = want;
            b->block_pos = 0;
            avail = want;
        }

        {
            size_t chunk = avail < (n - copied) ? avail : (n - copied);
            memcpy(dst + copied, b->block + b->block_pos, chunk);
            b->block_pos += chunk;
            copied += chunk;
        }
    }

    return 1;
}

static int yq_wal_scan_reserve_payload(yq_wal_scan_buf *b, size_t need) {
    if (b->payload_cap >= need) return YQ_OK;

    uint8_t *np = (uint8_t *)realloc(b->payload, need);
    if (!np) return YQ_ERR_NOMEM;

    b->payload = np;
    b->payload_cap = need;
    return YQ_OK;
}

int yq_wal_scan(yq_wal *wal, uint64_t from_lsn, yq_wal_visitor visit, void *ctx) {
    yq_wal_scan_buf *b;
    uint64_t pos = 0;
    int result = YQ_OK;

    if (!wal) return YQ_ERR_INVAL;
    if (from_lsn == 0) from_lsn = 1;

    /* 64 KiB 结构体放堆上，避免占用调用栈 */
    b = (yq_wal_scan_buf *)malloc(sizeof(*b));
    if (!b) return YQ_ERR_NOMEM;
    memset(b, 0, sizeof(*b));
    b->file_size = wal->file_size;

    while (pos + YQ_WAL_HEADER_SIZE <= b->file_size) {
        uint8_t header[YQ_WAL_HEADER_SIZE];
        uint8_t header_for_crc[YQ_WAL_HEADER_SIZE];
        uint64_t lsn;
        uint64_t txn_id;
        int rec_type;
        uint32_t payload_len;
        uint32_t stored_crc;
        uint32_t calc_crc;
        const uint8_t *payload = NULL;
        int g;

        g = yq_wal_scan_gather(wal, b, header, YQ_WAL_HEADER_SIZE);
        if (g != 1) {
            if (g < 0) result = YQ_ERR_IO;
            break;
        }

        memcpy(&lsn, header + 0, 8);
        memcpy(&txn_id, header + 8, 8);
        rec_type = header[16];
        memcpy(&payload_len, header + 17, 4);

        memcpy(header_for_crc + 0, header + 0, 8);
        memcpy(header_for_crc + 8, header + 8, 8);
        header_for_crc[16] = header[16];
        memcpy(header_for_crc + 17, header + 17, 4);
        memset(header_for_crc + 21, 0, 4);
        memset(header_for_crc + 25, 0, 4);

        memcpy(&stored_crc, header + 21, 4);
        calc_crc = yq_crc32c(header_for_crc, YQ_WAL_HEADER_SIZE);
        if (stored_crc != calc_crc) break; /* 坏记录：停止，保持截断语义 */

        if (payload_len > 0) {
            uint32_t stored_payload_crc;
            uint32_t calc_payload_crc;

            /* 记录不完整（尾部截断）：停止 */
            if (pos + YQ_WAL_HEADER_SIZE + payload_len > b->file_size) break;

            if (yq_wal_scan_reserve_payload(b, payload_len) != YQ_OK) {
                result = YQ_ERR_NOMEM;
                break;
            }

            if ((uint64_t)payload_len <= YQ_WAL_SCAN_BLOCK) {
                /* 小 payload：从块缓冲取，通常零系统调用 */
                g = yq_wal_scan_gather(wal, b, b->payload, payload_len);
                if (g != 1) {
                    if (g < 0) result = YQ_ERR_IO;
                    break;
                }
            } else {
                /* 大 payload：按精确长度一次读取，避免多次小块读 */
                if (yq_file_pread(wal->file, b->payload, payload_len,
                                  pos + YQ_WAL_HEADER_SIZE) != YQ_OK) {
                    result = YQ_ERR_IO;
                    break;
                }
                /* 块缓冲与文件位置脱节，重置以免读到旧数据 */
                b->block_off = pos + YQ_WAL_HEADER_SIZE + payload_len;
                b->block_len = 0;
                b->block_pos = 0;
            }

            memcpy(&stored_payload_crc, header + 25, 4);
            calc_payload_crc = yq_crc32c(b->payload, payload_len);
            if (stored_payload_crc != calc_payload_crc) break; /* 坏记录：停止 */

            payload = b->payload;
        }

        if (lsn >= from_lsn) {
            int vrc = visit(ctx, lsn, txn_id, rec_type, payload, payload_len);
            if (vrc != YQ_OK) {
                result = vrc;
                break;
            }
        }

        pos += YQ_WAL_HEADER_SIZE + payload_len;
    }

    free(b->payload);
    free(b);
    return result;
}
