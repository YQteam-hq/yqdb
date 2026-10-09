#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "yq.h"
#include "yq_vfs.h"
#include "yq_wal.h"
#include "yq_mvcc.h"
#include "yq_memtable.h"
#include "yq_btree.h"

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

/* Security-enhanced memory allocation */
static void* secure_malloc(size_t size) {
    if (size == 0 || size > 1024 * 1024 * 1024) { /* 1GB max */
        fprintf(stderr, "Security failure: Invalid allocation size %zu\n", size);
        return NULL;
    }
    void *ptr = malloc(size);
    if (ptr) {
        memset(ptr, 0, size); /* Zero-fill for security */
    }
    return ptr;
}

/* Security-enhanced file operations */
static int secure_remove_file(const char *path) {
    if (!path || strlen(path) == 0 || strlen(path) > 256) {
        return -1;
    }
    return remove(path);
}

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

    printf("[1] yq_version\n"); fflush(stdout);
    int ma=0,mi=0,pa=0;
    SECURITY_CHECK_NULL(&ma); SECURITY_CHECK_NULL(&mi); SECURITY_CHECK_NULL(&pa);
    yq_version(&ma,&mi,&pa);
    printf("[2] done v=%d.%d.%d\n", ma,mi,pa); fflush(stdout);

    /* Security-enhanced directory and file operations */
    if (!validate_db_path("build")) {
        fprintf(stderr, "Security failure: Invalid build path\n");
        return 1;
    }
    
    SECURITY_CHECK_RANGE(ma, 0, 1000); SECURITY_CHECK_RANGE(mi, 0, 1000); SECURITY_CHECK_RANGE(pa, 0, 1000);
    
    CreateDirectoryA("build", NULL);
    if (secure_remove_file("build\\yqtest.yqdb") != 0) fprintf(stderr, "Warning: failed to remove yqtest.yqdb\n");
    if (secure_remove_file("build\\yqtest.yqdb.shm") != 0) fprintf(stderr, "Warning: failed to remove yqtest.yqdb.shm\n");
    if (secure_remove_file("build\\yqtest.yqdb.lock") != 0) fprintf(stderr, "Warning: failed to remove yqtest.yqdb.lock\n");
    if (secure_remove_file("build\\yqtest.yqdb.log") != 0) fprintf(stderr, "Warning: failed to remove yqtest.yqdb.log\n");

    printf("[3] yq_file_open db\n"); fflush(stdout);
    if (!validate_db_path("build\\yqtest.yqdb")) {
        fprintf(stderr, "Security failure: Invalid database path\n");
        return 1;
    }
    yq_file *fdb = yq_file_open("build\\yqtest.yqdb", 1, 1);
    printf("[4] fdb=%p\n", (void*)fdb); fflush(stdout);
    if (!fdb) { printf("FAIL\n"); return 1; }

    printf("[5] yq_file_open shm\n"); fflush(stdout);
    if (!validate_db_path("build\\yqtest.yqdb.shm")) {
        fprintf(stderr, "Security failure: Invalid shared memory path\n");
        return 1;
    }
    yq_file *fshm = yq_file_open("build\\yqtest.yqdb.shm", 1, 1);
    printf("[6] fshm=%p\n", (void*)fshm); fflush(stdout);
    if (!fshm) { printf("FAIL\n"); return 1; }

    printf("[7] yq_file_open lock\n"); fflush(stdout);
    if (!validate_db_path("build\\yqtest.yqdb.lock")) {
        fprintf(stderr, "Security failure: Invalid lock file path\n");
        return 1;
    }
    yq_file *flock = yq_file_open("build\\yqtest.yqdb.lock", 1, 1);
    printf("[8] flock=%p\n", (void*)flock); fflush(stdout);
    if (!flock) { printf("FAIL\n"); return 1; }

    printf("[9] yq_mvcc_open\n"); fflush(stdout);
    SECURITY_CHECK_NULL(fdb); SECURITY_CHECK_NULL(fshm); SECURITY_CHECK_NULL(flock);
    yq_mvcc *mvcc = NULL;
    SECURITY_CHECK_NULL(&mvcc);
    int rc = yq_mvcc_open(&mvcc, fdb, fshm, flock, 4);
    printf("[10] mvcc=%p rc=%d\n", (void*)mvcc, rc); fflush(stdout);
    if (rc != 0 || !mvcc) { printf("FAIL mvcc rc=%d\n", rc); return 1; }

    printf("[11] yq_wal_open\n"); fflush(stdout);
    if (!validate_db_path("build\\yqtest.yqdb")) {
        fprintf(stderr, "Security failure: Invalid WAL path\n");
        return 1;
    }
    yq_wal *wal = NULL;
    SECURITY_CHECK_NULL(&wal);
    rc = yq_wal_open(&wal, "build\\yqtest.yqdb", 4096);
    printf("[12] wal=%p rc=%d\n", (void*)wal, rc); fflush(stdout);
    if (rc != 0 || !wal) { printf("FAIL wal rc=%d\n", rc); return 1; }

    printf("[13] yq_memtable_create\n"); fflush(stdout);
    SECURITY_CHECK_RANGE(1024*1024, 1, 1024*1024*1024);
    yq_memtable *mt = yq_memtable_create(1024*1024);
    printf("[14] mt=%p\n", (void*)mt); fflush(stdout);
    if (!mt) { printf("FAIL mt\n"); return 1; }

    printf("[15] yq_memtable_put\n"); fflush(stdout);
    SECURITY_CHECK_NULL(mt);
    yq_slice k1 = { .data = (const void*)"k1", .size = 2 };
    yq_slice v1 = { .data = (const void*)"v1", .size = 2 };
    SECURITY_CHECK_SIZE(k1.size, 1024*1024);
    SECURITY_CHECK_SIZE(v1.size, 1024*1024);
    rc = yq_memtable_put(mt, k1, v1);
    printf("[16] put rc=%d\n", rc); fflush(stdout);

    printf("[17] yq_memtable_get\n"); fflush(stdout);
    yq_slice out = {0};
    SECURITY_CHECK_NULL(&out);
    rc = yq_memtable_get(mt, k1, &out);
    printf("[18] get rc=%d out.size=%zu\n", rc, out.size); fflush(stdout);

    printf("[19] yq_memtable_destroy\n"); fflush(stdout);
    SECURITY_CHECK_NULL(mt);
    yq_memtable_destroy(mt);
    printf("[20] destroyed\n"); fflush(stdout);

    printf("[21] yq_wal_close\n"); fflush(stdout);
    SECURITY_CHECK_NULL(wal);
    yq_wal_close(wal);
    printf("[22] wal closed\n"); fflush(stdout);

    printf("[23] yq_mvcc_close\n"); fflush(stdout);
    SECURITY_CHECK_NULL(mvcc);
    yq_mvcc_close(mvcc);
    printf("[24] mvcc closed\n"); fflush(stdout);

    printf("[25] yq_file_close all\n"); fflush(stdout);
    SECURITY_CHECK_NULL(flock); SECURITY_CHECK_NULL(fshm); SECURITY_CHECK_NULL(fdb);
    yq_file_close(flock); yq_file_close(fshm); yq_file_close(fdb);
    
    /* Security-enhanced cleanup */
    if (secure_remove_file("build\\yqtest.yqdb") != 0) fprintf(stderr, "Warning: failed to cleanup yqtest.yqdb\n");
    if (secure_remove_file("build\\yqtest.yqdb.shm") != 0) fprintf(stderr, "Warning: failed to cleanup yqtest.yqdb.shm\n");
    if (secure_remove_file("build\\yqtest.yqdb.lock") != 0) fprintf(stderr, "Warning: failed to cleanup yqtest.yqdb.lock\n");
    if (secure_remove_file("build\\yqtest.yqdb.log") != 0) fprintf(stderr, "Warning: failed to cleanup yqtest.yqdb.log\n");
    
    printf("[26] ALL DONE OK\n"); fflush(stdout);
    return 0;
}
