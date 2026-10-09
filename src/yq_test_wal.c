/*
 * Regression tests for WAL record encoding of large values.
 *
 * yq_wal_append_put() used to encode varint(key_len) key varint(val_len) val
 * into a fixed 2048-byte stack buffer, while the engine accepts values up to
 * 1 GiB. Any value larger than the buffer smashed the stack (glibc aborts with
 * "*** buffer overflow detected ***"). These tests drive values well past that
 * old limit and check they round-trip through flush, reopen, scan and recovery.
 *
 * Checks use an explicit macro instead of assert() so that they stay active
 * regardless of whether the build type defines NDEBUG.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yq.h"
#include "yq_wal.h"
#include "yq_memtable.h"
#include "yq_recover.h"

static const char *TEST_DB = "yqtest_wal.db";
static const char *TEST_LOG = "yqtest_wal.db.log";

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            exit(1);                                                      \
        }                                                                 \
    } while (0)

static void remove_files(void) {
    remove(TEST_LOG);
    remove(TEST_DB);
}

/* Deterministic payload so a mis-sized copy is detectable, not just a crash. */
static void fill_pattern(uint8_t *buf, size_t len, unsigned seed) {
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)((i * 31u + seed * 7u + 1u) & 0xFF);
    }
}

/* Append one committed transaction holding `count` keys of `val_size` bytes. */
static void write_values(size_t val_size) {
    remove_files();

    yq_wal *wal = NULL;
    CHECK(yq_wal_open(&wal, TEST_DB, 4096) == YQ_OK);

    uint8_t *val = (uint8_t *)malloc(val_size ? val_size : 1);
    CHECK(val != NULL);
    fill_pattern(val, val_size, (unsigned)(val_size & 0xFFFF));

    const char *keys[2] = {"small-key", "large-key"};

    CHECK(yq_wal_append_begin(wal, 1) == YQ_OK);
    for (int i = 0; i < 2; i++) {
        yq_slice k;
        k.data = keys[i];
        k.size = strlen(keys[i]);
        yq_slice v;
        v.data = val;
        v.size = val_size;
        CHECK(yq_wal_append_put(wal, 1, k, v) == YQ_OK);
    }
    CHECK(yq_wal_append_commit(wal, 1) == YQ_OK);
    CHECK(yq_wal_flush(wal) == YQ_OK);
    CHECK(yq_wal_close(wal) == YQ_OK);

    free(val);
}

/* Replay the log and compare every recovered value against the original. */
static void verify_values(size_t val_size) {
    uint8_t *expect = (uint8_t *)malloc(val_size ? val_size : 1);
    CHECK(expect != NULL);
    fill_pattern(expect, val_size, (unsigned)(val_size & 0xFFFF));

    yq_wal *wal = NULL;
    CHECK(yq_wal_open(&wal, TEST_DB, 4096) == YQ_OK);

    yq_memtable *mt = yq_memtable_create(256u * 1024u * 1024u);
    CHECK(mt != NULL);
    CHECK(yq_recover(wal, mt) == YQ_OK);

    const char *keys[2] = {"small-key", "large-key"};
    for (int i = 0; i < 2; i++) {
        yq_slice k;
        k.data = keys[i];
        k.size = strlen(keys[i]);
        yq_slice out = {0};
        CHECK(yq_memtable_get(mt, k, &out) == YQ_OK);
        CHECK(out.size == val_size);
        CHECK(val_size == 0 || memcmp(out.data, expect, val_size) == 0);
    }

    yq_memtable_destroy(mt);
    CHECK(yq_wal_close(wal) == YQ_OK);

    free(expect);
    remove_files();
}

static void test_roundtrip_size(size_t val_size) {
    printf("test_wal_roundtrip(%zu bytes)... ", val_size);
    write_values(val_size);
    verify_values(val_size);
    printf("OK\n");
}

/* The encoder must reject nonsense instead of writing past its buffer. */
static void test_rejects_bad_input(void) {
    printf("test_wal_rejects_bad_input... ");
    remove_files();

    yq_wal *wal = NULL;
    CHECK(yq_wal_open(&wal, TEST_DB, 4096) == YQ_OK);

    yq_slice empty_key = {"", 0};
    yq_slice val = {"v", 1};
    CHECK(yq_wal_append_put(wal, 1, empty_key, val) == YQ_ERR_INVAL);

    uint8_t big_key[1025];
    memset(big_key, 'k', sizeof(big_key));
    yq_slice oversize_key = {big_key, sizeof(big_key)};
    CHECK(yq_wal_append_put(wal, 1, oversize_key, val) == YQ_ERR_INVAL);
    CHECK(yq_wal_append_del(wal, 1, oversize_key) == YQ_ERR_INVAL);

    yq_slice key = {"k", 1};
    yq_slice dangling = {NULL, 4096};
    CHECK(yq_wal_append_put(wal, 1, key, dangling) == YQ_ERR_INVAL);

    CHECK(yq_wal_close(wal) == YQ_OK);
    remove_files();
    printf("OK\n");
}

int main(void) {
    printf("=== yq-DB WAL Large Value Tests ===\n\n");

    /* 2048 was the old stack buffer; exercise just under, just over, and far
     * past it, plus the zero-length value edge case. */
    test_roundtrip_size(0);
    test_roundtrip_size(1);
    test_roundtrip_size(2040);
    test_roundtrip_size(2048);
    test_roundtrip_size(65536);
    test_roundtrip_size(1024 * 1024);
    test_rejects_bad_input();

    printf("\n=== ALL TESTS PASSED ===\n");
    return 0;
}
