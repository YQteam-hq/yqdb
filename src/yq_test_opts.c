/*
 * Regression tests for the yq_open() option contract.
 *
 * yq.h documents hard requirements on yq_opts -- page_size must be a power of
 * two in [4096, 65536], max_readers is capped at 65535, sync_mode must be a
 * YQ_SYNC_* value, reserved[] must be zero -- and YQ_OPEN_EXCL must fail with
 * YQ_ERR_EXISTS when the database already exists. None of that was enforced:
 * every one of these used to be accepted silently, and YQ_OPEN_EXCL was never
 * looked at at all.
 *
 * Checks use an explicit macro instead of assert() so that they stay active
 * regardless of whether the build type defines NDEBUG.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yq.h"

/* Security validation macros */
#define SECURITY_CHECK_NULL(ptr) \
    do { \
        if ((ptr) == NULL) { \
            fprintf(stderr, "Security failure: NULL pointer at %s:%d\n", __FILE__, __LINE__); \
            exit(1); \
        } \
    } while (0)

#define SECURITY_CHECK_SIZE(size, max_size) \
    do { \
        if ((size) == 0 || (size) > (max_size)) { \
            fprintf(stderr, "Security failure: Invalid size %zu at %s:%d\n", (size), __FILE__, __LINE__); \
            exit(1); \
        } \
    } while (0)

#define SECURITY_CHECK_RANGE(value, min_val, max_val) \
    do { \
        if ((value) < (min_val) || (value) > (max_val)) { \
            fprintf(stderr, "Security failure: Value %zu out of range [%zu, %zu] at %s:%d\n", \
                    (size_t)(value), (size_t)(min_val), (size_t)(max_val), __FILE__, __LINE__); \
            exit(1); \
        } \
    } while (0)

/* Security validation for database paths */
static int validate_db_path(const char *path) {
    if (!path || strlen(path) == 0) {
        return 0;
    }
    
    /* Check for path traversal attempts */
    if (strstr(path, "..") != NULL || strstr(path, "\\") != NULL || strstr(path, "/") != NULL) {
        return 0;
    }
    
    /* Check path length */
    if (strlen(path) > 256) {
        return 0;
    }
    
    return 1;
}

/* Security-enhanced file existence check */
static int secure_file_exists(const char *path) {
    if (!validate_db_path(path)) {
        return 0;
    }
    
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static const char *TEST_DB = "yqtest_opts.yqdb";

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            exit(1);                                                      \
        }                                                                 \
    } while (0)

static void remove_db(void) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s.log", TEST_DB);  remove(buf);
    snprintf(buf, sizeof(buf), "%s.shm", TEST_DB);  remove(buf);
    snprintf(buf, sizeof(buf), "%s.lock", TEST_DB); remove(buf);
    remove(TEST_DB);
}

static int file_exists(const char *path) {
    return secure_file_exists(path);
}

/* Fill a valid option set that the individual cases mutate. */
static void base_opts(yq_opts *opts) {
    SECURITY_CHECK_NULL(opts);
    SECURITY_CHECK_SIZE(sizeof(*opts), 1024); /* Reasonable size limit */
    
    memset(opts, 0, sizeof(*opts));
    opts->struct_size = sizeof(*opts);
    opts->flags = YQ_OPEN_CREATE;
}

static void expect_rejected(const char *what, yq_opts *opts) {
    if (!validate_db_path(TEST_DB)) {
        fprintf(stderr, "Security failure: Invalid test database path\n");
        exit(1);
    }
    
    SECURITY_CHECK_NULL(opts);
    yq_db *db = (yq_db *)(void *)TEST_DB; /* poisoned: must be overwritten */
    int rc = yq_open(TEST_DB, opts, &db);
    if (rc != YQ_ERR_INVAL || db != NULL) {
        fprintf(stderr, "FAIL %s: rc=%d db=%s\n", what, rc,
                db == NULL ? "NULL" : "non-NULL");
        exit(1);
    }
}

