# yq-DB Publish/Subscribe (Pub/Sub) Documentation

## Overview

The Publish/Subscribe (Pub/Sub) messaging system in yq-DB provides a powerful, lightweight messaging infrastructure that enables event-driven architectures within and across applications. This feature allows components to communicate asynchronously through topics and messages.

## Features

- **Topic-based messaging**: Messages are published to topics and delivered to subscribers
- **Message filtering**: Support for prefix and pattern-based filtering
- **Multiple delivery modes**: At-most-once, at-least-once, and exactly-once delivery
- **Message priority**: Support for urgent, high, normal, and low priority messages
- **Message persistence**: Optional message persistence across database restarts
- **Thread-safe operations**: Fully thread-safe implementation with proper synchronization
- **Message queuing**: Per-subscriber message queues with configurable size
- **Retry mechanisms**: Configurable retry logic for failed message delivery
- **Statistics and monitoring**: Comprehensive statistics and debugging capabilities

## Compilation

To enable Pub/Sub functionality, compile with the `YQ_ENABLE_PUBSUB` flag:

```bash
cmake -DYQ_ENABLE_PUBSUB=ON ..
make
```

Or compile with:
```bash
gcc -DYQ_ENABLE_PUBSUB=1 -Iinclude -c src/yq_pubsub.c
```

## API Reference

### Initialization and Management

#### `yq_pubsub_init`
```c
int yq_pubsub_init(yq_db *db, yq_pubsub **out);
```
Initialize the Pub/Sub system.

**Parameters:**
- `db`: Database instance
- `out`: Output parameter for Pub/Sub instance

**Returns:**
- `YQ_OK` on success
- `YQ_ERR_INVAL` if invalid parameters
- `YQ_ERR_NOMEM` if memory allocation fails

#### `yq_pubsub_close`
```c
int yq_pubsub_close(yq_pubsub *pubsub);
```
Close and cleanup the Pub/Sub system.

**Parameters:**
- `pubsub`: Pub/Sub instance

**Returns:**
- `YQ_OK` on success

#### `yq_pubsub_stats`
```c
int yq_pubsub_stats(yq_pubsub *pubsub, yq_pubsub_stats *stats);
```
Get Pub/Sub system statistics.

**Parameters:**
- `pubsub`: Pub/Sub instance
- `stats`: Output parameter for statistics

**Returns:**
- `YQ_OK` on success
- `YQ_ERR_INVAL` if invalid parameters

### Topic Management

#### `yq_topic_create`
```c
int yq_topic_create(yq_pubsub *pubsub, const char *name, yq_topic **out);
```
Create a new topic.

**Parameters:**
- `pubsub`: Pub/Sub instance
- `name`: Topic name
- `out`: Output parameter for topic instance

**Returns:**
- `YQ_OK` on success
- `YQ_ERR_INVAL` if invalid parameters
- `YQ_ERR_EXISTS` if topic already exists
- `YQ_ERR_NOMEM` if memory allocation fails

#### `yq_topic_delete`
```c
int yq_topic_delete(yq_pubsub *pubsub, const char *name);
```
Delete a topic.

**Parameters:**
- `pubsub`: Pub/Sub instance
- `name`: Topic name

**Returns:**
- `YQ_OK` on success
- `YQ_ERR_INVAL` if invalid parameters
- `YQ_ERR_NOTFOUND` if topic doesn't exist

#### `yq_topic_get`
```c
int yq_topic_get(yq_pubsub *pubsub, const char *name, yq_topic **out);
```
Get an existing topic.

**Parameters:**
- `pubsub`: Pub/Sub instance
- `name`: Topic name
- `out`: Output parameter for topic instance

**Returns:**
- `YQ_OK` on success
- `YQ_ERR_INVAL` if invalid parameters
- `YQ_ERR_NOTFOUND` if topic doesn't exist

#### `yq_topic_list`
```c
int yq_topic_list(yq_pubsub *pubsub, char ***topics, uint32_t *count);
```
List all topics.

**Parameters:**
- `pubsub`: Pub/Sub instance
- `topics`: Output parameter for topic names array
- `count`: Output parameter for number of topics

**Returns:**
- `YQ_OK` on success
- `YQ_ERR_INVAL` if invalid parameters

#### `yq_topic_stats`
```c
int yq_topic_stats(yq_topic *topic, yq_topic_stats *stats);
```
Get topic statistics.

