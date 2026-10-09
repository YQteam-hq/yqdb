#include <stdio.h>
#include <string.h>
#include "yq.h"

int main(void) {
    yq_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(opts);
    opts.flags = YQ_OPEN_CREATE;
    
    yq_db *db = NULL;
    if (yq_open("test.yqdb", &opts, &db) != YQ_OK) {
        printf("Failed to open database\n");
        return 1;
    }
    
    // Test basic operations
    yq_txn *txn = NULL;
    yq_txn_begin(db, YQ_TXN_READWRITE, &txn);
    
    yq_slice k = { "hello", 5 };
    yq_slice v = { "world", 5 };
    int rc = yq_put(txn, k, v, YQ_PUT_UPSERT);
    if (rc != YQ_OK) {
        printf("Failed to put: %d\n", rc);
        return 1;
    }
    
    yq_slice out = { 0 };
    rc = yq_get(txn, k, &out);
    if (rc != YQ_OK) {
        printf("Failed to get: %d\n", rc);
        return 1;
    }
    
    printf("Successfully stored and retrieved: %.*s\n", (int)out.size, (const char *)out.data);
    
    yq_txn_commit(txn);
    yq_close(db);
    
    printf("All tests passed!\n");
    return 0;
}