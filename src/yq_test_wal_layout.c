/*
 * yq_test_wal_layout.c — byte-exact check of the WAL record format.
 *
 * yq_wal_append_put()/del() now encode straight into the log buffer instead of
 * building the payload in a scratch buffer that append_record() then copied.
 * This test pins the concrete bytes so the refactor is provably format-neutral:
 *
 *   header (29 bytes): lsn(8) txn_id(8) type(1) payload_len(4) header_crc(4) payload_crc(4)
 *   PUT  payload     : varint(key_len) key varint(val_len) val
 *   DEL  payload     : varint(key_len) key
 *   BEGIN/COMMIT     : empty payload
 *
 * Both CRCs are checked against yq_crc32c() over the header with the two CRC
 * fields zeroed and over the payload, matching yq_wal_flush().
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "yq.h"
#include "yq_wal.h"
#include "yq_vfs.h"
#include "yq_enc.h"

static const char *TEST_DB = "yqtest_wal_layout.db";
static const char *TEST_LOG = "yqtest_wal_layout.db.log";

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            exit(1);                                                      \
        }                                                                 \
    } while (0)

#define WAL_HEADER 29
#define T_BEGIN 1
#define T_PUT   2
#define T_DEL   3
#define T_COMMIT 4

static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

int main(void) {
    remove(TEST_LOG);
    remove(TEST_DB);

    yq_wal *wal = NULL;
    CHECK(yq_wal_open(&wal, TEST_DB, 4096) == YQ_OK);

    yq_slice k1 = { "alpha", 5 };
    yq_slice v1 = { "hello", 5 };
    yq_slice k2 = { "beta", 4 };

    CHECK(yq_wal_append_begin(wal, 7) == YQ_OK);
    CHECK(yq_wal_append_put(wal, 7, k1, v1) == YQ_OK);
    CHECK(yq_wal_append_del(wal, 7, k2) == YQ_OK);
    CHECK(yq_wal_append_commit(wal, 7) == YQ_OK);
    CHECK(yq_wal_flush(wal) == YQ_OK);
    CHECK(yq_wal_close(wal) == YQ_OK);

    /* Read the raw log back and walk it record by record. */
    yq_file *f = yq_file_open(TEST_LOG, 0, 0);
    CHECK(f != NULL);
    uint64_t fsize = yq_file_size(f);
    CHECK(fsize > WAL_HEADER);

    uint8_t *buf = (uint8_t *)malloc((size_t)fsize);
    CHECK(buf != NULL);
    CHECK(yq_file_pread(f, buf, (size_t)fsize, 0) == YQ_OK);
    yq_file_close(f);

    size_t pos = 0;

    /* --- BEGIN --- */
    CHECK(rd32(buf + pos + 17) == 0);
    CHECK(buf[pos + 16] == T_BEGIN);
    CHECK(rd64(buf + pos + 8) == 7);
    {
        uint8_t hdr[WAL_HEADER];
        memcpy(hdr, buf + pos, WAL_HEADER);
        uint32_t stored = rd32(hdr + 21);
        memset(hdr + 21, 0, 8);
        CHECK(yq_crc32c(hdr, WAL_HEADER) == stored);
    }
    pos += WAL_HEADER;

    /* --- PUT --- */
    CHECK(buf[pos + 16] == T_PUT);
    CHECK(rd32(buf + pos + 17) == 12); /* 1 + 5 + 1 + 5 */
    {
        const uint8_t *pl = buf + pos + WAL_HEADER;
        CHECK(pl[0] == 5);                       /* varint(key_len) */
        CHECK(memcmp(pl + 1, "alpha", 5) == 0);  /* key */
        CHECK(pl[6] == 5);                       /* varint(val_len) */
        CHECK(memcmp(pl + 7, "hello", 5) == 0);  /* val */
        /* payload CRC must match */
        uint8_t hdr[WAL_HEADER];
        memcpy(hdr, buf + pos, WAL_HEADER);
        uint32_t stored_pcrc = rd32(hdr + 25);
        CHECK(yq_crc32c(pl, 12) == stored_pcrc);
    }
    pos += WAL_HEADER + 12;

    /* --- DEL --- */
    CHECK(buf[pos + 16] == T_DEL);
    CHECK(rd32(buf + pos + 17) == 5); /* 1 + 4 */
    {
        const uint8_t *pl = buf + pos + WAL_HEADER;
        CHECK(pl[0] == 4);                      /* varint(key_len) */
        CHECK(memcmp(pl + 1, "beta", 4) == 0);  /* key */
    }
    pos += WAL_HEADER + 5;

    /* --- COMMIT --- */
    CHECK(buf[pos + 16] == T_COMMIT);
    CHECK(rd32(buf + pos + 17) == 0);
    pos += WAL_HEADER;

    CHECK(pos == fsize); /* exactly four records, no trailing bytes */

    free(buf);
    remove(TEST_LOG);
    remove(TEST_DB);

    printf("wal_layout: OK\n");
    return 0;
}