**Parameters:**
- `topic`: Topic instance
- `stats`: Output parameter for statistics

**Returns:**
- `YQ_OK` on success
- `YQ_ERR_INVAL` if invalid parameters

### Message Publishing

#### `yq_publish`
```c
int yq_publish(yq_pubsub *pubsub, const char *topic, const void *payload, size_t payload_len, 
               uint32_t flags, uint64_t *message_id);
```
Publish a message to a topic.

**Parameters:**
- `pubsub`: Pub/Sub instance
- `topic`: Topic name
- `payload`: Message payload data
- `payload_len`: Length of payload data
- `flags`: Message flags
- `message_id`: Output parameter for message ID

**Returns:**
- `YQ_OK` on success
- `YQ_ERR_INVAL` if invalid parameters

#### `yq_publish_ex`
```c
int yq_publish_ex(yq_pubsub *pubsub, const char *topic, const void *payload, size_t payload_len,
                  const void *metadata, size_t metadata_len, uint32_t flags, 
                  uint32_t priority, uint64_t *message_id);
```
Publish an extended message with metadata and priority.

**Parameters:**
- `pubsub`: Pub/Sub instance
- `topic`: Topic name
- `payload`: Message payload data
- `payload_len`: Length of payload data
- `metadata`: Message metadata (optional)
- `metadata_len`: Length of metadata (0 if none)
- `flags`: Message flags
- `priority`: Message priority
- `message_id`: Output parameter for message ID

**Returns:**
- `YQ_OK` on success
- `YQ_ERR_INVAL` if invalid parameters

#### `yq_publish_batch`
```c
int yq_publish_batch(yq_pubsub *pubsub, const yq_message **messages, size_t count, 
                      uint64_t *message_ids);
```
Publish multiple messages in a batch.

**Parameters:**
- `pubsub`: Pub/Sub instance
- `messages`: Array of messages
- `count`: Number of messages
- `message_ids`: Output parameter for message IDs

**Returns:**
- `YQ_OK` on success
- `YQ_ERR_INVAL` if invalid parameters

### Subscription Management

#### `yq_subscribe`
```c
int yq_subscribe(yq_pubsub *pubsub, const char *topic, const yq_subscriber_opts *opts,
                 yq_message_callback message_cb, yq_error_callback error_cb,
                 void *user_data, yq_subscriber **out);
```
Subscribe to a topic.

**Parameters:**
- `pubsub`: Pub/Sub instance
- `topic`: Topic name to subscribe to
- `opts`: Subscriber options (optional, uses defaults if NULL)
- `message_cb`: Message callback function
- `error_cb`: Error callback function
- `user_data`: User data to pass to callbacks
- `out`: Output parameter for subscriber instance

**Returns:**
- `YQ_OK` on success
- `YQ_ERR_INVAL` if invalid parameters
- `YQ_ERR_DUPLICATE` if already subscribed
- `YQ_ERR_NOMEM` if memory allocation fails

#### `yq_subscribe_filtered`
```c
int yq_subscribe_filtered(yq_pubsub *pubsub, const yq_topic_filter *filter,
                          const yq_subscriber_opts *opts,
                          yq_message_callback message_cb, yq_error_callback error_cb,
                          void *user_data, yq_subscriber **out);
```
Subscribe to a topic with filtering.

**Parameters:**
- `pubsub`: Pub/Sub instance
- `filter`: Topic filter
- `opts`: Subscriber options (optional, uses defaults if NULL)
- `message_cb`: Message callback function
- `error_cb`: Error callback function
- `user_data`: User data to pass to callbacks
- `out`: Output parameter for subscriber instance

**Returns:**
- `YQ_OK` on success
- `YQ_ERR_INVAL` if invalid parameters
- `YQ_ERR_DUPLICATE` if already subscribed
- `YQ_ERR_NOMEM` if memory allocation fails

#### `yq_unsubscribe`
```c
int yq_unsubscribe(yq_subscriber *sub);
```
Unsubscribe from a topic.

**Parameters:**
- `sub`: Subscriber instance

**Returns:**
- `YQ_OK` on success
- `YQ_ERR_INVAL` if invalid parameters

#### `yq_unsubscribe_all`
```c
int yq_unsubscribe_all(yq_pubsub *pubsub);
```
Unsubscribe from all topics.

**Parameters:**
- `pubsub`: Pub/Sub instance

**Returns:**
- `YQ_OK` on success

