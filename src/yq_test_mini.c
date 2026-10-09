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

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("yq_version... ");
    fflush(stdout);
    int ma=0,mi=0,pa=0;
    SECURITY_CHECK_NULL(&ma); SECURITY_CHECK_NULL(&mi); SECURITY_CHECK_NULL(&pa);
    int rc = yq_version(&ma,&mi,&pa);
    printf("rc=%d v=%d.%d.%d\n", rc, ma, mi, pa);
    fflush(stdout);

    printf("yq_open... ");
    fflush(stdout);
    if (!validate_db_path("yqtest.yqdb")) {
        fprintf(stderr, "Security failure: Invalid database path\n");
        return 1;
    }
    yq_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(opts);
    opts.flags = YQ_OPEN_CREATE;
    SECURITY_CHECK_RANGE(opts.flags, 0, 0xFFFFFFFF);
    opts.page_size = 4096;
    SECURITY_CHECK_RANGE(opts.page_size, 4096, 65536);
    opts.memtable_bytes = 1024*1024;
    SECURITY_CHECK_RANGE(opts.memtable_bytes, 1024, 1024*1024*1024);

    yq_db *db = NULL;
    SECURITY_CHECK_NULL(&db);
    rc = yq_open("yqtest.yqdb", &opts, &db);
    printf("rc=%d db=%p\n", rc, (void*)db);
    fflush(stdout);

    printf("yq_txn_begin... ");
    fflush(stdout);
    SECURITY_CHECK_NULL(db);
    yq_txn *txn = NULL;
    SECURITY_CHECK_NULL(&txn);
    rc = yq_txn_begin(db, YQ_TXN_READWRITE, &txn);
    printf("rc=%d txn=%p\n", rc, (void*)txn);
    fflush(stdout);

    printf("yq_put... ");
    fflush(stdout);
    yq_slice k = { .data = (const void*)"hello", .size = 5 };
    yq_slice v = { .data = (const void*)"world", .size = 5 };
    SECURITY_CHECK_SIZE(k.size, 1024*1024);
    SECURITY_CHECK_SIZE(v.size, 1024*1024);
    rc = yq_put(txn, k, v, 0);
    printf("rc=%d\n", rc);
    fflush(stdout);

    printf("yq_txn_commit... ");
    fflush(stdout);
    SECURITY_CHECK_NULL(txn);
    rc = yq_txn_commit(txn);
    printf("rc=%d\n", rc);
    fflush(stdout);

    printf("yq_close... ");
    fflush(stdout);
    SECURITY_CHECK_NULL(db);
    rc = yq_close(db);
    printf("rc=%d\n", rc);
    fflush(stdout);

    printf("DONE\n");
    fflush(stdout);
    return 0;
}

