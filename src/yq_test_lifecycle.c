/*
 * Lifecycle regression tests for yq_open() / yq_close().
 *
 * free_db() used to skip the B+Tree handle entirely (`if (db->btree) { }`)
 * and never released the heap arena yq_open() mallocs when there is no file
 * contents to map, so every open/close cycle leaked both. LeakSanitizer sees
 * it immediately:
 *
 *     Direct leak of 96 byte(s) in 1 object(s) allocated from:
 *         #1 yq_btree_open src/yq_btree.c:701
 *
 * This suite does not assert on byte counts -- it just exercises the paths,
 * and the sanitizer job in CI fails if any of them leak or trip UB.
 *
 * Checks use an explicit macro instead of assert() so that they stay active
 * regardless of whether the build type defines NDEBUG.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yq.h"

static const char *TEST_DB = "yqtest_lifecycle.yqdb";

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                     \
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

static void open_db(yq_db **db, uint32_t flags) {
    yq_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(opts);
    opts.flags = flags;

    CHECK(yq_open(TEST_DB, &opts, db) == YQ_OK);
    CHECK(*db != NULL);
}

/* The leak is per-cycle, so cycle enough times to make it unmistakable. */
static void test_repeated_open_close(void) {
    printf("test_repeated_open_close... ");
    remove_db();

    for (int i = 0; i < 32; i++) {
        yq_db *db = NULL;
        open_db(&db, YQ_OPEN_CREATE);
        CHECK(yq_close(db) == YQ_OK);
    }

    remove_db();
    printf("OK\n");
}

/* A read-only handle takes a different path through yq_open(). */
static void test_repeated_readonly_open_close(void) {
    printf("test_repeated_readonly_open_close... ");
    remove_db();

    yq_db *w = NULL;
    open_db(&w, YQ_OPEN_CREATE);
    CHECK(yq_close(w) == YQ_OK);

    for (int i = 0; i < 32; i++) {
        yq_db *db = NULL;
        open_db(&db, YQ_OPEN_READONLY);
        CHECK(yq_close(db) == YQ_OK);
    }

    remove_db();
    printf("OK\n");
}

/* Reopening must keep working after many cycles, not just not crash. */
static void test_reopen_after_cycles(void) {
    printf("test_reopen_after_cycles... ");
    remove_db();

    for (int i = 0; i < 8; i++) {
        yq_db *db = NULL;
        open_db(&db, YQ_OPEN_CREATE);
        CHECK(yq_close(db) == YQ_OK);
    }

    yq_db *db = NULL;
    open_db(&db, YQ_OPEN_CREATE);

    yq_stat st;
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK(yq_db_stat(db, &st) == YQ_OK);
    CHECK(st.format_version == 1);
    CHECK(st.page_size == 4096);

    CHECK(yq_close(db) == YQ_OK);

    remove_db();
    printf("OK\n");
}

/* yq_close(NULL) is documented to be a no-op returning YQ_OK. */
static void test_close_null(void) {
    printf("test_close_null... ");
    CHECK(yq_close(NULL) == YQ_OK);
    printf("OK\n");
}

int main(void) {
    printf("=== yq-DB Lifecycle Tests ===\n\n");

    test_close_null();
    test_repeated_open_close();
    test_repeated_readonly_open_close();
    test_reopen_after_cycles();

    printf("\n=== ALL TESTS PASSED ===\n");
    return 0;
}
