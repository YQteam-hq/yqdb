# Time-To-Live (TTL) for yq-DB

[English](TTL.md) · [简体中文](TTL.zh-CN.md)

**Time-To-Live (TTL)** functionality allows automatic expiration of keys, making yq-DB suitable for caching, session management, and temporary data storage.

## Overview

TTL enables automatic cleanup of expired keys, reducing manual cleanup efforts and ensuring data freshness. Key features include:

- Automatic key expiration based on time-to-live settings
- Manual and automatic cleanup mechanisms
- Query functions for finding expiring and expired keys
- Memory usage monitoring and statistics
- Configurable cleanup intervals and batch sizes

## Features

| Feature | Description | Use Cases |
|---------|-------------|-----------|
| **Automatic Expiration** | Keys automatically expire after specified TTL | Session data, temporary caches |
| **Manual Cleanup** | On-demand cleanup of expired keys | Bulk operations, maintenance |
| **Query Functions** | Find keys that are expiring or already expired | Analytics, monitoring |
| **Statistics** | Monitor expired key count and memory usage | Performance tuning |
| **Flexible Configuration** | Configurable cleanup intervals and batch sizes | Different deployment scenarios |

## Quick Start

```c
#include "yq.h"
#include "yq_ttl.h"

int main() {
    // Initialize database
    yq_db *db = NULL;
    yq_opts opts = {sizeof(yq_opts), 0, 0, 0, 0, 0, 0, 0, 0, {0}};
    yq_open("my_database.yqdb", &opts, &db);
    
    // Configure TTL (automatic cleanup every 60 seconds, max 1000 keys per cleanup)
    yq_ttl_opts ttl_opts = {
        .struct_size = sizeof(yq_ttl_opts),
        .cleanup_interval = 60,
        .max_expired = 1000,
        .reserved = {0}
    };
    yq_ttl_configure(db, &ttl_opts);
    
    // Set TTL for a key (30 seconds)
    yq_txn *txn = NULL;
    yq_txn_begin(db, YQ_TXN_READWRITE, &txn);
    
    yq_slice key = {"session_123", 11};
    yq_slice value = {"user_data", 9};
    yq_put(txn, key, value, YQ_PUT_UPSERT);
    
    // Set TTL for 30 seconds
    yq_ttl_set(txn, &key, 30);
    
    yq_txn_commit(txn);
    
    // Check remaining TTL
    int32_t remaining = yq_ttl_get(txn, &key);
    printf("Remaining TTL: %d seconds\n", remaining);
    
    // Manual cleanup
    yq_ttl_result *result = NULL;
    yq_ttl_cleanup(db, 0, &result);
    printf("Cleaned %d keys\n", result->cleaned_count);
    yq_ttl_result_free(result);
    
    yq_close(db);
    return 0;
}
```

## API Reference

### Configuration

#### `yq_ttl_configure()`
Configure TTL settings.

```c
int yq_ttl_configure(yq_db *db, const yq_ttl_opts *opts);
```

**Parameters:**
- `db`: Database handle
- `opts`: TTL configuration options

**Returns:** `YQ_OK` on success, error code on failure

**Configuration Options:**
- `cleanup_interval`: Automatic cleanup interval in seconds (0 = manual cleanup)
- `max_expired`: Maximum number of keys to clean per interval (0 = unlimited)

#### `yq_ttl_get_config()`
Get current TTL configuration.

```c
int yq_ttl_get_config(yq_db *db, yq_ttl_opts *opts);
```

**Parameters:**
- `db`: Database handle
- `opts`: Output parameter for configuration

**Returns:** `YQ_OK` on success, error code on failure

### TTL Operations

#### `yq_ttl_set()`
Set TTL for a key.

```c
int yq_ttl_set(yq_txn *txn, const yq_slice *key, uint32_t ttl);
```

**Parameters:**
- `txn`: Transaction handle
- `key`: Key to set TTL for
- `ttl`: Time-to-live in seconds

**Returns:** `YQ_OK` on success, error code on failure

#### `yq_ttl_get()`
Get remaining TTL for a key.

```c
int32_t yq_ttl_get(yq_txn *txn, const yq_slice *key);
```

**Parameters:**
- `txn`: Transaction handle
- `key`: Key to get TTL for

**Returns:**
- `> 0`: Remaining seconds
- `-1`: Key never expires (TTL not set)
- `-2`: Key does not exist or is already expired

#### `yq_ttl_unset()`
Remove TTL from a key.

```c
int yq_ttl_unset(yq_txn *txn, const yq_slice *key);
```

**Parameters:**
- `txn`: Transaction handle
- `key`: Key to remove TTL from

**Returns:** `YQ_OK` on success, error code on failure

### Cleanup Operations

#### `yq_ttl_cleanup()`
Manually clean up expired keys.

