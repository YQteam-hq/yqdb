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

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("[1] yq_version\n"); fflush(stdout);
    int ma=0,mi=0,pa=0;
    yq_version(&ma,&mi,&pa);
    printf("[2] done v=%d.%d.%d\n", ma,mi,pa); fflush(stdout);

    CreateDirectoryA("build", NULL);
    DeleteFileA("build\\yqtest.yqdb"); DeleteFileA("build\\yqtest.yqdb.shm");
    DeleteFileA("build\\yqtest.yqdb.lock"); DeleteFileA("build\\yqtest.yqdb.log");

    printf("[3] yq_file_open db\n"); fflush(stdout);
    yq_file *fdb = yq_file_open("build\\yqtest.yqdb", 1, 1);
    printf("[4] fdb=%p\n", (void*)fdb); fflush(stdout);
    if (!fdb) { printf("FAIL\n"); return 1; }

    printf("[5] yq_file_open shm\n"); fflush(stdout);
    yq_file *fshm = yq_file_open("build\\yqtest.yqdb.shm", 1, 1);
    printf("[6] fshm=%p\n", (void*)fshm); fflush(stdout);
    if (!fshm) { printf("FAIL\n"); return 1; }

    printf("[7] yq_file_open lock\n"); fflush(stdout);
    yq_file *flock = yq_file_open("build\\yqtest.yqdb.lock", 1, 1);
    printf("[8] flock=%p\n", (void*)flock); fflush(stdout);
    if (!flock) { printf("FAIL\n"); return 1; }

    printf("[9] yq_mvcc_open\n"); fflush(stdout);
    yq_mvcc *mvcc = NULL;
    int rc = yq_mvcc_open(&mvcc, fdb, fshm, flock, 4);
    printf("[10] mvcc=%p rc=%d\n", (void*)mvcc, rc); fflush(stdout);
    if (rc != 0 || !mvcc) { printf("FAIL mvcc rc=%d\n", rc); return 1; }

    printf("[11] yq_wal_open\n"); fflush(stdout);
    yq_wal *wal = NULL;
    rc = yq_wal_open(&wal, "build\\yqtest.yqdb", 4096);
    printf("[12] wal=%p rc=%d\n", (void*)wal, rc); fflush(stdout);
    if (rc != 0 || !wal) { printf("FAIL wal rc=%d\n", rc); return 1; }

    printf("[13] yq_memtable_create\n"); fflush(stdout);
    yq_memtable *mt = yq_memtable_create(1024*1024);
    printf("[14] mt=%p\n", (void*)mt); fflush(stdout);
    if (!mt) { printf("FAIL mt\n"); return 1; }

    printf("[15] yq_memtable_put\n"); fflush(stdout);
    yq_slice k1 = { .data = (const void*)"k1", .size = 2 };
    yq_slice v1 = { .data = (const void*)"v1", .size = 2 };
    rc = yq_memtable_put(mt, k1, v1);
    printf("[16] put rc=%d\n", rc); fflush(stdout);

    printf("[17] yq_memtable_get\n"); fflush(stdout);
    yq_slice out = {0};
    rc = yq_memtable_get(mt, k1, &out);
    printf("[18] get rc=%d out.size=%zu\n", rc, out.size); fflush(stdout);

    printf("[19] yq_memtable_destroy\n"); fflush(stdout);
    yq_memtable_destroy(mt);
    printf("[20] destroyed\n"); fflush(stdout);

    printf("[21] yq_wal_close\n"); fflush(stdout);
    yq_wal_close(wal);
    printf("[22] wal closed\n"); fflush(stdout);

    printf("[23] yq_mvcc_close\n"); fflush(stdout);
    yq_mvcc_close(mvcc);
    printf("[24] mvcc closed\n"); fflush(stdout);

    printf("[25] yq_file_close all\n"); fflush(stdout);
    yq_file_close(flock); yq_file_close(fshm); yq_file_close(fdb);
    printf("[26] ALL DONE OK\n"); fflush(stdout);
    return 0;
}