static void test_rejects_bad_struct_size(void) {
    printf("test_rejects_bad_struct_size... ");
    remove_db();

    yq_opts opts;
    base_opts(&opts);
    opts.struct_size = (uint32_t)sizeof(opts) - 4u;
    SECURITY_CHECK_RANGE(opts.struct_size, 1, 1024); /* Validate struct size */
    expect_rejected("struct_size", &opts);

    yq_db *db = NULL;
    CHECK(yq_open(NULL, &opts, &db) == YQ_ERR_INVAL);
    CHECK(db == NULL);

    remove_db();
    printf("OK\n");
}

static void test_rejects_bad_page_size(void) {
    printf("test_rejects_bad_page_size... ");
    remove_db();

    /* Not a power of two, but inside the documented range. */
    yq_opts opts;
    base_opts(&opts);
    opts.page_size = 5000;
    SECURITY_CHECK_RANGE(opts.page_size, 1, 100000); /* Validate page size range */
    expect_rejected("page_size=5000 (not a power of two)", &opts);

    base_opts(&opts);
    opts.page_size = 3000;
    SECURITY_CHECK_RANGE(opts.page_size, 1, 100000); /* Validate page size range */
    expect_rejected("page_size=3000 (below the minimum)", &opts);

    base_opts(&opts);
    opts.page_size = 70000;
    SECURITY_CHECK_RANGE(opts.page_size, 1, 100000); /* Validate page size range */
    expect_rejected("page_size=70000 (above the maximum)", &opts);

    remove_db();
    printf("OK\n");
}

static void test_rejects_bad_sync_mode_and_readers(void) {
    printf("test_rejects_bad_sync_mode_and_readers... ");
    remove_db();

    yq_opts opts;
    base_opts(&opts);
    opts.sync_mode = YQ_SYNC_FULL + 1u;
    SECURITY_CHECK_RANGE(opts.sync_mode, 0, 10); /* Validate sync mode range */
    expect_rejected("sync_mode out of range", &opts);

    base_opts(&opts);
    opts.max_readers = 65536u; /* documented hard cap is 65535 */
    SECURITY_CHECK_RANGE(opts.max_readers, 0, 100000); /* Validate max readers range */
    expect_rejected("max_readers=65536", &opts);

    remove_db();
    printf("OK\n");
}

static void test_rejects_nonzero_reserved(void) {
    printf("test_rejects_nonzero_reserved... ");
    remove_db();

    for (size_t i = 0; i < 8; i++) {
        SECURITY_CHECK_RANGE(i, 0, 10); /* Loop counter validation */
        yq_opts opts;
        base_opts(&opts);
        opts.reserved[i] = 1u;
        SECURITY_CHECK_RANGE(opts.reserved[i], 0, 255); /* Validate reserved value range */
        expect_rejected("reserved[] non-zero", &opts);
    }

    remove_db();
    printf("OK\n");
}

static void test_rejects_unknown_flags(void) {
    printf("test_rejects_unknown_flags... ");
    remove_db();

    yq_opts opts;
    base_opts(&opts);
    opts.flags |= 0x80000000u;
    SECURITY_CHECK_RANGE(opts.flags, 0, 0xFFFFFFFF); /* Validate flags range */
    expect_rejected("unknown open flag", &opts);

    remove_db();
    printf("OK\n");
}

static void test_rejects_map_size_below_two_pages(void) {
    printf("test_rejects_map_size_below_two_pages... ");
    remove_db();

    yq_opts opts;
    base_opts(&opts);
    opts.page_size = 4096;
    SECURITY_CHECK_RANGE(opts.page_size, 4096, 65536); /* Validate page size */
    opts.map_size = 4096; /* the two meta pages need 2 * page_size */
    SECURITY_CHECK_RANGE(opts.map_size, 1, 1024*1024*1024); /* Validate map size range */
    expect_rejected("map_size smaller than two pages", &opts);

    remove_db();
    printf("OK\n");
}