```c
int yq_ttl_cleanup(yq_db *db, uint32_t max_count, yq_ttl_result **result);
```

**Parameters:**
- `db`: Database handle
- `max_count`: Maximum number of keys to clean (0 = unlimited)
- `result`: Output parameter for cleanup results

**Returns:** `YQ_OK` on success, error code on failure

#### `yq_ttl_result_free()`
Free cleanup results.

```c
void yq_ttl_result_free(yq_ttl_result *result);
```

**Parameters:**
- `result`: Cleanup results to free

### Statistics

#### `yq_ttl_expired_count()`
Get count of expired keys.

```c
int64_t yq_ttl_expired_count(yq_db *db);
```

**Parameters:**
- `db`: Database handle

**Returns:** Number of expired keys, or `-1` on error

#### `yq_ttl_memory_usage()`
Get memory usage of TTL system.

```c
int64_t yq_ttl_memory_usage(yq_db *db);
```

**Parameters:**
- `db`: Database handle

**Returns:** Memory usage in bytes, or `-1` on error

### Query Functions

#### `yq_ttl_find_expiring()`
Find keys that are about to expire.

```c
yq_slice **yq_ttl_find_expiring(yq_txn *txn, uint32_t max_ttl, int *count);
```

**Parameters:**
- `txn`: Transaction handle
- `max_ttl`: Maximum remaining time in seconds
- `count`: Output parameter for number of keys found

**Returns:** Array of keys, caller must free with `yq_ttl_query_result_free()`

#### `yq_ttl_find_expired()`
Find keys that have already expired.

```c
yq_slice **yq_ttl_find_expired(yq_txn *txn, int *count);
```

**Parameters:**
- `txn`: Transaction handle
- `count`: Output parameter for number of keys found

**Returns:** Array of keys, caller must free with `yq_ttl_query_result_free()`

#### `yq_ttl_query_result_free()`
Free query results.

```c
void yq_ttl_query_result_free(yq_slice **keys, int count);
```

**Parameters:**
- `keys`: Array of keys to free
- `count`: Number of keys in array

### Utility Functions

#### `yq_ttl_now()`
Get current timestamp.

```c
time_t yq_ttl_now(void);
```

**Returns:** Current timestamp in seconds since epoch

#### `yq_ttl_format_time()`
Format timestamp as string.

```c
char *yq_ttl_format_time(time_t timestamp);
```

**Parameters:**
- `timestamp`: Timestamp to format

**Returns:** Formatted time string, caller must free

#### `yq_ttl_parse_time()`
Parse time string to timestamp.

```c
time_t yq_ttl_parse_time(const char *time_str);
```

**Parameters:**
- `time_str`: Time string to parse

**Returns:** Timestamp, or `-1` on error

## Configuration Examples

### Manual Cleanup Mode
```c
// Configure for manual cleanup only
yq_ttl_opts opts = {
    .struct_size = sizeof(yq_ttl_opts),
    .cleanup_interval = 0,  // No automatic cleanup
    .max_expired = 0,       // Unlimited cleanup size
    .reserved = {0}
};
yq_ttl_configure(db, &opts);

// Manual cleanup when needed
yq_ttl_result *result = NULL;
yq_ttl_cleanup(db, 1000, &result);  // Clean up to 1000 keys
printf("Cleaned %d keys\n", result->cleaned_count);
yq_ttl_result_free(result);
```

### Automatic Cleanup Mode
```c
// Configure for automatic cleanup every 30 seconds
yq_ttl_opts opts = {
    .struct_size = sizeof(yq_ttl_opts),
    .cleanup_interval = 30,  // Clean every 30 seconds
    .max_expired = 500,      // Clean up to 500 keys per interval
    .reserved = {0}
};
yq_ttl_configure(db, &opts);
```

### High-Frequency Cleanup
```c
// Configure for frequent cleanup with small batches
yq_ttl_opts opts = {
    .struct_size = sizeof(yq_ttl_opts),
    .cleanup_interval = 5,   // Clean every 5 seconds
    .max_expired = 100,      // Clean up to 100 keys per interval
    .reserved = {0}
};
yq_ttl_configure(db, &opts);
```

## Use Cases

### Session Management
```c
// Set session TTL
yq_slice session_key = {"user_123_session", 17};
yq_put(txn, session_key, session_data, YQ_PUT_UPSERT);
yq_ttl_set(txn, &session_key, 3600);  // 1 hour TTL

// Check if session is valid
int32_t remaining = yq_ttl_get(txn, &session_key);
if (remaining <= 0) {
    // Session expired
    yq_delete(txn, session_key);
} else {
    // Session still valid
}
```