### Message Queue Operations

#### `yq_message_queue_get`
```c
int yq_message_queue_get(yq_subscriber *sub, yq_message_queue **out);
```
Get the message queue for a subscriber.

**Parameters:**
- `sub`: Subscriber instance
- `out`: Output parameter for message queue

**Returns:**
- `YQ_OK` on success
- `YQ_ERR_INVAL` if invalid parameters

#### `yq_message_queue_poll`
```c
int yq_message_queue_poll(yq_message_queue *queue, yq_message **out, uint32_t timeout_ms);
```
Poll a message from the queue.

**Parameters:**
- `queue`: Message queue
- `out`: Output parameter for message
- `timeout_ms`: Timeout in milliseconds (0 for no timeout)

**Returns:**
- `YQ_OK` on success
- `YQ_ERR_INVAL` if invalid parameters
- `YQ_ERR_NOTFOUND` if no message available

#### `yq_message_queue_poll_batch`
```c
int yq_message_queue_poll_batch(yq_message_queue *queue, yq_message **messages, 
                                 size_t max_count, uint32_t timeout_ms, size_t *actual_count);
```
Poll multiple messages from the queue.

**Parameters:**
- `queue`: Message queue
- `messages`: Output parameter for messages array
- `max_count`: Maximum number of messages to poll
- `timeout_ms`: Timeout in milliseconds
- `actual_count`: Output parameter for actual number of messages polled

**Returns:**
- `YQ_OK` on success
- `YQ_ERR_INVAL` if invalid parameters

#### `yq_message_ack`
```c
int yq_message_ack(yq_message_queue *queue, const yq_message *msg);
```
Acknowledge a message as processed.

**Parameters:**
- `queue`: Message queue
- `msg`: Message to acknowledge

**Returns:**
- `YQ_OK` on success
- `YQ_ERR_INVAL` if invalid parameters

#### `yq_message_nack`
```c
int yq_message_nack(yq_message_queue *queue, const yq_message *msg, uint32_t retry_delay_ms);
```
Reject a message for re-delivery.

**Parameters:**
- `queue`: Message queue
- `msg`: Message to reject
- `retry_delay_ms`: Delay before retrying

**Returns:**
- `YQ_OK` on success
- `YQ_ERR_INVAL` if invalid parameters

### Message Operations

#### `yq_message_create`
```c
int yq_message_create(uint64_t message_id, const char *topic, const void *payload, size_t payload_len,
                      const void *metadata, size_t metadata_len, uint32_t flags,
                      uint32_t priority, yq_message **out);
```
Create a message object.

**Parameters:**
- `message_id`: Message ID
- `topic`: Topic name
- `payload`: Message payload
- `payload_len`: Payload length
- `metadata`: Message metadata
- `metadata_len`: Metadata length
- `flags`: Message flags
- `priority`: Message priority
- `out`: Output parameter for message

**Returns:**
- `YQ_OK` on success
- `YQ_ERR_INVAL` if invalid parameters
- `YQ_ERR_NOMEM` if memory allocation fails

#### `yq_message_free`
```c
void yq_message_free(yq_message *msg);
```
Free a message object.

**Parameters:**
- `msg`: Message to free

#### `yq_message_get_payload`
```c
int yq_message_get_payload(const yq_message *msg, void **payload, size_t *payload_len);
```
Get a copy of the message payload.

**Parameters:**
- `msg`: Message
- `payload`: Output parameter for payload data
- `payload_len`: Output parameter for payload length

**Returns:**
- `YQ_OK` on success
- `YQ_ERR_INVAL` if invalid parameters
- `YQ_ERR_NOMEM` if memory allocation fails

#### `yq_message_get_metadata`
```c
int yq_message_get_metadata(const yq_message *msg, void **metadata, size_t *metadata_len);
```
Get a copy of the message metadata.

**Parameters:**
- `msg`: Message
- `metadata`: Output parameter for metadata
- `metadata_len`: Output parameter for metadata length

**Returns:**
- `YQ_OK` on success
- `YQ_ERR_INVAL` if invalid parameters
- `YQ_ERR_NOMEM` if memory allocation fails

### Utility Functions

#### `yq_pubsub_set_debug`
```c
int yq_pubsub_set_debug(yq_pubsub *pubsub, int enabled);
```
Enable or disable debug mode.

**Parameters:**
- `pubsub`: Pub/Sub instance
- `enabled`: 1 to enable, 0 to disable