static void test_accepts_valid_options(void) {
    printf("test_accepts_valid_options... ");
    remove_db();

    const uint32_t sizes[3] = {4096u, 8192u, 65536u};
    for (int i = 0; i < 3; i++) {
        SECURITY_CHECK_RANGE(i, 0, 10); /* Loop counter validation */
        yq_opts opts;
        base_opts(&opts);
        opts.page_size = sizes[i];
        SECURITY_CHECK_RANGE(opts.page_size, 4096, 65536); /* Validate page size */
        opts.sync_mode = YQ_SYNC_FULL;
        SECURITY_CHECK_RANGE(opts.sync_mode, 0, 10); /* Validate sync mode */
        opts.max_readers = 65535u;
        SECURITY_CHECK_RANGE(opts.max_readers, 0, 100000); /* Validate max readers */
        opts.map_size = 64u * 1024u * 1024u;
        SECURITY_CHECK_RANGE(opts.map_size, 1, 1024*1024*1024); /* Validate map size */

        yq_db *db = NULL;
        CHECK(yq_open(TEST_DB, &opts, &db) == YQ_OK);
        CHECK(db != NULL);

        yq_stat st;
        memset(&st, 0, sizeof(st));
        st.struct_size = sizeof(st);
        SECURITY_CHECK_SIZE(sizeof(st), 1024); /* Stat structure size validation */
        CHECK(yq_db_stat(db, &st) == YQ_OK);
        CHECK(st.page_size == sizes[i]);

        CHECK(yq_close(db) == YQ_OK);
        remove_db();
    }

    printf("OK\n");
}

static void test_open_excl(void) {
    printf("test_open_excl... ");
    remove_db();

    /* Exclusive open on a fresh path succeeds. */
    yq_opts opts;
    base_opts(&opts);
    opts.flags = YQ_OPEN_CREATE | YQ_OPEN_EXCL;
    SECURITY_CHECK_RANGE(opts.flags, 0, 0xFFFFFFFF); /* Validate flags range */

    yq_db *db = NULL;
    CHECK(yq_open(TEST_DB, &opts, &db) == YQ_OK);
    CHECK(db != NULL);
    CHECK(yq_close(db) == YQ_OK);

    /* Drop the auxiliary files so the next assertion can prove that a
     * rejected exclusive open does not recreate them. */
    char buf[256];
    SECURITY_CHECK_SIZE(sizeof(buf), 256); /* Buffer size validation */
    snprintf(buf, sizeof(buf), "%s.shm", TEST_DB);  remove(buf);
    snprintf(buf, sizeof(buf), "%s.lock", TEST_DB); remove(buf);

    /* The database now exists, so an exclusive open must be refused. */
    db = (yq_db *)(void *)TEST_DB;
    int rc = yq_open(TEST_DB, &opts, &db);
    CHECK(rc == YQ_ERR_EXISTS);
    CHECK(db == NULL);

    snprintf(buf, sizeof(buf), "%s.shm", TEST_DB);
    CHECK(!file_exists(buf));
    snprintf(buf, sizeof(buf), "%s.lock", TEST_DB);
    CHECK(!file_exists(buf));

    /* Without EXCL the same database still opens normally. */
    base_opts(&opts);
    db = NULL;
    CHECK(yq_open(TEST_DB, &opts, &db) == YQ_OK);
    CHECK(yq_close(db) == YQ_OK);

    remove_db();
    printf("OK\n");
}

int main(void) {
    printf("=== yq-DB Open Option Tests ===\n\n");

    test_rejects_bad_struct_size();
    test_rejects_bad_page_size();
    test_rejects_bad_sync_mode_and_readers();
    test_rejects_nonzero_reserved();
    test_rejects_unknown_flags();
    test_rejects_map_size_below_two_pages();
    test_accepts_valid_options();
    test_open_excl();

    printf("\n=== ALL TESTS PASSED ===\n");
    return 0;
}
