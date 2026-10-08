# yq-DB Pub/Sub Performance Optimization Guide

## Table of Contents

1. [Performance Overview](#performance-overview)
2. [Benchmarking Methodology](#benchmarking-methodology)
3. [Memory Management](#memory-management)
4. [CPU Optimization](#cpu-optimization)
5. [I/O Optimization](#io-optimization)
6. [Concurrency Tuning](#concurrency-tuning)
7. [Network Considerations](#network-considerations)
8. [Monitoring and Metrics](#monitoring-and-metrics)
9. [Troubleshooting Performance Issues](#troubleshooting-performance-issues)
10. [Best Practices](#best-practices)

## Performance Overview

### Performance Characteristics

yq-DB Pub/Sub is designed for high-performance messaging with the following characteristics:

- **Low Latency**: Message delivery in microseconds under optimal conditions
- **High Throughput**: Capable of handling thousands of messages per second
- **Scalable**: Performance scales linearly with available resources
- **Memory Efficient**: Optimized memory usage with minimal overhead

### Key Performance Metrics

| Metric | Target | Monitoring Method |
|--------|--------|------------------|
| Message Latency | < 1ms | `yq_pubsub_stats()` |
| Throughput | > 1000 msg/s | Custom timing tests |
| Memory Usage | < 1GB per 1000 msg | `yq_pubsub_get_debug_info()` |
| CPU Usage | < 50% under load | System monitoring |
| Queue Fill Rate | < 80% | `yq_queue_stats()` |

## Benchmarking Methodology

### Test Environment Setup

```c
// Benchmark configuration
typedef struct benchmark_config {
    int message_count;
    int message_size;
    int subscriber_count;
    int publisher_count;
    int duration_seconds;
    int queue_size;
    int delivery_mode;
} benchmark_config;

// Simple benchmark runner
void run_benchmark(benchmark_config *config) {
    yq_db *db;
    yq_pubsub *pubsub;
    yq_opts opts = {sizeof(yq_opts), 0};
    
    yq_open("benchmark.db", &opts, &db);
    yq_pubsub_init(db, &pubsub);
    
    // Set up publishers and subscribers
    setup_publishers(pubsub, config);
    setup_subscribers(pubsub, config);
    
    // Run benchmark
    uint64_t start_time = yq_current_timestamp_ms();
    uint64_t end_time = start_time + (config->duration_seconds * 1000);
    
    uint64_t total_messages = 0;
    uint64_t start_messages = get_total_message_count(pubsub);
    
    while (yq_current_timestamp_ms() < end_time) {
        usleep(100000); // 100ms intervals
        total_messages = get_total_message_count(pubsub) - start_messages;
        
        printf("Throughput: %lu msg/s\n", total_messages / config->duration_seconds);
    }
    
    // Cleanup
    cleanup_publishers();
    cleanup_subscribers();
    yq_pubsub_close(pubsub);
    yq_close(db);
}
```

### Performance Test Suite

```c
// Comprehensive performance tests
void run_performance_tests() {
    test_latency();
    test_throughput();
    test_memory_usage();
    test_cpu_usage();
    test_queue_performance();
    test_concurrency();
}

void test_latency() {
    printf("=== LATENCY TEST ===\n");
    
    yq_db *db;
    yq_pubsub *pubsub;
    yq_opts opts = {sizeof(yq_opts), 0};
    
    yq_open("latency_test.db", &opts, &db);
    yq_pubsub_init(db, &pubsub);
    
    // Set up single subscriber
    yq_subscriber *sub;
    yq_subscribe(pubsub, "latency_test", NULL, 
                 latency_callback, error_callback, NULL, &sub);
    
    // Measure latency for different message sizes
    int sizes[] = {16, 64, 256, 1024, 4096, 16384};
    
    for (int i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        char payload[sizes[i]];
        memset(payload, 'A', sizes[i]);
        
        uint64_t start_time = yq_current_timestamp_ms();
        uint64_t message_id;
        
        yq_publish(pubsub, "latency_test", payload, sizes[i], 0, &message_id);
        
        // Wait for message to be processed
        while (!message_received) {
            usleep(100);
        }
        
        uint64_t end_time = yq_current_timestamp_ms();
        uint64_t latency = end_time - start_time;
        
        printf("Message size %d bytes: %lu ms latency\n", sizes[i], latency);
        message_received = 0;
    }
    
    yq_unsubscribe(sub);
    yq_pubsub_close(pubsub);
    yq_close(db);
    printf("===================\n");
}
```

## Memory Management

### Memory Allocation Strategies

```c
// Memory pool for frequent allocations
typedef struct message_pool {
    yq_message *pool;
    size_t pool_size;
    size_t used;
    pthread_mutex_t mutex;
} message_pool;

static message_pool msg_pool = {0};

int init_message_pool(size_t size) {
    msg_pool.pool = malloc(size * sizeof(yq_message));
    if (!msg_pool.pool) return YQ_ERR_NOMEM;
    
    msg_pool.pool_size = size;
    msg_pool.used = 0;
    pthread_mutex_init(&msg_pool.mutex, NULL);
    
    return YQ_OK;
}

yq_message *alloc_from_pool() {
    pthread_mutex_lock(&msg_pool.mutex);
    
    if (msg_pool.used >= msg_pool.pool_size) {
        pthread_mutex_unlock(&msg_pool.mutex);
        return NULL;
    }
    
    yq_message *msg = &msg_pool.pool[msg_pool.used++];
    pthread_mutex_unlock(&msg_pool.mutex);
    
    return msg;
}

void free_to_pool(yq_message *msg) {
    pthread_mutex_lock(&msg_pool.mutex);
    
    // Find and free the message
    for (size_t i = 0; i < msg_pool.used; i++) {
        if (&msg_pool.pool[i] == msg) {
            msg_pool.used--;
            // Move last element to this position
            if (i < msg_pool.used) {
                msg_pool.pool[i] = msg_pool.pool[msg_pool.used];
            }
            break;
        }
    }
    
    pthread_mutex_unlock(&msg_pool.mutex);
}
```

### Memory Usage Monitoring

```c
// Memory usage tracking
typedef struct memory_stats {
    size_t total_allocated;
    size_t total_freed;
    size_t peak_usage;
    size_t current_usage;
    size_t allocation_count;
    size_t free_count;
} memory_stats;

static memory_stats mem_stats = {0};

void *tracked_malloc(size_t size) {
    void *ptr = malloc(size);
    if (ptr) {
        pthread_mutex_lock(&mem_stats_mutex);
        mem_stats.total_allocated += size;
        mem_stats.current_usage += size;
        mem_stats.allocation_count++;
        
        if (mem_stats.current_usage > mem_stats.peak_usage) {
            mem_stats.peak_usage = mem_stats.current_usage;
        }
        pthread_mutex_unlock(&mem_stats_mutex);
    }
    return ptr;
}

void tracked_free(void *ptr, size_t size) {
    if (ptr) {
        pthread_mutex_lock(&mem_stats_mutex);
        mem_stats.total_freed += size;
        mem_stats.current_usage -= size;
        mem_stats.free_count++;
        pthread_mutex_unlock(&mem_stats_mutex);
        
        free(ptr);
    }
}

void print_memory_stats() {
    printf("=== MEMORY STATS ===\n");
    printf("Total Allocated: %zu bytes\n", mem_stats.total_allocated);
    printf("Total Freed: %zu bytes\n", mem_stats.total_freed);
    printf("Peak Usage: %zu bytes\n", mem_stats.peak_usage);
    printf("Current Usage: %zu bytes\n", mem_stats.current_usage);
    printf("Allocation Count: %zu\n", mem_stats.allocation_count);
    printf("Free Count: %zu\n", mem_stats.free_count);
    printf("Efficiency: %.2f%%\n", 
           mem_stats.total_allocated > 0 ? 
           (double)mem_stats.total_freed / mem_stats.total_allocated * 100 : 0);
    printf("===================\n");
}
```

### Memory Optimization Techniques

```c
// Zero-copy message handling
int zero_copy_publish(yq_pubsub *pubsub, const char *topic, 
                     const void *payload, size_t payload_len) {
    // Create message that references external buffer
    yq_message *msg = alloc_from_pool();
    if (!msg) return YQ_ERR_NOMEM;
    
    msg->struct_size = sizeof(yq_message);
    msg->message_id = generate_message_id();
    msg->timestamp = yq_current_timestamp_ms();
    msg->priority = YQ_MESSAGE_NORMAL;
    msg->flags = 0;
    msg->topic_len = strlen(topic);
    msg->payload_len = payload_len;
    msg->metadata_len = 0;
    msg->topic = topic;  // Reference external string
    msg->payload = payload;  // Reference external buffer
    msg->metadata = NULL;
    
    // Publish the message
    uint64_t published_id;
    int result = yq_publish_internal(pubsub, msg, &published_id);
    
    if (result != YQ_OK) {
        free_to_pool(msg);
    }
    
    return result;
}

// Pre-allocated buffers for common message sizes
typedef struct buffer_pool {
    void *buffers[10];  // Different buffer sizes
    size_t sizes[10];   // Corresponding sizes
    int counts[10];     // Available counts
    pthread_mutex_t mutex;
} buffer_pool;

static buffer_pool buf_pool = {0};

void init_buffer_pool() {
    // Pre-allocate common buffer sizes
    int sizes[] = {64, 256, 1024, 4096, 16384, 65536, 262144, 1048576};
    
    for (int i = 0; i < 8; i++) {
        buf_pool.buffers[i] = malloc(sizes[i] * 10);  // 10 buffers each
        buf_pool.sizes[i] = sizes[i];
        buf_pool.counts[i] = 10;
    }
    
    pthread_mutex_init(&buf_pool.mutex, NULL);
}

void *get_buffer(size_t size) {
    pthread_mutex_lock(&buf_pool.mutex);
    
    // Find appropriate buffer size
    for (int i = 0; i < 8; i++) {
        if (buf_pool.sizes[i] >= size && buf_pool.counts[i] > 0) {
            buf_pool.counts[i]--;
            pthread_mutex_unlock(&buf_pool.mutex);
            return buf_pool.buffers[i] + (buf_pool.sizes[i] * (10 - buf_pool.counts[i] - 1));
        }
    }
    
    pthread_mutex_unlock(&buf_pool.mutex);
    return NULL;
}

void return_buffer(void *buffer, size_t size) {
    pthread_mutex_lock(&buf_pool.mutex);
    
    // Find and return buffer
    for (int i = 0; i < 8; i++) {
        if (buf_pool.sizes[i] >= size) {
            buf_pool.counts[i]++;
            pthread_mutex_unlock(&buf_pool.mutex);
            return;
        }
    }
    
    pthread_mutex_unlock(&buf_pool.mutex);
}
```

## CPU Optimization

### Lock-Free Data Structures

```c
// Lock-free queue for high-performance message delivery
typedef struct lf_node {
    yq_message *message;
    volatile struct lf_node *next;
    volatile int processed;
} lf_node;

typedef struct lock_free_queue {
    volatile lf_node *head;
    volatile lf_node *tail;
    volatile size_t size;
    size_t max_size;
    pthread_mutex_t resize_mutex;
} lock_free_queue;

int lf_queue_push(lock_free_queue *queue, yq_message *msg) {
    lf_node *node = malloc(sizeof(lf_node));
    if (!node) return YQ_ERR_NOMEM;
    
    node->message = msg;
    node->next = NULL;
    node->processed = 0;
    
    // Atomic push operation
    lf_node *old_tail;
    lf_node *old_next;
    
    do {
        old_tail = (lf_node *)queue->tail;
        old_next = (lf_node *)old_tail->next;
        
        if (old_next != NULL) {
            // Tail is not pointing to the last node, help it move forward
            __sync_bool_compare_and_swap(&queue->tail, old_tail, old_next);
            continue;
        }
        
        // Try to add the new node
    } while (!__sync_bool_compare_and_swap(&old_tail->next, old_next, node));
    
    // Advance the tail
    __sync_bool_compare_and_swap(&queue->tail, old_tail, node);
    
    // Update size
    __sync_add_and_fetch(&queue->size, 1);
    
    return YQ_OK;
}

yq_message *lf_queue_pop(lock_free_queue *queue) {
    lf_node *old_head;
    lf_node *old_next;
    
    do {
        old_head = (lf_node *)queue->head;
        lf_node *tail = (lf_node *)queue->tail;
        
        if (old_head == tail) {
            // Queue is empty
            return NULL;
        }
        
        old_next = (lf_node *)old_head->next;
        
        if (old_next == NULL) {
            // Queue is empty
            return NULL;
        }
        
        // Try to move head to next node
    } while (!__sync_bool_compare_and_swap(&queue->head, old_head, old_next));
    
    // Update size
    __sync_sub_and_fetch(&queue->size, 1);
    
    yq_message *msg = old_next->message;
    free(old_head);
    
    return msg;
}
```

### CPU Cache Optimization

```c
// Cache-friendly message structure
typedef struct cache_aligned_message {
    uint32_t struct_size;      // 4 bytes
    uint32_t priority;          // 4 bytes
    uint32_t flags;            // 4 bytes
    uint32_t topic_len;        // 4 bytes
    uint32_t payload_len;      // 4 bytes
    uint32_t metadata_len;     // 4 bytes
    uint32_t reserved[4];      // 16 bytes
    uint64_t message_id;       // 8 bytes
    uint64_t timestamp;        // 8 bytes
    
    // Align to cache line (64 bytes)
    uint8_t padding[16];       // 16 bytes
    
    // Frequently accessed fields together
    const char *topic;         // 8 bytes
    const void *payload;       // 8 bytes
    const void *metadata;      // 8 bytes
    
    // Cache line boundary
    uint8_t cache_line[40];   // 40 bytes to fill cache line
} __attribute__((aligned(64))) cache_aligned_message;

// Batch processing for better cache utilization
void process_message_batch(yq_message **messages, size_t count) {
    // Prefetch data for better cache performance
    for (size_t i = 0; i < count; i++) {
        __builtin_prefetch(messages[i], 0, 3);  // Prefetch data
    }
    
    // Process messages in batches
    for (size_t i = 0; i < count; i++) {
        // Process message
        process_single_message(messages[i]);
        
        // Prefetch next message
        if (i + 1 < count) {
            __builtin_prefetch(messages[i + 1], 0, 3);
        }
    }
}
```

### CPU Affinity and Scheduling

```c
// CPU affinity for performance-critical threads
void set_cpu_affinity(pthread_t thread, int cpu_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);
    pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
}

// Real-time scheduling for high-priority message processing
void setup_realtime_priority(pthread_t thread) {
    struct sched_param param;
    param.sched_priority = 10;  // High priority
    
    pthread_setschedparam(thread, SCHED_RR, &param);
}

// Thread pool for message processing
typedef struct thread_pool {
    pthread_t *threads;
    int thread_count;
    volatile int running;
    lock_free_queue *work_queue;
    pthread_mutex_t queue_mutex;
    pthread_cond_t queue_cond;
} thread_pool;

void *thread_worker(void *arg) {
    thread_pool *pool = (thread_pool *)arg;
    
    // Set CPU affinity for this thread
    set_cpu_affinity(pthread_self(), 1);  // CPU 1
    
    // Set real-time priority
    setup_realtime_priority(pthread_self());
    
    while (pool->running) {
        yq_message *msg = lf_queue_pop(pool->work_queue);
        
        if (msg) {
            // Process the message
            process_message(msg);
            yq_message_free(msg);
        } else {
            // Wait for work
            pthread_mutex_lock(&pool->queue_mutex);
            pthread_cond_wait(&pool->queue_cond, &pool->queue_mutex);
            pthread_mutex_unlock(&pool->queue_mutex);
        }
    }
    
    return NULL;
}

int init_thread_pool(thread_pool *pool, int thread_count) {
    pool->threads = malloc(thread_count * sizeof(pthread_t));
    pool->thread_count = thread_count;
    pool->running = 1;
    pool->work_queue = create_lock_free_queue();
    
    pthread_mutex_init(&pool->queue_mutex, NULL);
    pthread_cond_init(&pool->queue_cond, NULL);
    
    for (int i = 0; i < thread_count; i++) {
        pthread_create(&pool->threads[i], NULL, thread_worker, pool);
    }
    
    return YQ_OK;
}
```

## I/O Optimization

### Asynchronous I/O

```c
// Non-blocking message publishing
int async_publish(yq_pubsub *pubsub, const char *topic, 
                 const void *payload, size_t payload_len) {
    // Create message
    yq_message *msg = create_message(topic, payload, payload_len);
    if (!msg) return YQ_ERR_NOMEM;
    
    // Add to async queue
    if (!async_queue_push(msg)) {
        yq_message_free(msg);
        return YQ_ERR_NOMEM;
    }
    
    // Signal I/O thread
    pthread_cond_signal(&async_io_cond);
    
    return YQ_OK;
}

// I/O thread for non-blocking operations
void *io_thread(void *arg) {
    while (1) {
        pthread_mutex_lock(&async_io_mutex);
        pthread_cond_wait(&async_io_cond, &async_io_mutex);
        pthread_mutex_unlock(&async_io_mutex);
        
        // Process async queue
        yq_message *msg;
        while ((msg = async_queue_pop()) != NULL) {
            // Publish message
            uint64_t message_id;
            yq_publish(pubsub, msg->topic, msg->payload, msg->payload_len, 
                       msg->flags, &message_id);
            
            yq_message_free(msg);
        }
    }
    
    return NULL;
}
```

### Batch I/O Operations

```c
// Batch message publishing for better I/O performance
int batch_publish_messages(yq_pubsub *pubsub, yq_message **messages, size_t count) {
    // Batch messages for efficient I/O
    size_t total_size = 0;
    for (size_t i = 0; i < count; i++) {
        total_size += messages[i]->topic_len + messages[i]->payload_len + messages[i]->metadata_len;
    }
    
    // Allocate batch buffer
    void *batch_buffer = malloc(total_size);
    if (!batch_buffer) return YQ_ERR_NOMEM;
    
    // Pack messages into batch
    char *current = batch_buffer;
    for (size_t i = 0; i < count; i++) {
        // Copy topic
        memcpy(current, messages[i]->topic, messages[i]->topic_len);
        current += messages[i]->topic_len;
        
        // Copy payload
        memcpy(current, messages[i]->payload, messages[i]->payload_len);
        current += messages[i]->payload_len;
        
        // Copy metadata
        if (messages[i]->metadata_len > 0) {
            memcpy(current, messages[i]->metadata, messages[i]->metadata_len);
            current += messages[i]->metadata_len;
        }
    }
    
    // Publish batch (implementation depends on pubsub capabilities)
    uint64_t *message_ids = malloc(count * sizeof(uint64_t));
    int result = yq_publish_batch(pubsub, messages, count, message_ids);
    
    // Cleanup
    free(batch_buffer);
    free(message_ids);
    
    return result;
}
```

## Concurrency Tuning

### Thread Pool Optimization

```c
// Dynamic thread pool sizing
typedef struct dynamic_thread_pool {
    pthread_t *threads;
    int thread_count;
    int optimal_threads;
    volatile int active_threads;
    volatile int queue_size;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} dynamic_thread_pool;

void adjust_thread_pool(dynamic_thread_pool *pool) {
    pthread_mutex_lock(&pool->mutex);
    
    // Calculate optimal thread count based on queue size
    int queue_threshold = 100;
    int max_threads = 8;
    
    if (pool->queue_size > queue_threshold && pool->thread_count < max_threads) {
        // Add more threads
        int new_threads = min(2, max_threads - pool->thread_count);
        
        for (int i = 0; i < new_threads; i++) {
            pthread_create(&pool->threads[pool->thread_count], NULL, 
                         thread_worker, pool);
            pool->thread_count++;
        }
    } else if (pool->queue_size < queue_threshold / 2 && pool->thread_count > 2) {
        // Remove threads (graceful shutdown)
        pool->thread_count--;
    }
    
    pthread_mutex_unlock(&pool->mutex);
}

// Work stealing for load balancing
typedef struct work_stealing_pool {
    pthread_t *threads;
    int thread_count;
    lock_free_queue **queues;
    pthread_mutex_t *mutexes;
} work_stealing_pool;

yq_message *steal_work(work_stealing_pool *pool, int thief_id) {
    // Try to steal work from other queues
    for (int i = 0; i < pool->thread_count; i++) {
        if (i != thief_id) {
            yq_message *msg = lf_queue_pop(pool->queues[i]);
            if (msg) {
                return msg;
            }
        }
    }
    
    return NULL;
}
```

### Lock-Free Statistics

```c
// Lock-free performance counters
typedef struct lock_free_stats {
    volatile uint64_t messages_processed;
    volatile uint64_t messages_failed;
    volatile uint64_t total_bytes;
    volatile uint64_t start_time;
    volatile uint64_t last_update;
} lock_free_stats;

// Atomic operations for performance counters
void increment_messages_processed(lock_free_stats *stats) {
    __sync_add_and_fetch(&stats->messages_processed, 1);
}

void increment_messages_failed(lock_free_stats *stats) {
    __sync_add_and_fetch(&stats->messages_failed, 1);
}

void add_bytes_processed(lock_free_stats *stats, uint64_t bytes) {
    __sync_add_and_fetch(&stats->total_bytes, bytes);
}

// Get current statistics
void get_performance_stats(lock_free_stats *stats, 
                          uint64_t *messages_processed,
                          uint64_t *messages_failed,
                          uint64_t *total_bytes,
                          double *messages_per_second) {
    *messages_processed = stats->messages_processed;
    *messages_failed = stats->messages_failed;
    *total_bytes = stats->total_bytes;
    
    uint64_t now = yq_current_timestamp_ms();
    uint64_t elapsed = now - stats->start_time;
    
    if (elapsed > 0) {
        *messages_per_second = (double)*messages_processed / (elapsed / 1000.0);
    } else {
        *messages_per_second = 0.0;
    }
}
```

## Network Considerations

### Network Buffer Management

```c
// Network-optimized message handling
typedef struct network_buffer {
    char *buffer;
    size_t size;
    size_t capacity;
    size_t head;
    size_t tail;
    pthread_mutex_t mutex;
} network_buffer;

int network_buffer_write(network_buffer *nb, const void *data, size_t len) {
    pthread_mutex_lock(&nb->mutex);
    
    // Check if we have enough space
    if (nb->tail + len > nb->capacity) {
        // Need to resize
        size_t new_capacity = nb->capacity * 2;
        while (new_capacity < nb->tail + len) {
            new_capacity *= 2;
        }
        
        char *new_buffer = realloc(nb->buffer, new_capacity);
        if (!new_buffer) {
            pthread_mutex_unlock(&nb->mutex);
            return YQ_ERR_NOMEM;
        }
        
        nb->buffer = new_buffer;
        nb->capacity = new_capacity;
    }
    
    // Copy data
    memcpy(nb->buffer + nb->tail, data, len);
    nb->tail += len;
    
    pthread_mutex_unlock(&nb->mutex);
    return YQ_OK;
}

int network_buffer_read(network_buffer *nb, void *data, size_t len) {
    pthread_mutex_lock(&nb->mutex);
    
    if (nb->head + len > nb->tail) {
        pthread_mutex_unlock(&nb->mutex);
        return YQ_ERR_NOTFOUND;
    }
    
    memcpy(data, nb->buffer + nb->head, len);
    nb->head += len;
    
    // If buffer is empty, reset it
    if (nb->head >= nb->tail) {
        nb->head = 0;
        nb->tail = 0;
    }
    
    pthread_mutex_unlock(&nb->mutex);
    return YQ_OK;
}
```

### Network Compression

```c
// Network compression for large messages
int compress_for_network(const void *data, size_t data_len, 
                        void **compressed_data, size_t *compressed_len) {
    // Use fast compression for network transmission
    yq_compressor *compressor;
    yq_compressor_init(YQ_COMPRESSOR_LZ4, &compressor);
    
    *compressed_data = yq_compress(compressor, data, data_len, compressed_len);
    yq_compressor_close(compressor);
    
    return *compressed_data ? YQ_OK : YQ_ERR_NOMEM;
}

int decompress_from_network(const void *compressed_data, size_t compressed_len,
                           void **data, size_t *data_len) {
    yq_compressor *compressor;
    yq_compressor_init(YQ_COMPRESSOR_LZ4, &compressor);
    
    *data = yq_decompress(compressor, compressed_data, compressed_len, data_len);
    yq_compressor_close(compressor);
    
    return *data ? YQ_OK : YQ_ERR_NOMEM;
}
```

## Monitoring and Metrics

### Real-time Performance Monitoring

```c
// Real-time performance monitoring system
typedef struct performance_monitor {
    uint64_t last_update;
    uint64_t messages_processed;
    uint64_t messages_failed;
    uint64_t total_bytes;
    double avg_latency;
    double max_latency;
    double min_latency;
    uint64_t latency_samples;
    pthread_mutex_t mutex;
} performance_monitor;

void update_performance_metrics(performance_monitor *monitor, 
                              uint64_t messages_processed,
                              uint64_t messages_failed,
                              uint64_t total_bytes,
                              double latency) {
    pthread_mutex_lock(&monitor->mutex);
    
    uint64_t now = yq_current_timestamp_ms();
    uint64_t elapsed = now - monitor->last_update;
    
    if (elapsed > 0) {
        // Update averages
        monitor->avg_latency = (monitor->avg_latency * monitor->latency_samples + latency) / 
                               (monitor->latency_samples + 1);
        monitor->latency_samples++;
        
        // Update max/min
        if (latency > monitor->max_latency) {
            monitor->max_latency = latency;
        }
        if (latency < monitor->min_latency || monitor->min_latency == 0) {
            monitor->min_latency = latency;
        }
        
        // Update totals
        monitor->messages_processed = messages_processed;
        monitor->messages_failed = messages_failed;
        monitor->total_bytes = total_bytes;
        monitor->last_update = now;
    }
    
    pthread_mutex_unlock(&monitor->mutex);
}

void print_performance_report(performance_monitor *monitor) {
    pthread_mutex_lock(&monitor->mutex);
    
    printf("=== PERFORMANCE REPORT ===\n");
    printf("Messages Processed: %lu\n", monitor->messages_processed);
    printf("Messages Failed: %lu\n", monitor->messages_failed);
    printf("Success Rate: %.2f%%\n", 
           monitor->messages_processed > 0 ? 
           (double)(monitor->messages_processed - monitor->messages_failed) / 
           monitor->messages_processed * 100 : 0);
    printf("Total Bytes: %lu\n", monitor->total_bytes);
    printf("Average Latency: %.2f ms\n", monitor->avg_latency);
    printf("Max Latency: %.2f ms\n", monitor->max_latency);
    printf("Min Latency: %.2f ms\n", monitor->min_latency);
    printf("==========================\n");
    
    pthread_mutex_unlock(&monitor->mutex);
}
```

### Performance Profiling

```c
// Performance profiling with timing
typedef struct profile_entry {
    const char *function_name;
    uint64_t call_count;
    uint64_t total_time;
    uint64_t min_time;
    uint64_t max_time;
} profile_entry;

typedef struct profiler {
    profile_entry *entries;
    int entry_count;
    pthread_mutex_t mutex;
} profiler;

void profile_start(profiler *prof, const char *function_name) {
    uint64_t start_time = yq_current_timestamp_ms();
    
    pthread_mutex_lock(&prof->mutex);
    
    // Find or create entry
    profile_entry *entry = NULL;
    for (int i = 0; i < prof->entry_count; i++) {
        if (strcmp(prof->entries[i].function_name, function_name) == 0) {
            entry = &prof->entries[i];
            break;
        }
    }
    
    if (!entry) {
        // Add new entry
        prof->entries = realloc(prof->entries, (prof->entry_count + 1) * sizeof(profile_entry));
        entry = &prof->entries[prof->entry_count++];
        entry->function_name = function_name;
        entry->call_count = 0;
        entry->total_time = 0;
        entry->min_time = 0;
        entry->max_time = 0;
    }
    
    // Store start time in thread-local storage
    pthread_setspecific(prof_key, (void *)start_time);
    
    pthread_mutex_unlock(&prof->mutex);
}

void profile_end(profiler *prof, const char *function_name) {
    uint64_t end_time = yq_current_timestamp_ms();
    uint64_t start_time = (uint64_t)pthread_getspecific(prof_key);
    uint64_t duration = end_time - start_time;
    
    pthread_mutex_lock(&prof->mutex);
    
    // Find entry
    for (int i = 0; i < prof->entry_count; i++) {
        if (strcmp(prof->entries[i].function_name, function_name) == 0) {
            profile_entry *entry = &prof->entries[i];
            entry->call_count++;
            entry->total_time += duration;
            
            if (entry->min_time == 0 || duration < entry->min_time) {
                entry->min_time = duration;
            }
            if (duration > entry->max_time) {
                entry->max_time = duration;
            }
            break;
        }
    }
    
    pthread_mutex_unlock(&prof->mutex);
}

void print_profile_report(profiler *prof) {
    pthread_mutex_lock(&prof->mutex);
    
    printf("=== PROFILE REPORT ===\n");
    for (int i = 0; i < prof->entry_count; i++) {
        profile_entry *entry = &prof->entries[i];
        double avg_time = entry->call_count > 0 ? 
                          (double)entry->total_time / entry->call_count : 0;
        
        printf("%s:\n", entry->function_name);
        printf("  Calls: %lu\n", entry->call_count);
        printf("  Total Time: %lu ms\n", entry->total_time);
        printf("  Avg Time: %.2f ms\n", avg_time);
        printf("  Min Time: %lu ms\n", entry->min_time);
        printf("  Max Time: %lu ms\n", entry->max_time);
        printf("\n");
    }
    printf("======================\n");
    
    pthread_mutex_unlock(&prof->mutex);
}
```

## Troubleshooting Performance Issues

### Common Performance Bottlenecks

```c
// Performance bottleneck detection
typedef struct bottleneck_detector {
    uint64_t last_check;
    uint64_t message_count;
    double cpu_usage;
    double memory_usage;
    double queue_fill_rate;
    int bottleneck_detected;
} bottleneck_detector;

void detect_bottlenecks(bottleneck_detector *detector, 
                       yq_pubsub *pubsub,
                       double cpu_usage,
                       double memory_usage) {
    uint64_t now = yq_current_timestamp_ms();
    uint64_t elapsed = now - detector->last_check;
    
    if (elapsed < 10000) return; // Check every 10 seconds
    
    // Get pubsub statistics
    yq_pubsub_stats stats;
    if (yq_pubsub_stats(pubsub, &stats) == YQ_OK) {
        uint64_t current_messages = stats.total_messages;
        uint64_t message_rate = (current_messages - detector->message_count) / 
                               (elapsed / 1000.0);
        
        detector->message_count = current_messages;
        detector->cpu_usage = cpu_usage;
        detector->memory_usage = memory_usage;
        detector->queue_fill_rate = stats.queue_usage;
        
        // Detect bottlenecks
        detector->bottleneck_detected = 0;
        
        if (cpu_usage > 80.0) {
            printf("BOTTLENECK: High CPU usage (%.1f%%)\n", cpu_usage);
            detector->bottleneck_detected = 1;
        }
        
        if (memory_usage > 80.0) {
            printf("BOTTLENECK: High memory usage (%.1f%%)\n", memory_usage);
            detector->bottleneck_detected = 1;
        }
        
        if (stats.queue_usage > 90.0) {
            printf("BOTTLENECK: High queue usage (%u%%)\n", stats.queue_usage);
            detector->bottleneck_detected = 1;
        }
        
        if (message_rate < 100.0 && elapsed > 30000) {
            printf("BOTTLENECK: Low message rate (%.1f msg/s)\n", message_rate);
            detector->bottleneck_detected = 1;
        }
    }
    
    detector->last_check = now;
}
```

### Performance Analysis Tools

```c
// Performance analysis and recommendations
void analyze_performance(performance_monitor *monitor, 
                        bottleneck_detector *detector,
                        yq_pubsub *pubsub) {
    printf("=== PERFORMANCE ANALYSIS ===\n");
    
    // Check overall health
    if (detector->bottleneck_detected) {
        printf("WARNING: Performance bottlenecks detected!\n");
        
        // Provide recommendations
        if (detector->cpu_usage > 80.0) {
            printf("RECOMMENDATION: Increase CPU resources or optimize CPU-intensive operations\n");
            printf("  - Consider using lock-free data structures\n");
            printf("  - Optimize algorithms for better cache utilization\n");
            printf("  - Use thread pools for better load balancing\n");
        }
        
        if (detector->memory_usage > 80.0) {
            printf("RECOMMENDATION: Increase memory resources or optimize memory usage\n");
            printf("  - Use memory pools for frequent allocations\n");
            printf("  - Implement message compression for large payloads\n");
            printf("  - Consider message expiration policies\n");
        }
        
        if (detector->queue_fill_rate > 90.0) {
            printf("RECOMMENDATION: Increase queue capacity or improve consumer performance\n");
            printf("  - Add more consumer threads\n");
            printf("  - Implement batch processing\n");
            printf("  - Consider message prioritization\n");
        }
    } else {
        printf("STATUS: Performance is within acceptable limits\n");
    }
    
    // Detailed analysis
    printf("\nDetailed Metrics:\n");
    printf("- Average Latency: %.2f ms\n", monitor->avg_latency);
    printf("- Message Throughput: %.1f msg/s\n", 
           monitor->messages_processed / (monitor->last_update / 1000.0));
    printf("- Success Rate: %.2f%%\n", 
           monitor->messages_processed > 0 ? 
           (double)(monitor->messages_processed - monitor->messages_failed) / 
           monitor->messages_processed * 100 : 0);
    
    printf("=============================\n");
}
```

## Best Practices

### Configuration Optimization

```c
// Optimal configuration based on workload
typedef struct workload_profile {
    enum {
        WORKLOAD_IO_BOUND,
        WORKLOAD_CPU_BOUND,
        WORKLOAD_BALANCED
    } type;
    
    int message_size;
    int throughput_requirement;
    int latency_requirement;
    int memory_constraint;
} workload_profile;

workload_profile analyze_workload(yq_pubsub *pubsub) {
    workload_profile profile = {0};
    
    // Analyze message size distribution
    yq_pubsub_stats stats;
    if (yq_pubsub_stats(pubsub, &stats) == YQ_OK) {
        // This is a simplified analysis - in practice, you'd collect more data
        profile.message_size = 1024; // Default
        profile.throughput_requirement = 1000;
        profile.latency_requirement = 10;
        profile.memory_constraint = 1024 * 1024 * 1024; // 1GB
        
        // Determine workload type
        if (stats.queue_usage > 70) {
            profile.type = WORKLOAD_IO_BOUND;
        } else if (stats.total_messages > 10000) {
            profile.type = WORKLOAD_CPU_BOUND;
        } else {
            profile.type = WORKLOAD_BALANCED;
        }
    }
    
    return profile;
}

void configure_optimal_settings(workload_profile *profile) {
    switch (profile->type) {
        case WORKLOAD_IO_BOUND:
            printf("Configuring for I/O-bound workload...\n");
            // Use larger queues, more threads, async I/O
            break;
        case WORKLOAD_CPU_BOUND:
            printf("Configuring for CPU-bound workload...\n");
            // Use fewer threads, lock-free structures, CPU optimization
            break;
        case WORKLOAD_BALANCED:
            printf("Configuring for balanced workload...\n");
            // Use balanced settings
            break;
    }
}
```

### Performance Testing Checklist

```c
// Comprehensive performance testing checklist
void performance_testing_checklist() {
    printf("=== PERFORMANCE TESTING CHECKLIST ===\n");
    
    printf("✓ Baseline Performance Testing\n");
    printf("  - Measure latency under normal load\n");
    printf("  - Measure throughput capacity\n");
    printf("  - Measure memory usage patterns\n");
    printf("  - Measure CPU utilization\n");
    
    printf("✓ Stress Testing\n");
    printf("  - Test with 10x normal load\n");
    printf("  - Test with maximum message sizes\n");
    printf("  - Test with high frequency bursts\n");
    printf("  - Test with sustained high load\n");
    
    printf("✓ Failure Testing\n");
    printf("  - Test with consumer failures\n");
    printf("  - Test with network interruptions\n");
    printf("  - Test with memory pressure\n");
    printf("  - Test with CPU saturation\n");
    
    printf("✓ Recovery Testing\n");
    printf("  - Test recovery after failures\n");
    printf("  - Test message integrity after restart\n");
    printf("  - Test queue recovery after crashes\n");
    printf("  - Test resource cleanup\n");
    
    printf("✓ Long-term Testing\n");
    printf("  - Test with 24+ hour continuous operation\n");
    printf("  - Test with memory leak detection\n");
    printf("  - Test with performance degradation\n");
    printf("  - Test with resource exhaustion\n");
    
    printf("======================================\n");
}
```

### Performance Optimization Summary

1. **Memory Management**: Use memory pools and zero-copy techniques
2. **CPU Optimization**: Use lock-free data structures and cache-friendly algorithms
3. **I/O Optimization**: Use asynchronous I/O and batch operations
4. **Concurrency**: Use thread pools and work stealing for load balancing
5. **Monitoring**: Implement comprehensive monitoring and alerting
6. **Testing**: Use comprehensive testing methodologies
7. **Configuration**: Optimize based on workload characteristics

By following these guidelines, you can achieve optimal performance from yq-DB Pub/Sub in your applications. Remember to test thoroughly in your specific environment and adjust configurations based on your actual workload.