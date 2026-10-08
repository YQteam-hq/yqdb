# yq-DB Pub/Sub Quick Start Guide

## Introduction

This guide will help you get started with the Publish/Subscribe (Pub/Sub) messaging system in yq-DB. You'll learn how to set up, publish, and subscribe to messages in minutes.

## Prerequisites

- yq-DB library with Pub/Sub support enabled
- Basic understanding of C programming
- C compiler with pthread support

## Building with Pub/Sub Support

```bash
# Configure with Pub/Sub support
cmake -DYQ_ENABLE_PUBSUB=ON ..

# Build the library
make
```

## Basic Setup

### 1. Initialize the Database

```c
#include "yq.h"
#include "yq_pubsub.h"

int main() {
    yq_db *db;
    yq_pubsub *pubsub;
    yq_opts opts = {sizeof(yq_opts), 0};
    
    // Open database
    yq_open("myapp.db", &opts, &db);
    
    // Initialize Pub/Sub system
    yq_pubsub_init(db, &pubsub);
    
    // Your pub/sub code here...
    
    // Cleanup
    yq_pubsub_close(pubsub);
    yq_close(db);
    
    return 0;
}
```

### 2. Publish Your First Message

```c
// Create a simple publisher
void simple_publisher() {
    yq_db *db;
    yq_pubsub *pubsub;
    yq_opts opts = {sizeof(yq_opts), 0};
    
    yq_open("publisher.db", &opts, &db);
    yq_pubsub_init(db, &pubsub);
    
    // Publish a message
    const char *topic = "notifications";
    const char *message = "Hello from publisher!";
    uint64_t message_id;
    
    int result = yq_publish(pubsub, topic, message, strlen(message), 0, &message_id);
    
    if (result == YQ_OK) {
        printf("Published message %lu to topic '%s'\n", message_id, topic);
    } else {
        printf("Failed to publish message: %d\n", result);
    }
    
    yq_pubsub_close(pubsub);
    yq_close(db);
}
```

### 3. Subscribe to Messages

```c
// Message callback function
static void message_handler(yq_subscriber *sub, const yq_message *msg, void *user_data) {
    printf("Received message %lu: %.*s\n", 
           msg->message_id, 
           (int)msg->payload_len, 
           (const char *)msg->payload);
}

// Error callback function
static void error_handler(yq_subscriber *sub, int error_code, const char *error_msg, void *user_data) {
    printf("Error: %s (code: %d)\n", error_msg, error_code);
}

void simple_subscriber() {
    yq_db *db;
    yq_pubsub *pubsub;
    yq_subscriber *sub;
    yq_opts opts = {sizeof(yq_opts), 0};
    
    yq_open("subscriber.db", &opts, &db);
    yq_pubsub_init(db, &pubsub);
    
    // Subscribe to topic
    int result = yq_subscribe(pubsub, "notifications", NULL, 
                              message_handler, error_handler, NULL, &sub);
    
    if (result == YQ_OK) {
        printf("Subscribed to 'notifications' topic\n");
        
        // Keep the subscriber running
        while (1) {
            sleep(1); // In real app, use proper event loop
        }
    } else {
        printf("Failed to subscribe: %d\n", result);
    }
    
    yq_unsubscribe(sub);
    yq_pubsub_close(pubsub);
    yq_close(db);
}
```

## Complete Example

Here's a complete example with both publisher and subscriber:

```c
#include "yq.h"
#include "yq_pubsub.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Global flag to keep the program running
static volatile int running = 1;

// Message handler
static void message_handler(yq_subscriber *sub, const yq_message *msg, void *user_data) {
    printf("[%lu] %.*s: %.*s\n", 
           msg->message_id,
           (int)msg->topic_len,
           (const char *)msg->topic,
           (int)msg->payload_len,
           (const char *)msg->payload);
}

// Error handler
static void error_handler(yq_subscriber *sub, int error_code, const char *error_msg, void *user_data) {
    printf("Error: %s (code: %d)\n", error_msg, error_code);
}

// Signal handler for graceful shutdown
static void signal_handler(int sig) {
    running = 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <publisher|subscriber>\n", argv[0]);
        return 1;
    }
    
    // Set up signal handling
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    yq_db *db;
    yq_pubsub *pubsub;
    yq_opts opts = {sizeof(yq_opts), 0};
    
    // Open database
    yq_open("example.db", &opts, &db);
    yq_pubsub_init(db, &pubsub);
    
    if (strcmp(argv[1], "publisher") == 0) {
        // Publisher mode
        printf("Publisher started. Press Ctrl+C to stop.\n");
        
        while (running) {
            // Publish a test message
            static int counter = 0;
            char topic[32];
            char message[64];
            
            snprintf(topic, sizeof(topic), "test.%d", counter % 3);
            snprintf(message, sizeof(message), "Test message %d", counter++);
            
            uint64_t message_id;
            int result = yq_publish(pubsub, topic, message, strlen(message), 0, &message_id);
            
            if (result == YQ_OK) {
                printf("Published: %s -> %.*s (ID: %lu)\n", 
                       topic, (int)strlen(message), message, message_id);
            } else {
                printf("Failed to publish: %d\n", result);
            }
            
            sleep(1);
        }
        
    } else if (strcmp(argv[1], "subscriber") == 0) {
        // Subscriber mode
        printf("Subscriber started. Press Ctrl+C to stop.\n");
        
        // Subscribe to all test topics
        yq_subscriber *sub;
        int result = yq_subscribe(pubsub, "test.", NULL, 
                                 message_handler, error_handler, NULL, &sub);
        
        if (result == YQ_OK) {
            while (running) {
                sleep(1); // Wait for messages
            }
        } else {
            printf("Failed to subscribe: %d\n", result);
        }
        
        yq_unsubscribe(sub);
    } else {
        printf("Invalid mode. Use 'publisher' or 'subscriber'\n");
    }
    
    // Cleanup
    yq_pubsub_close(pubsub);
    yq_close(db);
    
    printf("Shutdown complete.\n");
    return 0;
}
```

