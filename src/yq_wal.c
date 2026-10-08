#include "yq_wal.h"
#include "yq_vfs.h"
#include "yq_enc.h"
#include <stdlib.h>
#include <string.h>

#define YQ_WAL_HEADER_SIZE 29

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

int yq_wal_append_put(yq_wal *wal, uint64_t txn_id, yq_slice key, yq_slice val) {
    uint8_t enc_buf[2048];
    size_t pos = 0;

    size_t nk;
    if (yq_varint_encode(key.size, enc_buf, &nk) != YQ_OK) return YQ_ERR_INVAL;
    memcpy(enc_buf + nk, key.data, key.size);
    pos = nk + key.size;

    size_t nv;
    if (yq_varint_encode(val.size, enc_buf + pos, &nv) != YQ_OK) return YQ_ERR_INVAL;
    memcpy(enc_buf + pos + nv, val.data, val.size);
    pos += nv + val.size;

    return append_record(wal, txn_id, WAL_TYPE_PUT, enc_buf, pos);
}

int yq_wal_append_del(yq_wal *wal, uint64_t txn_id, yq_slice key) {
    uint8_t enc_buf[1032];
    size_t nk;
    if (yq_varint_encode(key.size, enc_buf, &nk) != YQ_OK) return YQ_ERR_INVAL;
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

int yq_wal_scan(yq_wal *wal, uint64_t from_lsn, yq_wal_visitor visit, void *ctx) {
    size_t pos = 0;
    int rc;

    if (from_lsn == 0) {
        from_lsn = 1;
    }

    while (pos + YQ_WAL_HEADER_SIZE <= wal->file_size) {
        uint8_t header[YQ_WAL_HEADER_SIZE];
        rc = yq_file_pread(wal->file, header, YQ_WAL_HEADER_SIZE, pos);
        if (rc != YQ_OK) return rc;

        uint64_t lsn;
        memcpy(&lsn, header + 0, 8);

        uint64_t txn_id;
        memcpy(&txn_id, header + 8, 8);

        int rec_type = header[16];

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

        uint8_t *payload = NULL;
        if (payload_len > 0) {
            if (pos + YQ_WAL_HEADER_SIZE + payload_len > wal->file_size) break;

            payload = malloc(payload_len);
            if (!payload) return YQ_ERR_NOMEM;

            rc = yq_file_pread(wal->file, payload, payload_len, pos + YQ_WAL_HEADER_SIZE);
            if (rc != YQ_OK) {
                free(payload);
                return rc;
            }

            uint32_t stored_payload_crc;
            memcpy(&stored_payload_crc, header + 25, 4);
            uint32_t calc_payload_crc = yq_crc32c(payload, payload_len);

            if (stored_payload_crc != calc_payload_crc) {
                free(payload);
                break;
            }
        }

        if (lsn >= from_lsn) {
            rc = visit(ctx, lsn, txn_id, rec_type, payload, payload_len);
            if (rc != YQ_OK) {
                free(payload);
                return rc;
            }
        }

        free(payload);
        pos += YQ_WAL_HEADER_SIZE + payload_len;
    }

    return YQ_OK;
}
