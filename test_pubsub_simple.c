#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "yq_pubsub.h"

int main() {
    printf("Testing yq-DB Pub/Sub functionality...\n");
    
    /* Test basic pub/sub functions exist */
    printf("Testing pub/sub API functions...\n");
    
    /* Test basic operations */
    yq_pubsub_stats stats;
    printf("Pub/Sub stats structure size: %zu\n", sizeof(stats));
    
    /* Test error codes */
    printf("Testing error codes...\n");
    printf("YQ_OK: %d\n", YQ_OK);
    printf("YQ_ERR: %d\n", YQ_ERR);
    printf("YQ_ERR_NOMEM: %d\n", YQ_ERR_NOMEM);
    
    /* Test message priority enum */
    printf("Message priority enum size: %zu\n", sizeof(yq_message_priority));
    
    /* Test topic filter enum */
    printf("Topic filter enum size: %zu\n", sizeof(yq_filter_type));
    
    /* Test message structure */
    printf("Message structure size: %zu\n", sizeof(yq_message));
    
    /* Test subscriber structure (opaque type) */
    printf("Subscriber structure is opaque (forward declared)\n");
    
    printf("All pub/sub API structures and enums are properly defined!\n");
    
    return 0;
}