### Caching System
```c
// Set cache TTL
yq_slice cache_key = {"api_response_123", 16};
yq_put(txn, cache_key, api_response, YQ_PUT_UPSERT);
yq_ttl_set(txn, &cache_key, 300);  // 5 minutes TTL

// Find and clean expired cache entries
yq_slice **expired_keys = NULL;
int expired_count = 0;
expired_keys = yq_ttl_find_expired(txn, &expired_count);

for (int i = 0; i < expired_count; i++) {
    yq_delete(txn, *expired_keys[i]);
    free(expired_keys[i]->data);
    free(expired_keys[i]);
}
free(expired_keys);
```

### Temporary Data Storage
```c
// Store temporary data with TTL
yq_slice temp_key = {"upload_temp_123", 16};
yq_slice temp_data = {upload_data, upload_size};
yq_put(txn, temp_key, temp_data, YQ_PUT_UPSERT);
yq_ttl_set(txn, &temp_key, 86400);  // 24 hours TTL

// Clean up all temporary data
yq_ttl_cleanup(db, 0, &result);
printf("Cleaned %d temporary files\n", result->cleaned_count);
yq_ttl_result_free(result);
```

### Monitoring and Analytics
```c
// Monitor TTL system
int64_t expired_count = yq_ttl_expired_count(db);
int64_t memory_usage = yq_ttl_memory_usage(db);
printf("Expired keys: %ld, Memory: %ld bytes\n", expired_count, memory_usage);

// Find keys expiring soon
yq_slice **expiring_keys = yq_ttl_find_expiring(txn, 60, &count);
if (count > 0) {
    printf("%d keys will expire in the next minute\n", count);
}
yq_ttl_query_result_free(expiring_keys, count);
```

## Performance Considerations

### Memory Usage
- Each TTL entry consumes additional memory for the key and timestamp
- Memory usage can be monitored with `yq_ttl_memory_usage()`
- Automatic cleanup helps prevent memory buildup

### Cleanup Performance
- Automatic cleanup runs on intervals to minimize impact
- Manual cleanup allows control over cleanup timing
- Batch processing reduces overhead

### Query Performance
- TTL queries scan the entire TTL index
- Use appropriate time ranges for better performance
- Cache query results when possible

## Error Handling

### Common Error Codes
- `YQ_ERR_INVAL`: Invalid parameters (NULL pointers, wrong size)
- `YQ_ERR_NOMEM`: Memory allocation failed
- `YQ_ERR_NOTFOUND`: Key not found
- `YQ_ERR_NOTSUP`: Operation not supported

### Error Messages
Use `yq_ttl_strerror()` to get human-readable error messages:

```c
int rc = yq_ttl_set(txn, &key, ttl);
if (rc != YQ_OK) {
    printf("Error setting TTL: %s\n", yq_ttl_strerror(rc));
}
```

## Building

To compile with TTL support:

```bash
gcc -std=c11 -Iinclude -DYQ_ENABLE_TTL -O2 -c src/*.c
ar rcs libyqdb.a *.o
```

Or with CMake:

```bash
cmake -S . -B build -DYQ_ENABLE_TTL=..
cmake --build build
```

## Best Practices

1. **Choose Appropriate Cleanup Intervals**: Balance between memory usage and performance
2. **Monitor Memory Usage**: Regularly check `yq_ttl_memory_usage()`
3. **Use Manual Cleanup for Critical Systems**: Control cleanup timing in production
4. **Handle Missing Keys Gracefully**: Check return values of `yq_ttl_get()`
5. **Clean Up Results**: Always free result structures to prevent memory leaks
6. **Use Transactions**: Always perform TTL operations within transactions
7. **Set Realistic TTL Values**: Consider data access patterns when setting TTL

## Limitations

1. **Single Writer**: Only one read-write transaction can be active at a time
2. **Memory Overhead**: TTL system consumes additional memory
3. **No Partial Updates**: Entire keys must be updated (TTL is reset)
4. **Time Precision**: TTL resolution is in seconds
5. **No Time Zones**: All timestamps are in local time

## Troubleshooting

### Common Issues

**Memory Usage Too High**
- Check for memory leaks with `yq_ttl_memory_usage()`
- Adjust cleanup intervals and batch sizes
- Use manual cleanup for better control

**Keys Not Expiring**
- Verify TTL was set correctly with `yq_ttl_get()`
- Check system time synchronization
- Ensure cleanup is running (automatic or manual)

**Performance Issues**
- Reduce cleanup frequency
- Use smaller batch sizes
- Monitor for expired key buildup

### Debug Tips

```c
// Check TTL configuration
yq_ttl_opts config;
yq_ttl_get_config(db, &config);
printf("Cleanup interval: %u seconds\n", config.cleanup_interval);

// Monitor expired keys
int64_t expired = yq_ttl_expired_count(db);
printf("Expired keys: %ld\n", expired);

// Check memory usage
int64_t memory = yq_ttl_memory_usage(db);
printf("Memory usage: %ld bytes\n", memory);
```