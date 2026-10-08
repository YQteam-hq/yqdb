# yq-DB Pub/Sub Advanced Features

## Table of Contents

1. [Message Delivery Modes](#message-delivery-modes)
2. [Advanced Filtering](#advanced-filtering)
3. [Message Persistence](#message-persistence)
4. [Retry Mechanisms](#retry-mechanisms)
5. [Message Metadata](#message-metadata)
6. [Performance Optimization](#performance-optimization)
7. [Monitoring and Debugging](#monitoring-and-debugging)
8. [Integration with Other Features](#integration-with-other-features)
9. [Error Recovery Strategies](#error-recovery-strategies)
10. [Security Considerations](#security-considerations)

## Message Delivery Modes

### Understanding Delivery Modes

yq-DB Pub/Sub supports three delivery modes to ensure reliable message delivery:

```c
typedef enum yq_delivery_mode {
    YQ_DELIVERY_AT_MOST_ONCE = 0,  /* Fire and forget */
    YQ_DELIVERY_AT_LEAST_ONCE = 1, /* At least one delivery */
    YQ_DELIVERY_EXACTLY_ONCE = 2  /* Exactly one delivery */
} yq_delivery_mode;
```

### Implementation Examples

#### At-Most-Once Delivery

```c
// Simple, fast delivery with no guarantees
yq_subscriber_opts opts = {
    .struct_size = sizeof(yq_subscriber_opts),
    .queue_size = 100,
    .delivery_mode = YQ_DELIVERY_AT_MOST_ONCE
};

// Messages are delivered once or not at all
yq_subscribe(pubsub, "events", &opts, message_handler, error_handler, NULL, &sub);
```

#### At-Least-Once Delivery

```c
// Guaranteed delivery with possible duplicates
yq_subscriber_opts opts = {
    .struct_size = sizeof(yq_subscriber_opts),
    .queue_size = 1000,
    .max_retry = 3,
    .retry_delay_ms = 1000,
    .delivery_mode = YQ_DELIVERY_AT_LEAST_ONCE
};

// Messages are delivered at least once
yq_subscribe(pubsub, "critical_events", &opts, message_handler, error_handler, NULL, &sub);
```

#### Exactly-Once Delivery

```c
// Complex delivery with no duplicates (requires application logic)
yq_subscriber_opts opts = {
    .struct_size = sizeof(yq_subscriber_opts),
    .queue_size = 500,
    .delivery_mode = YQ_DELIVERY_EXACTLY_ONCE
};

// Application must implement idempotency
yq_subscribe(pubsub, "financial_transactions", &opts, 
             idempotent_handler, error_handler, NULL, &sub);
```

### Exactly-Once Implementation Pattern

```c
// Idempotent message handler
static int idempotent_handler(yq_subscriber *sub, const yq_message *msg, void *user_data) {
    // Generate unique key for this message
    char msg_key[128];
    snprintf(msg_key, sizeof(msg_key), "msg_%lu_%lu", 
             msg->message_id, msg->timestamp);
    
    // Check if message already processed
    if (yq_db_get(db, msg_key, NULL, 0) == YQ_OK) {
        printf("Message %lu already processed\n", msg->message_id);
        return 0; // Success, but don't retry
    }
    
    // Process the message
    int result = process_message(msg);
    
    if (result == 0) {
        // Store processed message ID
        yq_db_put(db, msg_key, &msg->message_id, sizeof(msg->message_id));
        return 0;
    } else {
        return result; // Will trigger retry
    }
}
```

## Advanced Filtering

### Filter Types and Usage

#### Prefix Filtering

```c
// Subscribe to all user events
yq_topic_filter prefix_filter = {
    .struct_size = sizeof(yq_topic_filter),
    .type = YQ_FILTER_PREFIX,
    .pattern_len = strlen("user."),
    .pattern = "user."
};

yq_subscribe_filtered(pubsub, &prefix_filter, NULL, 
                      user_event_handler, error_handler, NULL, &sub);
```

#### Custom Filtering Function

```c
// Custom filter function
static int custom_filter(const char *topic, const yq_topic_filter *filter) {
    // Custom logic: only allow topics with even numbers
    int number;
    if (sscanf(topic, "user.%d.events", &number) == 1) {
        return (number % 2) == 0;
    }
    return 0;
}

// Set custom filter
yq_topic_filter custom_filter = {
    .struct_size = sizeof(yq_topic_filter),
    .type = YQ_FILTER_CUSTOM,
    .pattern = "user.*.events"
};

// Apply filter in subscription
yq_subscribe_filtered(pubsub, &custom_filter, NULL, 
                      filtered_handler, error_handler, NULL, &sub);
```

### Complex Filtering Patterns

```c
// Multi-topic subscription with different filters
void setup_multiple_filters() {
    yq_subscriber *sub1, *sub2, *sub3;
    
    // Filter 1: User login events
    yq_topic_filter login_filter = {
        .struct_size = sizeof(yq_topic_filter),
        .type = YQ_FILTER_PREFIX,
        .pattern = "user.login."
    };
    yq_subscribe_filtered(pubsub, &login_filter, NULL, 
                         login_handler, error_handler, NULL, &sub1);
    
    // Filter 2: System alerts
    yq_topic_filter alert_filter = {
        .struct_size = sizeof(yq_topic_filter),
        .type = YQ_FILTER_PREFIX,
        .pattern = "system.alert."
    };
    yq_subscribe_filtered(pubsub, &alert_filter, NULL, 
                         alert_handler, error_handler, NULL, &sub2);
    
    // Filter 3: High-priority messages
    yq_topic_filter priority_filter = {
        .struct_size = sizeof(yq_topic_filter),
        .type = YQ_FILTER_PREFIX,
        .pattern = "urgent."
    };
    yq_subscribe_filtered(pubsub, &priority_filter, NULL, 
                         urgent_handler, error_handler, NULL, &sub3);
}
```

## Message Persistence

### Persistent Messages

```c
// Mark messages as persistent
uint32_t flags = YQ_MESSAGE_FLAG_PERSISTENT;
const char *payload = "Important data that must survive restarts";
size_t payload_len = strlen(payload);

uint64_t message_id;
yq_publish(pubsub, "persistent_topic", payload, payload_len, flags, &message_id);
```

### Persistence Implementation

```c
// Enhanced publisher with persistence
int publish_with_persistence(yq_pubsub *pubsub, const char *topic, 
                           const void *payload, size_t payload_len) {
    // Create message record in database
    char msg_key[64];
    snprintf(msg_key, sizeof(msg_key), "msg_%lu", yq_current_timestamp_ms());
    
    // Store message in database for persistence
    int db_result = yq_db_put(db, msg_key, payload, payload_len);
    if (db_result != YQ_OK) {
        return db_result;
    }
    
    // Publish with persistent flag
    uint32_t flags = YQ_MESSAGE_FLAG_PERSISTENT;
    uint64_t message_id;
    int pub_result = yq_publish(pubsub, topic, payload, payload_len, flags, &message_id);
    
    // Clean up database record after successful publish
    if (pub_result == YQ_OK) {
        yq_db_delete(db, msg_key);
    }
    
    return pub_result;
}
```

### Recovery of Persistent Messages

```c
// On startup, recover any pending persistent messages
void recover_persistent_messages(yq_pubsub *pubsub) {
    yq_iterator *iter;
    yq_db_iter_init(db, "msg_", &iter);
    
    char key[64];
    size_t key_len;
    void *value;
    size_t value_len;
    
    while (yq_db_iter_next(iter, key, &key_len, &value, &value_len) == YQ_OK) {
        // Republish persistent messages
        uint64_t timestamp;
        if (sscanf(key, "msg_%lu", &timestamp) == 1) {
            // Determine topic based on content or stored metadata
            const char *topic = "recovered";
            uint64_t message_id;
            
            yq_publish(pubsub, topic, value, value_len, 
                      YQ_MESSAGE_FLAG_PERSISTENT, &message_id);
            
            printf("Recovered message %lu to topic %s\n", message_id, topic);
            
            // Remove from database after recovery
            yq_db_delete(db, key);
        }
    }
    
    yq_db_iter_free(iter);
}
```

## Retry Mechanisms

### Configurable Retry Strategy

```c
// Advanced subscriber with exponential backoff
yq_subscriber_opts retry_opts = {
    .struct_size = sizeof(yq_subscriber_opts),
    .queue_size = 2000,
    .max_retry = 5,
    .retry_delay_ms = 1000,    // Initial delay
    .timeout_ms = 30000,      // 30 second timeout
    .delivery_mode = YQ_DELIVERY_AT_LEAST_ONCE
};

static int retry_handler(yq_subscriber *sub, const yq_message *msg, void *user_data) {
    static int retry_count = 0;
    
    if (retry_count >= 3) {
        printf("Max retries exceeded for message %lu\n", msg->message_id);
        return YQ_ERR_RETRY_EXCEEDED;
    }
    
    int result = process_with_external_service(msg);
    
    if (result != 0) {
        retry_count++;
        printf("Retry %d for message %lu\n", retry_count, msg->message_id);
        return result; // Will trigger retry with backoff
    }
    
    retry_count = 0;
    return 0;
}
```

### Dead Letter Queue Pattern

```c
// Dead letter queue for failed messages
static int dlq_handler(yq_subscriber *sub, const yq_message *msg, void *user_data) {
    int result = process_message(msg);
    
    if (result != 0) {
        // Send to dead letter queue
        char dlq_topic[64];
        snprintf(dlq_topic, sizeof(dlq_topic), "dlq.%s", msg->topic);
        
        uint64_t dlq_message_id;
        yq_publish(pubsub, dlq_topic, msg->payload, msg->payload_len, 
                  msg->flags, &dlq_message_id);
        
        printf("Message %lu sent to dead letter queue as %lu\n", 
               msg->message_id, dlq_message_id);
        
        return 0; // Don't retry
    }
    
    return 0;
}

// Set up dead letter queue subscriber
void setup_dlq() {
    yq_subscriber *dlq_sub;
    yq_topic_filter dlq_filter = {
        .struct_size = sizeof(yq_topic_filter),
        .type = YQ_FILTER_PREFIX,
        .pattern = "dlq."
    };
    
    yq_subscribe_filtered(pubsub, &dlq_filter, NULL, 
                         dlq_handler, error_handler, NULL, &dlq_sub);
}
```

## Message Metadata

### Working with Metadata

```c
// Publish message with metadata
typedef struct message_metadata {
    char sender[64];
    uint32_t priority;
    char source[32];
} message_metadata;

message_metadata meta = {
    .sender = "system.monitor",
    .priority = 1,
    .source = "production"
};

uint32_t flags = 0;
uint64_t message_id;

yq_publish_ex(pubsub, "metrics", "CPU: 85%", 8, 
              &meta, sizeof(meta), flags, YQ_MESSAGE_NORMAL, &message_id);
```

### Metadata Processing

```c
// Enhanced message handler with metadata processing
static int metadata_handler(yq_subscriber *sub, const yq_message *msg, void *user_data) {
    // Process payload
    printf("Message: %.*s\n", (int)msg->payload_len, (const char *)msg->payload);
    
    // Process metadata if available
    if (msg->metadata_len > 0) {
        message_metadata *meta = (message_metadata *)msg->metadata;
        printf("From: %s, Priority: %u, Source: %s\n", 
               meta->sender, meta->priority, meta->source);
        
        // Route based on priority
        if (meta->priority >= 2) {
            process_high_priority(msg);
        } else {
            process_normal_priority(msg);
        }
    }
    
    return 0;
}
```

## Performance Optimization

### Batch Operations

```c
// High-performance batch publisher
void batch_publisher() {
    yq_message *messages[100];
    uint64_t message_ids[100];
    
    // Create batch of messages
    for (int i = 0; i < 100; i++) {
        char topic[32];
        char payload[64];
        
        snprintf(topic, sizeof(topic), "batch.%d", i);
        snprintf(payload, sizeof(payload), "Batch message %d", i);
        
        yq_message_create(i, topic, payload, strlen(payload), 
                         NULL, 0, 0, YQ_MESSAGE_NORMAL, &messages[i]);
    }
    
    // Publish all messages at once
    yq_publish_batch(pubsub, messages, 100, message_ids);
    
    // Clean up
    for (int i = 0; i < 100; i++) {
        yq_message_free(messages[i]);
    }
}
```

### Queue Management

```c
// Advanced queue management
void manage_queue_efficiently() {
    yq_message_queue *queue;
    yq_message_queue_get(sub, &queue);
    
    // Batch poll for better performance
    yq_message *messages[10];
    size_t count;
    
    while (1) {
        // Poll multiple messages at once
        int result = yq_message_queue_poll_batch(queue, messages, 10, 100, &count);
        
        if (result == YQ_OK && count > 0) {
            // Process batch
            for (size_t i = 0; i < count; i++) {
                process_message(messages[i]);
                yq_message_ack(queue, messages[i]);
                yq_message_free(messages[i]);
            }
        } else if (result == YQ_ERR_NOTFOUND) {
            // No messages, wait a bit
            usleep(10000); // 10ms
        }
    }
}
```

### Memory Pool Allocation

```c
// Memory pool for better performance
#define MESSAGE_POOL_SIZE 1000
#define MESSAGE_POOL_SIZE 1000

typedef struct message_pool {
    yq_message pool[MESSAGE_POOL_SIZE];
    int used[MESSAGE_POOL_SIZE];
    int next_free;
} message_pool;

static message_pool msg_pool = {0};

yq_message *pool_alloc_message() {
    if (msg_pool.next_free >= MESSAGE_POOL_SIZE) {
        return NULL;
    }
    
    int index = msg_pool.next_free++;
    msg_pool.used[index] = 1;
    return &msg_pool.pool[index];
}

void pool_free_message(yq_message *msg) {
    // Find and free the message
    for (int i = 0; i < MESSAGE_POOL_SIZE; i++) {
        if (&msg_pool.pool[i] == msg && msg_pool.used[i]) {
            msg_pool.used[i] = 0;
            msg_pool.next_free = i; // Reuse this slot next
            return;
        }
    }
}
```

## Monitoring and Debugging

### Comprehensive Monitoring

```c
// Advanced monitoring system
void setup_monitoring() {
    yq_pubsub_stats stats;
    
    while (1) {
        // Get current statistics
        if (yq_pubsub_stats(pubsub, &stats) == YQ_OK) {
            printf("=== PUBSUB STATS ===\n");
            printf("Total Messages: %lu\n", stats.total_messages);
            printf("Delivered: %lu\n", stats.delivered_messages);
            printf("Failed: %lu\n", stats.failed_messages);
            printf("Success Rate: %.2f%%\n", 
                   stats.total_messages > 0 ? 
                   (double)stats.delivered_messages / stats.total_messages * 100 : 0);
            printf("Active Topics: %u\n", stats.active_topics);
            printf("Active Subscribers: %u\n", stats.active_subscribers);
            printf("Queue Usage: %u%%\n", stats.queue_usage);
            printf("===================\n");
        }
        
        sleep(5); // Update every 5 seconds
    }
}
```

### Debug Mode with Detailed Logging

```c
// Enhanced debugging
void enable_debug_mode() {
    // Enable debug output
    yq_pubsub_set_debug(pubsub, 1);
    
    // Set up debug callback
    static char debug_buffer[4096];
    
    char *debug_info;
    if (yq_pubsub_get_debug_info(pubsub, &debug_info) == YQ_OK) {
        printf("Debug Info:\n%s\n", debug_info);
        free(debug_info);
    }
    
    // Monitor specific topics
    yq_topic_filter debug_filter = {
        .struct_size = sizeof(yq_topic_filter),
        .type = YQ_FILTER_PREFIX,
        .pattern = "debug."
    };
    
    yq_subscriber *debug_sub;
    yq_subscribe_filtered(pubsub, &debug_filter, NULL, 
                         debug_message_handler, error_handler, NULL, &debug_sub);
}
```

## Integration with Other Features

### Integration with Compression

```c
// Compressed message publishing
void publish_compressed_message(yq_pubsub *pubsub, const char *topic, 
                                const char *data, size_t data_len) {
    // Compress the data
    yq_compressor *compressor;
    yq_compressor_init(YQ_COMPRESSOR_SNAPPY, &compressor);
    
    size_t compressed_len;
    void *compressed_data = yq_compress(compressor, data, data_len, &compressed_len);
    
    if (compressed_data) {
        // Publish compressed message
        uint32_t flags = YQ_MESSAGE_FLAG_COMPRESSED;
        uint64_t message_id;
        
        yq_publish(pubsub, topic, compressed_data, compressed_len, flags, &message_id);
        
        // Clean up
        free(compressed_data);
        yq_compressor_close(compressor);
        
        printf("Published compressed message %lu (ratio: %.2f%%)\n", 
               message_id, (double)compressed_len / data_len * 100);
    }
}
```

### Integration with Encryption

```c
// Encrypted message publishing
void publish_encrypted_message(yq_pubsub *pubsub, const char *topic, 
                               const char *data, size_t data_len) {
    // Encrypt the data
    yq_crypto *crypto;
    yq_crypto_init(YQ_CRYPTO_AES256, &crypto);
    
    // Set encryption key (in real app, load from secure storage)
    const char *key = "my-secret-key-32-bytes-long";
    yq_crypto_set_key(crypto, key, strlen(key));
    
    size_t encrypted_len;
    void *encrypted_data = yq_encrypt(crypto, data, data_len, &encrypted_len);
    
    if (encrypted_data) {
        // Publish encrypted message
        uint32_t flags = YQ_MESSAGE_FLAG_ENCRYPTED;
        uint64_t message_id;
        
        yq_publish(pubsub, topic, encrypted_data, encrypted_len, flags, &message_id);
        
        // Clean up
        free(encrypted_data);
        yq_crypto_close(crypto);
        
        printf("Published encrypted message %lu\n", message_id);
    }
}
```

### Integration with TTL

```c
// Time-sensitive message with TTL
void publish_ttl_message(yq_pubsub *pubsub, const char *topic, 
                        const char *data, size_t data_len, uint32_t ttl_seconds) {
    // Add TTL metadata
    typedef struct ttl_metadata {
        uint64_t expiration_time;
    } ttl_metadata;
    
    ttl_metadata meta = {
        .expiration_time = yq_current_timestamp_ms() + (ttl_seconds * 1000)
    };
    
    // Publish with TTL metadata
    uint64_t message_id;
    yq_publish_ex(pubsub, topic, data, data_len, 
                  &meta, sizeof(meta), 0, YQ_MESSAGE_NORMAL, &message_id);
    
    printf("Published TTL message %lu (expires in %u seconds)\n", 
           message_id, ttl_seconds);
}
```

## Error Recovery Strategies

### Circuit Breaker Pattern

```c
// Circuit breaker for external service failures
typedef struct circuit_breaker {
    int failure_count;
    int failure_threshold;
    uint64_t last_failure_time;
    int state; // 0=closed, 1=open, 2=half-open
} circuit_breaker;

static circuit_breaker cb = {0, 5, 0, 0};

int circuit_breaker_check() {
    uint64_t now = yq_current_timestamp_ms();
    
    if (cb.state == 0) { // Closed
        return 1;
    } else if (cb.state == 1) { // Open
        if (now - cb.last_failure_time > 60000) { // 1 minute timeout
            cb.state = 2; // Half-open
            return 1;
        }
        return 0;
    } else { // Half-open
        return 1;
    }
}

void circuit_breaker_failure() {
    cb.failure_count++;
    cb.last_failure_time = yq_current_timestamp_ms();
    
    if (cb.failure_count >= cb.failure_threshold) {
        cb.state = 1; // Open
        printf("Circuit breaker opened!\n");
    }
}

void circuit_breaker_success() {
    cb.failure_count = 0;
    cb.state = 0; // Closed
    printf("Circuit breaker closed\n");
}
```

### Health Check System

```c
// Health check for pub/sub system
void health_check_loop() {
    while (1) {
        yq_pubsub_stats stats;
        if (yq_pubsub_stats(pubsub, &stats) == YQ_OK) {
            // Check health metrics
            if (stats.failed_messages > stats.total_messages * 0.1) {
                printf("WARNING: High failure rate: %lu/%lu\n", 
                       stats.failed_messages, stats.total_messages);
                trigger_alert();
            }
            
            if (stats.queue_usage > 90) {
                printf("WARNING: High queue usage: %u%%\n", stats.queue_usage);
                scale_up_resources();
            }
        }
        
        sleep(30); // Check every 30 seconds
    }
}
```

## Security Considerations

### Message Authentication

```c
// Message authentication with HMAC
#include <openssl/hmac.h>

typedef struct message_auth {
    uint8_t hmac[32]; // SHA-256 HMAC
} message_auth;

int authenticate_message(const yq_message *msg, const char *secret_key) {
    // Calculate HMAC of message payload
    unsigned char hmac[EVP_MAX_MD_SIZE];
    unsigned int hmac_len;
    
    HMAC(EVP_sha256(), secret_key, strlen(secret_key), 
         msg->payload, msg->payload_len, hmac, &hmac_len);
    
    // Compare with stored HMAC
    message_auth *auth = (message_auth *)msg->metadata;
    return memcmp(hmac, auth->hmac, 32) == 0;
}
```

### Access Control

```c
// Topic-based access control
typedef struct topic_permission {
    char topic[64];
    int can_publish;
    int can_subscribe;
} topic_permission;

static topic_permission permissions[] = {
    {"public.*", 1, 1},
    {"internal.*", 1, 0},
    {"admin.*", 0, 1},
    {NULL, 0, 0} // Terminator
};

int check_permission(const char *topic, int is_publisher) {
    for (int i = 0; permissions[i].topic != NULL; i++) {
        if (strncmp(topic, permissions[i].topic, strlen(permissions[i].topic)) == 0) {
            if (is_publisher) {
                return permissions[i].can_publish;
            } else {
                return permissions[i].can_subscribe;
            }
        }
    }
    return 0; // Default deny
}
```

### Secure Message Handling

```c
// Secure message handling with validation
int validate_message(const yq_message *msg) {
    // Check message size limits
    if (msg->payload_len > 1024 * 1024) { // 1MB limit
        return YQ_ERR_INVAL;
    }
    
    // Check topic name validity
    if (msg->topic_len == 0 || msg->topic_len > 64) {
        return YQ_ERR_INVAL;
    }
    
    // Check for malicious content
    if (contains_injection(msg->payload, msg->payload_len)) {
        return YQ_ERR_INVAL;
    }
    
    return YQ_OK;
}

int contains_injection(const void *data, size_t len) {
    // Simple injection detection
    const char *patterns[] = {
        "SELECT", "INSERT", "UPDATE", "DELETE",
        "DROP", "CREATE", "ALTER", "EXEC",
        "<script>", "javascript:", "vbscript:"
    };
    
    const char *str = (const char *)data;
    for (int i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
        if (strstr(str, patterns[i])) {
            return 1;
        }
    }
    
    return 0;
}
```

## Conclusion

These advanced features demonstrate the power and flexibility of yq-DB Pub/Sub. By implementing these patterns, you can build robust, scalable, and reliable messaging systems for your applications.

Remember to:
- Choose the right delivery mode for your use case
- Implement proper error handling and recovery
- Monitor performance and resource usage
- Consider security implications
- Test thoroughly in production-like environments

For more information, refer to the main [API Reference](PUBSUB.md) and [Quick Start Guide](PUBSUB_QUICKSTART.md).