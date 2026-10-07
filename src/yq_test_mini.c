#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "yq.h"

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("yq_version... ");
    fflush(stdout);
    int ma=0,mi=0,pa=0;
    int rc = yq_version(&ma,&mi,&pa);
    printf("rc=%d v=%d.%d.%d\n", rc, ma, mi, pa);
    fflush(stdout);

    printf("yq_open... ");
    fflush(stdout);
    yq_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(opts);
    opts.flags = YQ_OPEN_CREATE;
    opts.page_size = 4096;
    opts.memtable_bytes = 1024*1024;

    yq_db *db = NULL;
    rc = yq_open("yqtest.yqdb", &opts, &db);
    printf("rc=%d db=%p\n", rc, (void*)db);
    fflush(stdout);

    printf("yq_txn_begin... ");
    fflush(stdout);
    yq_txn *txn = NULL;
    rc = yq_txn_begin(db, YQ_TXN_READWRITE, &txn);
    printf("rc=%d txn=%p\n", rc, (void*)txn);
    fflush(stdout);

    printf("yq_put... ");
    fflush(stdout);
    yq_slice k = { .data = (const void*)"hello", .size = 5 };
    yq_slice v = { .data = (const void*)"world", .size = 5 };
    rc = yq_put(txn, k, v, 0);
    printf("rc=%d\n", rc);
    fflush(stdout);

    printf("yq_txn_commit... ");
    fflush(stdout);
    rc = yq_txn_commit(txn);
    printf("rc=%d\n", rc);
    fflush(stdout);

    printf("yq_close... ");
    fflush(stdout);
    rc = yq_close(db);
    printf("rc=%d\n", rc);
    fflush(stdout);

    printf("DONE\n");
    fflush(stdout);
    return 0;
}

