#include "yq.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Simple test to check if pubsub is properly compiled
int main() {
    printf("Testing yq-DB with Pub/Sub support...\n");
    
    // Check if YQ_ENABLE_PUBSUB is defined
    #if YQ_ENABLE_PUBSUB
    printf("✓ YQ_ENABLE_PUBSUB is defined\n");
    #else
    printf("✗ YQ_ENABLE_PUBSUB is not defined\n");
    #endif
    
    // Try to open database
    yq_db *db;
    yq_opts opts = {sizeof(yq_opts), 0};
    
    if (yq_open("test_simple.db", &opts, &db) == YQ_OK) {
        printf("✓ Database opened successfully\n");
        
        // Try to close database
        if (yq_close(db) == YQ_OK) {
            printf("✓ Database closed successfully\n");
        } else {
            printf("✗ Failed to close database\n");
        }
    } else {
        printf("✗ Failed to open database\n");
    }
    
    // Clean up
    remove("test_simple.db");
    
    printf("Test completed.\n");
    return 0;
}