**Returns:**
- `YQ_OK` on success
- `YQ_ERR_INVAL` if invalid parameters

#### `yq_pubsub_get_debug_info`
```c
int yq_pubsub_get_debug_info(yq_pubsub *pubsub, char **debug_info);
```
Get debug information.

**Parameters:**
- `pubsub`: Pub/Sub instance
- `debug_info`: Output parameter for debug information

**Returns:**
- `YQ_OK` on success
- `YQ_ERR_INVAL` if invalid parameters
- `YQ_ERR_NOMEM` if memory allocation fails

#### `yq_pubsub_cleanup_expired`
```c
int yq_pubsub_cleanup_expired(yq_pubsub *pubsub, uint64_t max_age_ms);
```
Clean up expired messages.

**Parameters:**
- `pubsub`: Pub/Sub instance
- `max_age_ms`: Maximum age of messages to keep in milliseconds

**Returns:**
- `YQ_OK` on success
- `YQ_ERR_INVAL` if invalid parameters

## Data Types

### `yq_message_priority`
```c
typedef enum yq_message_priority {
    YQ_MESSAGE_LOW    = 0,  /* Low priority, processed last */
    YQ_MESSAGE_NORMAL = 1,  /* Normal priority, default */
    YQ_MESSAGE_HIGH   = 2,  /* High priority, processed early */
    YQ_MESSAGE_URGENT = 3   /* Urgent priority, processed immediately */
} yq_message_priority;
```

### `yq_message_flags`
```c
#define YQ_MESSAGE_FLAG_PERSISTENT  0x0001u /* Persistent message */
#define YQ_MESSAGE_FLAG_BROADCAST   0x0002u /* Broadcast message */
#define YQ_MESSAGE_FLAG_FILTERED     0x0004u /* Filtered message */
#define YQ_MESSAGE_FLAG_COMPRESSED   0x0008u /* Compressed message */
#define YQ_MESSAGE_FLAG_ENCRYPTED    0x0010u /* Encrypted message */
```

### `yq_delivery_mode`
```c
typedef enum yq_delivery_mode {
    YQ_DELIVERY_AT_MOST_ONCE = 0, /* At most once delivery */
    YQ_DELIVERY_AT_LEAST_ONCE = 1, /* At least once delivery */
    YQ_DELIVERY_EXACTLY_ONCE = 2  /* Exactly once delivery */
} yq_delivery_mode;
```

### `yq_filter_type`
```c
typedef enum yq_filter_type {
    YQ_FILTER_NONE = 0,       /* No filtering */
    YQ_FILTER_PREFIX = 1,     /* Prefix filtering */
    YQ_FILTER_REGEX = 2,      /* Regular expression filtering */
    YQ_FILTER_CUSTOM = 3      /* Custom filtering */
} yq_filter_type;
```

### `yq_topic_filter`
```c
typedef struct yq_topic_filter {
    uint32_t struct_size;      /* Must be sizeof(yq_topic_filter) */
    yq_filter_type type;       /* Filter type */
    uint32_t pattern_len;      /* Pattern length */
    uint32_t reserved[7];      /* Must be 0 */
    const char *pattern;       /* Filter pattern */
} yq_topic_filter;
```

### `yq_subscriber_opts`
```c
typedef struct yq_subscriber_opts {
    uint32_t struct_size;      /* Must be sizeof(yq_subscriber_opts) */
    uint32_t queue_size;       /* Message queue size */
    uint32_t max_retry;        /* Maximum retry count */
    uint32_t retry_delay_ms;   /* Retry delay in milliseconds */
    uint32_t timeout_ms;       /* Message processing timeout */
    yq_delivery_mode delivery_mode; /* Delivery mode */
    uint32_t reserved[8];      /* Must be 0 */
} yq_subscriber_opts;
```

### `yq_message`
```c
typedef struct yq_message {
    uint32_t struct_size;      /* Must be sizeof(yq_message) */
    uint64_t message_id;      /* Message ID */
    uint64_t timestamp;       /* Message timestamp */
    uint32_t priority;        /* Message priority */
    uint32_t flags;           /* Message flags */
    uint32_t topic_len;       /* Topic string length */
    uint32_t payload_len;     /* Payload length */
    uint32_t metadata_len;    /* Metadata length */
    uint32_t reserved[4];      /* Must be 0 */
    const char *topic;        /* Topic string */
    const void *payload;      /* Message payload */
    const void *metadata;     /* Message metadata */
} yq_message;
```