## Building the Example

```bash
# Compile the example
gcc -DYQ_ENABLE_PUBSUB=1 -Iinclude -o pubsub_example pubsub_example.c -lpthread

# Run in two terminals:
# Terminal 1 (Publisher):
./pubsub_example publisher

# Terminal 2 (Subscriber):
./pubsub_example subscriber
```

## Advanced Features

### 1. Message Priorities

```c
// Publish with high priority
yq_publish_ex(pubsub, "urgent", "Important message!", strlen("Important message!"),
              NULL, 0, 0, YQ_MESSAGE_HIGH, &message_id);
```

### 2. Topic Filtering

```c
// Subscribe with prefix filtering
yq_topic_filter filter = {sizeof(yq_topic_filter), YQ_FILTER_PREFIX, 0, {0}, "user."};
yq_subscribe_filtered(pubsub, &filter, NULL, message_handler, error_handler, NULL, &sub);
```

### 3. Batch Publishing

```c
// Publish multiple messages at once
yq_message *messages[3];
uint64_t message_ids[3];

// Create messages
yq_message_create(1, "topic1", "msg1", 4, NULL, 0, 0, YQ_MESSAGE_NORMAL, &messages[0]);
yq_message_create(2, "topic2", "msg2", 4, NULL, 0, 0, YQ_MESSAGE_NORMAL, &messages[1]);
yq_message_create(3, "topic3", "msg3", 4, NULL, 0, 0, YQ_MESSAGE_NORMAL, &messages[2]);

// Publish batch
yq_publish_batch(pubsub, messages, 3, message_ids);
```

### 4. Message Queues

```c
// Get message queue
yq_message_queue *queue;
yq_message_queue_get(sub, &queue);

// Poll for messages
yq_message *msg;
yq_message_queue_poll(queue, &msg, 1000); // 1 second timeout

if (msg) {
    // Process message
    printf("Processing message: %.*s\n", (int)msg->payload_len, (const char *)msg->payload);
    
    // Acknowledge message
    yq_message_ack(queue, msg);
    yq_message_free(msg);
}
```

## Monitoring and Statistics

```c
// Get system statistics
yq_pubsub_stats stats;
if (yq_pubsub_stats(pubsub, &stats) == YQ_OK) {
    printf("Total messages: %lu\n", stats.total_messages);
    printf("Delivered messages: %lu\n", stats.delivered_messages);
    printf("Failed messages: %lu\n", stats.failed_messages);
    printf("Active topics: %u\n", stats.active_topics);
    printf("Active subscribers: %u\n", stats.active_subscribers);
}
```

## Best Practices

1. **Error Handling**: Always check return values from Pub/Sub functions
2. **Memory Management**: Free messages when done using `yq_message_free()`
3. **Resource Cleanup**: Always unsubscribe and close Pub/Sub when done
4. **Thread Safety**: Pub/Sub is thread-safe, but be mindful of callback execution
5. **Queue Management**: Monitor queue usage to prevent memory issues
6. **Timeout Handling**: Use appropriate timeouts for message polling

## Common Issues

1. **Compilation Errors**: Ensure `DYQ_ENABLE_PUBSUB=1` is set
2. **Linking Errors**: Link with `-lpthread` for thread support
3. **Memory Leaks**: Always free messages and clean up resources
4. **Deadlocks**: Use appropriate timeouts in callbacks

## Next Steps

- Read the full [API Reference](PUBSUB.md)
- Explore [Advanced Features](PUBSUB_ADVANCED.md)
- Check out [Performance Tips](PUBSUB_PERFORMANCE.md)

## Getting Help

For more information, check the main yq-DB documentation or create an issue on the GitHub repository.