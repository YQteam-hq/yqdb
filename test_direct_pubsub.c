#include "yq.h"
#include <stdio.h>

int main() {
    printf("Testing direct inclusion of yq.h and pubsub...\n");
    
    // Check if we can access yq_db type
    yq_db *db = NULL;
    printf("✓ yq_db type is available\n");
    
    // Check if pubsub functions are available
    #if YQ_ENABLE_PUBSUB
    printf("✓ YQ_ENABLE_PUBSUB is defined\n");
    
    // Try to access pubsub types
    yq_pubsub *pubsub = NULL;
    printf("✓ yq_pubsub type is available\n");
    #else
    printf("✗ YQ_ENABLE_PUBSUB is not defined\n");
    #endif
    
    printf("Test completed.\n");
    return 0;
}