### `yq_pubsub_stats`
```c
typedef struct yq_pubsub_stats {
    uint32_t struct_size;      /* Must be sizeof(yq_pubsub_stats) */
    uint64_t total_messages;   /* Total messages published */
    uint64_t delivered_messages; /* Messages successfully delivered */
    uint64_t failed_messages;  /* Failed deliveries */
    uint32_t active_topics;    /* Number of active topics */
    uint32_t active_subscribers; /* Number of active subscribers */
    uint32_t queue_usage;     /* Queue usage percentage */
    uint32_t max_queue_size;   /* Maximum queue size */
    uint64_t total_bytes;      /* Total bytes transferred */
    uint32_t reserved[8];      /* Must be 0 */
} yq_pubsub_stats;
```

### `yq_topic_stats`
```c
typedef struct yq_topic_stats {
    uint32_t struct_size;      /* Must be sizeof(yq_topic_stats) */
    uint64_t message_count;   /* Number of messages in topic */
    uint64_t subscriber_count; /* Number of subscribers */
    uint64_t total_bytes;     /* Total bytes in topic */
    uint32_t message_rate;     /* Messages per second */
    uint32_t reserved[8];      /* Must be 0 */
} yq_topic_stats;
```

### `yq_queue_stats`
```c
typedef struct yq_queue_stats {
    uint32_t struct_size;      /* Must be sizeof(yq_queue_stats) */
    uint32_t queue_size;      /* Queue capacity */
    uint32_t queue_usage;     /* Current queue usage */
    uint32_t pending_count;   /* Pending messages count */
    uint32_t processed_count; /* Processed messages count */
    uint32_t failed_count;    /* Failed messages count */
    uint32_t avg_process_ms; /* Average processing time */
    uint32_t reserved[8];      /* Must be 0 */
} yq_queue_stats;
```

## Callback Functions

### `yq_message_callback`
```c
typedef int (*yq_message_callback)(yq_subscriber *sub, const yq_message *msg, void *user_data);
```
Callback function for message processing.

**Parameters:**
- `sub`: Subscriber instance
- `msg`: Message to process
- `user_data`: User data

**Returns:**
- 0 on success
- Non-zero on failure (will trigger retry or error callback)

### `yq_error_callback`
```c
typedef int (*yq_error_callback)(yq_subscriber *sub, int error_code, const char *error_msg, void *user_data);
```
Callback function for error handling.

**Parameters:**
- `sub`: Subscriber instance
- `error_code`: Error code
- `error_msg`: Error message
- `user_data`: User data

**Returns:**
- 0 on success
- Non-zero on critical error

### `yq_stats_callback`
```c
typedef void (*yq_stats_callback)(yq_pubsub *pubsub, void *user_data);
```
Callback function for statistics updates.

**Parameters:**
- `pubsub`: Pub/Sub instance
- `user_data`: User data

## Usage Examples

### Basic Publisher
```c
#include "yq.h"
#include "yq_pubsub.h"

int main() {
    yq_db *db;
    yq_pubsub *pubsub;
    yq_opts opts = {sizeof(yq_opts), 0};
    
    // Open database
    yq_open("test.db", &opts, &db);
    
    // Initialize pub/sub
    yq_pubsub_init(db, &pubsub);
    
    // Publish a message
    uint64_t message_id;
    const char *topic = "events";
    const char *payload = "Hello, World!";
    yq_publish(pubsub, topic, payload, strlen(payload), 0, &message_id);
    
    printf("Published message %lu to topic %s\n", message_id, topic);
    
    // Cleanup
    yq_pubsub_close(pubsub);
    yq_close(db);
    
    return 0;
}
```

### Basic Subscriber
```c
#include "yq.h"
#include "yq_pubsub.h"

static void message_callback(yq_subscriber *sub, const yq_message *msg, void *user_data) {
    printf("Received message %lu: %.*s\n", msg->message_id, (int)msg->payload_len, (const char *)msg->payload);
}

static void error_callback(yq_subscriber *sub, int error_code, const char *error_msg, void *user_data) {
    printf("Error: %s\n", error_msg);
}

int main() {
    yq_db *db;
    yq_pubsub *pubsub;
    yq_subscriber *sub;
    yq_opts opts = {sizeof(yq_opts), 0};
    
    // Open database
    yq_open("test.db", &opts, &db);
    
    // Initialize pub/sub
    yq_pubsub_init(db, &pubsub);
    
    // Subscribe to topic
    yq_subscribe(pubsub, "events", NULL, message_callback, error_callback, NULL, &sub);
    
    printf("Subscribed to events topic\n");
    
    // Keep running
    while (1) {
        // Process messages...
        sleep(1);
    }
    
    // Cleanup
    yq_unsubscribe(sub);
    yq_pubsub_close(pubsub);
    yq_close(db);
    
    return 0;
}
```

### Advanced Usage with Filtering
```c
#include "yq.h"
#include "yq_pubsub.h"

int main() {
    yq_db *db;
    yq_pubsub *pubsub;
    yq_subscriber *sub;
    yq_opts opts = {sizeof(yq_opts), 0};
    
    // Open database
    yq_open("test.db", &opts, &db);
    
    // Initialize pub/sub
    yq_pubsub_init(db, &pubsub);
    
    // Create subscriber with custom options
    yq_subscriber_opts sub_opts = {sizeof(yq_subscriber_opts), 1000, 5, 2000, 10000, YQ_DELIVERY_AT_LEAST_ONCE};
    
    // Subscribe with prefix filtering
    yq_topic_filter filter = {sizeof(yq_topic_filter), YQ_FILTER_PREFIX, 0, {0}, "user."};
    yq_subscribe_filtered(pubsub, &filter, &sub_opts, message_callback, error_callback, NULL, &sub);
    
    // Publish multiple messages
    for (int i = 0; i < 10; i++) {
        char topic[32];
        char payload[64];
        snprintf(topic, sizeof(topic), "user.%d.events", i);
        snprintf(payload, sizeof(payload), "Message %d for user %d", i, i);
        
        uint64_t message_id;
        yq_publish(pubsub, topic, payload, strlen(payload), 0, &message_id);
    }
    
    printf("Published 10 messages\n");
    
    // Get statistics
    yq_pubsub_stats stats;
    if (yq_pubsub_stats(pubsub, &stats) == YQ_OK) {
        printf("Statistics: %lu total messages, %lu delivered\n", 
               stats.total_messages, stats.delivered_messages);
    }
    
    // Cleanup
    yq_unsubscribe(sub);
    yq_pubsub_close(pubsub);
    yq_close(db);
    
    return 0;
}
```

## Performance Considerations

1. **Message Size**: Messages should be kept reasonably small for better performance
2. **Queue Size**: Configure appropriate queue sizes based on expected message volume
3. **Timeout Values**: Set appropriate timeouts to balance responsiveness and resource usage
4. **Memory Usage**: Monitor memory usage, especially with large numbers of messages
5. **Thread Safety**: The implementation is fully thread-safe, but consider contention in high-throughput scenarios

## Error Handling

The Pub/Sub system uses the yq-DB error code system:

- `YQ_OK`: Success
- `YQ_ERR_INVAL`: Invalid parameters
- `YQ_ERR_NOMEM`: Memory allocation failed
- `YQ_ERR_NOTFOUND`: Topic or subscriber not found
- `YQ_ERR_DUPLICATE`: Duplicate subscription
- `YQ_ERR_TIMEOUT`: Operation timeout
- `YQ_ERR_PANIC`: Internal error

## Debugging

Enable debug mode for detailed logging:

```c
yq_pubsub_set_debug(pubsub, 1);
```

Get debug information:

```c
char *debug_info;
if (yq_pubsub_get_debug_info(pubsub, &debug_info) == YQ_OK) {
    printf("Debug info:\n%s\n", debug_info);
    free(debug_info);
}
```

## Thread Safety

The Pub/Sub system is fully thread-safe:

- All public functions are thread-safe
- Multiple threads can publish and subscribe concurrently
- Each subscriber runs in its own thread
- Proper synchronization is used for all shared data structures

## Limitations

1. **Message Size**: Limited by available memory
2. **Topic Names**: Limited by system string length limits
3. **Subscriber Count**: Limited by available system resources
4. **Queue Size**: Limited by available memory
5. **Message Retention**: Messages are retained in memory unless marked as persistent

## Future Enhancements

Potential future enhancements:

- Message persistence to disk
- Advanced filtering and routing
- Message compression and encryption
- Cluster support
- Advanced monitoring and metrics
- Plugin architecture for custom functionality

## License

This feature is part of yq-DB and follows the same license terms.