# New Features Documentation

This document describes the new features added to yq-DB in this release.

## Table of Contents

- [JSON Support](#json-support)
- [Batch Operations](#batch-operations)
- [Secondary Indexes](#secondary-indexes)
- [Time-To-Live (TTL)](#time-to-live-ttl)
- [Compression](#compression)
- [Encryption](#encryption)
- [Usage Examples](#usage-examples)
- [Building with New Features](#building-with-new-features)

---

## JSON Support

yq-DB now includes optional JSON support for handling structured data.

### Enabling JSON Support

To enable JSON functionality, define `YQ_ENABLE_JSON` before including `yq.h`:

```c
#define YQ_ENABLE_JSON 1
#include "yq.h"
```

Or compile with the `-DYQ_ENABLE_JSON` flag:

```bash
gcc -DYQ_ENABLE_JSON -std=c11 -Iinclude src/your_app.c src/*.o -o your_app
```

### JSON Data Types

The JSON support provides the following data types:

- `YQ_JSON_NULL` - JSON null value
- `YQ_JSON_BOOL` - Boolean values (true/false)
- `YQ_JSON_NUMBER` - Numeric values (double precision)
- `YQ_JSON_STRING` - String values
- `YQ_JSON_ARRAY` - Array of JSON values
- `YQ_JSON_OBJECT` - Object with key-value pairs

### JSON API Functions

#### Creating JSON Values

```c
// Create null value
yq_json_value *null_val = yq_json_create_null();

// Create boolean value
yq_json_value *bool_val = yq_json_create_bool(1);

// Create number
yq_json_value *num_val = yq_json_create_number(3.14);

// Create string
yq_json_value *str_val = yq_json_create_string("hello", 5);

// Create array
yq_json_value *arr_val = yq_json_create_array();

// Create object
yq_json_value *obj_val = yq_json_create_object();
```

#### Modifying JSON Values

```c
// Add element to array
yq_json_array_add(arr_val, str_val);

// Add member to object
yq_json_object_add(obj_val, "key", str_val);
```

#### Parsing and Generating JSON

```c
// Parse JSON from string
yq_json_value *parsed = NULL;
int rc = yq_json_parse("{\"name\":\"John\",\"age\":30}", 0, &parsed);

// Generate JSON string
char *json_str = NULL;
rc = yq_json_generate(parsed, 0, &json_str);
// json_str now contains: {"name":"John","age":30}

// Pretty print JSON
char *pretty_str = NULL;
rc = yq_json_generate(parsed, 1, &pretty_str);
// pretty_str contains formatted JSON

// Don't forget to free allocated memory
free(json_str);
free(pretty_str);
yq_json_value_destroy(parsed);
```

#### Database Integration

```c
// Store JSON value in database
yq_json_value *data = yq_json_create_object();
yq_json_object_add(data, "name", yq_json_create_string("John Doe", 8));
yq_json_object_add(data, "age", yq_json_create_number(25));
yq_json_object_add(data, "active", yq_json_create_bool(1));

yq_slice key = {"user:1", 6};
yq_json_put(txn, key, data);

// Retrieve JSON value from database
yq_json_value *retrieved = NULL;
yq_json_get(txn, key, &retrieved);

// Store raw JSON string
yq_json_put_raw(txn, key, "{\"name\":\"Jane\",\"age\":28}");

// Retrieve raw JSON string
yq_slice raw_json = {0};
yq_json_get_raw(txn, key, &raw_json);
```

### Error Handling

JSON operations return `yq_json_rc` error codes:

```c
switch (rc) {
    case YQ_JSON_OK:
        // Success
        break;
    case YQ_JSON_ERR:
        // JSON parsing error
        break;
    case YQ_JSON_NOMEM:
        // Memory allocation failed
        break;
    case YQ_JSON_DEPTH:
        // JSON nesting depth exceeded
        break;
    case YQ_JSON_INVALID:
        // Invalid JSON format
        break;
    case YQ_JSON_TRUNCATED:
        // JSON data was truncated
        break;
    case YQ_JSON_UNSUPPORTED:
        // Unsupported JSON feature
        break;
}
```

---

## Batch Operations

yq-DB now supports batch operations for improved performance when dealing with multiple operations.

### Enabling Batch Operations

To enable batch operations functionality, define `YQ_ENABLE_BATCH` before including `yq.h`:

```c
#define YQ_ENABLE_BATCH 1
#include "yq.h"
```

Or compile with the `-DYQ_ENABLE_BATCH` flag:

```bash
gcc -DYQ_ENABLE_BATCH -std=c11 -Iinclude src/your_app.c src/*.o -o your_app
```

### Batch Operation Types

- `YQ_BATCH_OP_PUT` - Insert or update (upsert)
- `YQ_BATCH_OP_DEL` - Delete key
- `YQ_BATCH_OP_UPSERT` - Insert only (key must not exist)

### Batch API Functions

#### Creating Batch Operations

```c
// Create a batch operation
yq_slice key1 = {"key1", 4};
yq_slice val1 = {"value1", 6};
yq_batch_op op1 = yq_batch_op_create(YQ_BATCH_OP_PUT, key1, val1);

// Create delete operation
yq_slice key2 = {"key2", 4};
yq_batch_op op2 = yq_batch_op_create_delete(key2);

// Create insert operation
yq_slice key3 = {"key3", 4};
yq_slice val3 = {"value3", 6};
yq_batch_op op3 = yq_batch_op_create_insert(key3, val3);
```

#### Executing Batch Operations

```c
// Prepare batch operations
yq_batch_op ops[] = {
    yq_batch_op_create_insert(key1, val1),
    yq_batch_op_create_delete(key2),
    yq_batch_op_create(YQ_BATCH_OP_PUT, key3, val3)
};
size_t count = sizeof(ops) / sizeof(ops[0]);

// Execute batch operations
yq_batch_result *result = NULL;
int rc = yq_batch_execute(txn, ops, count, &result);

if (rc == YQ_OK) {
    printf("Success: %d, Errors: %d\n", 
           result->success_count, result->error_count);
    
    if (result->error_count > 0) {
        for (int i = 0; i < result->error_count; i++) {
            int error_idx = result->error_indices[i];
            int error_code = result->error_codes[i];
            printf("Operation %d failed with error %d\n", 
                   error_idx, error_code);
        }
    }
}

// Clean up
yq_batch_result_destroy(result);
```

#### Specialized Batch Functions

```c
// Batch insert (only insert if key doesn't exist)
yq_batch_result *insert_result = NULL;
rc = yq_batch_insert(txn, ops, count, &insert_result);

// Batch delete
yq_slice keys[] = {"key1", "key2", "key3"};
yq_batch_result *delete_result = NULL;
rc = yq_batch_delete(txn, keys, 3, &delete_result);

// Batch get
yq_slice values[3];
rc = yq_batch_get(txn, keys, 3, values);

// Batch exists check
int exists[3];
rc = yq_batch_exists(txn, keys, 3, exists);
```

### Batch Results

The `yq_batch_result` structure provides detailed information about batch operation results:

```c
typedef struct yq_batch_result {
    uint32_t struct_size;     // Must be sizeof(yq_batch_result)
    int success_count;        // Number of successful operations
    int error_count;          // Number of failed operations
    int *error_indices;       // Indices of failed operations
    int *error_codes;         // Error codes for failed operations
} yq_batch_result;
```

### Error Handling

Batch operations return `yq_batch_rc` error codes:

```c
switch (rc) {
    case YQ_BATCH_OK:
        // Success
        break;
    case YQ_BATCH_ERR:
        // Batch operation error
        break;
    case YQ_BATCH_NOMEM:
        // Memory allocation failed
        break;
    case YQ_BATCH_EMPTY:
        // Batch was empty
        break;
    case YQ_BATCH_CONFLICT:
        // Batch operation conflict
        break;
    case YQ_BATCH_LIMIT:
        // Batch operation limit exceeded
        break;
}
```}

---

## Secondary Indexes

yq-DB now includes optional secondary indexes for advanced querying capabilities.

### Enabling Secondary Indexes

To enable secondary indexes functionality, define `YQ_ENABLE_INDEX` before including `yq.h`:

```c
#define YQ_ENABLE_INDEX 1
#include "yq.h"
```

Or compile with the `-DYQ_ENABLE_INDEX` flag:

```bash
gcc -DYQ_ENABLE_INDEX -std=c11 -Iinclude src/your_app.c src/*.o -o your_app
```

### Index Types

yq-DB supports four types of secondary indexes:

- **String Index**: For text data, supports exact matches, prefix searches, and range queries
- **Int64 Index**: For 64-bit integers, supports exact matches and range queries
- **Double Index**: For double-precision floats, supports exact matches and range queries  
- **Binary Index**: For binary data, supports exact matches only

### Creating Indexes

```c
// Create a string index
yq_index_opts opts = {sizeof(yq_index_opts), YQ_INDEX_STRING, 0, {0}};
yq_index_create(db, "email_index", &opts);

// Create a unique integer index
yq_index_opts unique_opts = {sizeof(yq_index_opts), YQ_INDEX_INT64, YQ_INDEX_UNIQUE, {0}};
yq_index_create(db, "user_id_index", &unique_opts);

// Create a descending double index
yq_index_opts desc_opts = {sizeof(yq_index_opts), YQ_INDEX_DOUBLE, YQ_INDEX_DESCENDING, {0}};
yq_index_create(db, "price_index", &desc_opts);
```

### Querying Indexes

```c
// Exact match
yq_slice email = {"john@example.com", 16};
yq_index_result *result = NULL;
yq_index_find_exact(txn, "email_index", &email, &result);

// Range query
int64_t min_age = 25;
int64_t max_age = 35;
yq_slice min_age_slice = {&min_age, sizeof(min_age)};
yq_slice max_age_slice = {&max_age, sizeof(max_age)};
yq_index_find_range(txn, "age_index", &min_age_slice, &max_age_slice, &result);

// Prefix search (string indexes only)
yq_slice prefix = {"john", 4};
yq_index_find_prefix(txn, "name_index", &prefix, &result);

// Greater than query
double min_price = 100.0;
yq_slice min_price_slice = {&min_price, sizeof(min_price)};
yq_index_find_gt(txn, "price_index", &min_price_slice, &result);

// Process results
if (result && result->count > 0) {
    for (int i = 0; i < result->count; i++) {
        printf("Found key: %.*s\n", 
               (int)result->keys[i].size, (const char *)result->keys[i].data);
    }
}

// Clean up
yq_index_result_free(result);
```

### Index Management

```c
// Check if index exists
if (yq_index_exists(db, "email_index")) {
    printf("Email index exists\n");
}

// Get index statistics
int64_t size = yq_index_size(db, "email_index");
int64_t memory = yq_index_memory_usage(db, "email_index");
printf("Index size: %ld, Memory: %ld bytes\n", size, memory);

// Drop index
yq_index_drop(db, "unused_index");

// List all indexes
int count = 0;
char **indexes = yq_index_list(db, &count);
for (int i = 0; i < count; i++) {
    printf("Index: %s\n", indexes[i]);
    free(indexes[i]);
}
free(indexes);
```

### Index Flags

- `YQ_INDEX_UNIQUE`: Enforce unique values
- `YQ_INDEX_CASE_SENSITIVE`: Case-sensitive string comparison
- `YQ_INDEX_DESCENDING`: Return results in descending order
- `YQ_INDEX_NULLS_FIRST`: Place NULL values first in sorted results

### Performance Considerations

- Indexes are automatically maintained during data operations
- Large indexes consume additional memory
- Use appropriate index types for your data
- Drop unused indexes to free memory
- Monitor index size and memory usage

### Error Handling

```c
int rc = yq_index_create(db, "my_index", &opts);
if (rc != YQ_OK) {
    printf("Error creating index: %s\n", yq_index_strerror(rc));
}
```

---

## Usage Examples

### Complete Example: JSON with Batch Operations

```c
#include <stdio.h>
#include <string.h>
#include "yq.h"

#define YQ_ENABLE_JSON 1
#define YQ_ENABLE_BATCH 1

int main(void) {
    yq_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(opts);
    opts.flags = YQ_OPEN_CREATE;
    
    yq_db *db = NULL;
    if (yq_open("app_data.yqdb", &opts, &db) != YQ_OK) {
        return 1;
    }
    
    // Start transaction
    yq_txn *txn = NULL;
    yq_txn_begin(db, YQ_TXN_READWRITE, &txn);
    
    // Create batch operations for user data
    yq_batch_op ops[3];
    
    // User 1
    yq_json_value *user1 = yq_json_create_object();
    yq_json_object_add(user1, "id", yq_json_create_number(1));
    yq_json_object_add(user1, "name", yq_json_create_string("Alice", 5));
    yq_json_object_add(user1, "email", yq_json_create_string("alice@example.com", 16));
    yq_slice key1 = {"user:1", 6};
    ops[0] = yq_batch_op_create(YQ_BATCH_OP_PUT, key1, (yq_slice){0});
    
    // User 2
    yq_json_value *user2 = yq_json_create_object();
    yq_json_object_add(user2, "id", yq_json_create_number(2));
    yq_json_object_add(user2, "name", yq_json_create_string("Bob", 3));
    yq_json_object_add(user2, "email", yq_json_create_string("bob@example.com", 14));
    yq_slice key2 = {"user:2", 6};
    ops[1] = yq_batch_op_create(YQ_BATCH_OP_PUT, key2, (yq_slice){0});
    
    // Delete user 3 (if exists)
    yq_slice key3 = {"user:3", 6};
    ops[2] = yq_batch_op_create_delete(key3);
    
    // Execute batch operations
    yq_batch_result *result = NULL;
    int rc = yq_batch_execute(txn, ops, 3, &result);
    
    if (rc == YQ_OK) {
        printf("Batch operation completed: %d success, %d errors\n",
               result->success_count, result->error_count);
    }
    
    yq_batch_result_destroy(result);
    
    // Commit transaction
    yq_txn_commit(txn);
    yq_close(db);
    
    return 0;
}
```

---

## Time-To-Live (TTL)

yq-DB now includes optional Time-To-Live (TTL) functionality for automatic key expiration.

### Enabling TTL

To enable TTL functionality, define `YQ_ENABLE_TTL` before including `yq.h`:

```c
#define YQ_ENABLE_TTL 1
#include "yq.h"
```

Or compile with the `-DYQ_ENABLE_TTL` flag:

```bash
gcc -DYQ_ENABLE_TTL -std=c11 -Iinclude src/your_app.c src/*.o -o your_app
```

### Basic Usage

```c
// Configure TTL (automatic cleanup every 60 seconds)
yq_ttl_opts opts = {sizeof(yq_ttl_opts), 60, 1000, {0}};
yq_ttl_configure(db, &opts);

// Set TTL for a key (30 seconds)
yq_slice key = {"session_123", 11};
yq_ttl_set(txn, &key, 30);

// Check remaining TTL
int32_t remaining = yq_ttl_get(txn, &key);
if (remaining > 0) {
    printf("Key expires in %d seconds\n", remaining);
} else {
    printf("Key has expired\n");
}

// Remove TTL
yq_ttl_unset(txn, &key);
```

### Cleanup Operations

```c
// Manual cleanup
yq_ttl_result *result = NULL;
yq_ttl_cleanup(db, 0, &result);
printf("Cleaned %d keys\n", result->cleaned_count);
yq_ttl_result_free(result);

// Find expiring keys
yq_slice **expiring = NULL;
int count = 0;
expiring = yq_ttl_find_expiring(txn, 60, &count);
for (int i = 0; i < count; i++) {
    printf("Key %.*s will expire soon\n", 
           (int)expiring[i]->size, (const char *)expiring[i]->data);
}
yq_ttl_query_result_free(expiring, count);

// Find expired keys
yq_slice **expired = NULL;
expired = yq_ttl_find_expired(txn, &count);
for (int i = 0; i < count; i++) {
    printf("Key %.*s has expired\n", 
           (int)expired[i]->size, (const char *)expired[i]->data);
    yq_delete(txn, *expired[i]);
}
yq_ttl_query_result_free(expired, count);
```

### Statistics and Monitoring

```c
// Get expired key count
int64_t expired_count = yq_ttl_expired_count(db);
printf("Expired keys: %ld\n", expired_count);

// Get memory usage
int64_t memory = yq_ttl_memory_usage(db);
printf("Memory usage: %ld bytes\n", memory);

// Get current time
time_t now = yq_ttl_now();
printf("Current time: %ld\n", now);

// Format time
char *time_str = yq_ttl_format_time(now);
printf("Formatted time: %s\n", time_str);
free(time_str);
```

### Use Cases

- **Session Management**: Automatically expire user sessions
- **Caching**: Set expiration times for cached data
- **Temporary Storage**: Automatically clean up temporary files
- **Rate Limiting**: Expire rate limit counters after time window
- **Analytics**: Track data freshness and expiration patterns

### Configuration Options

- `cleanup_interval`: Automatic cleanup interval in seconds (0 = manual)
- `max_expired`: Maximum keys to clean per interval (0 = unlimited)

### Error Handling

```c
int rc = yq_ttl_set(txn, &key, ttl);
if (rc != YQ_OK) {
    printf("Error: %s\n", yq_ttl_strerror(rc));
}
```

---

## Compression

yq-DB now includes optional compression functionality for automatic value compression, reducing storage space and improving I/O performance.

### Enabling Compression

To enable compression functionality, define `YQ_ENABLE_COMPRESS` before including `yq.h`:

```c
#define YQ_ENABLE_COMPRESS 1
#include "yq.h"
```

Or compile with the `-DYQ_ENABLE_COMPRESS` flag:

```bash
gcc -DYQ_ENABLE_COMPRESS -std=c11 -Iinclude src/your_app.c src/*.o -o your_app
```

### Basic Usage

```c
// Configure compression (LZ4, normal level, min 1KB)
yq_compress_opts opts = {
    .struct_size = sizeof(yq_compress_opts),
    .algorithm = YQ_COMPRESS_LZ4,
    .level = YQ_COMPRESS_LEVEL_NORMAL,
    .min_size = 1024,  // Only compress values > 1KB
    .reserved = {0}
};
yq_compress_configure(db, &opts);

// Store large data (will be automatically compressed)
yq_slice key = {"large_data", 10};
yq_slice value = {large_data_ptr, large_data_size};
yq_put(txn, key, value, YQ_PUT_UPSERT);

// Check if data is compressed
int is_compressed = yq_compress_is_compressed(txn, &key);
printf("Data is %scompressed\n", is_compressed ? "" : "not ");

// Get compression statistics
int64_t compressed_count = yq_compress_stats(db);
int64_t saved_space = yq_compress_saved_space(db);
printf("Compressed %ld items, saved %ld bytes\n", compressed_count, saved_space);
```

### Compression Algorithms

| Algorithm | Speed | Compression Ratio | Memory Usage | Best For |
|-----------|-------|------------------|--------------|----------|
| **Snappy** | Very Fast | Low (60-80%) | Low | Real-time applications |
| **LZ4** | Fast | Medium (50-70%) | Low | General purpose |
| **Zstd** | Good | High (40-60%) | Medium | Storage optimization |
| **Zlib** | Medium | Medium (50-70%) | Medium | Compatibility |

### Configuration Options

- `algorithm`: Compression algorithm selection
- `level`: Compression level (Fast, Normal, Max)
- `min_size`: Minimum size for compression (avoid overhead for small data)

### Monitoring and Statistics

```c
// Monitor compression effectiveness
int64_t compressed_count = yq_compress_stats(db);
int64_t saved_space = yq_compress_saved_space(db);
int64_t memory_usage = yq_compress_memory_usage(db);

printf("Compression Statistics:\n");
printf("  Compressed items: %ld\n", compressed_count);
printf("  Space saved: %ld bytes\n", saved_space);
printf("  Memory usage: %ld bytes\n", memory_usage);

// Estimate compression for new data
size_t estimated_size = yq_compress_estimate_size(new_data, new_size, YQ_COMPRESS_ZSTD);
printf("Estimated compressed size: %zu bytes\n", estimated_size);
```

### Use Cases

- **Large Text Storage**: Compress documents and logs
- **Binary Data**: Reduce storage for images and files
- **Network Transfer**: Minimize data transfer for remote databases
- **Memory Efficiency**: Lower memory usage for cached data

### Error Handling

```c
int rc = yq_compress_data(data, size, level, &result);
if (rc != YQ_OK) {
    printf("Error: %s\n", yq_compress_strerror(rc));
}
```

### Performance Considerations

- **CPU Usage**: Compression consumes CPU cycles
- **Memory Overhead**: Additional memory for compression buffers
- **Latency**: Increased latency for write operations
- **Storage Efficiency**: Large values benefit most from compression

---

## Encryption

yq-DB now includes optional encryption functionality for secure storage of sensitive data.

### Enabling Encryption

To enable encryption functionality, define `YQ_ENABLE_CRYPTO` before including `yq.h`:

```c
#define YQ_ENABLE_CRYPTO 1
#include "yq.h"
```

Or compile with the `-DYQ_ENABLE_CRYPTO` flag:

```bash
gcc -DYQ_ENABLE_CRYPTO -std=c11 -Iinclude src/your_app.c src/*.o -o your_app
```

### Basic Usage

```c
// Configure encryption (AES-256, CBC mode, min 1KB)
yq_crypto_opts opts = {
    .struct_size = sizeof(yq_crypto_opts),
    .algorithm = YQ_CRYPTO_AES256,
    .mode = YQ_CRYPTO_MODE_CBC,
    .key_size = 32,  // 256-bit key
    .iv_size = 16,   // 128-bit IV
    .min_size = 1024, // Only encrypt values > 1KB
    .reserved = {0}
};
yq_crypto_configure(db, &opts);

// Store sensitive data (will be automatically encrypted)
yq_slice key = {"user_password", 13};
yq_slice value = {password_ptr, password_size};
yq_put(txn, key, value, YQ_PUT_UPSERT);

// Check if data is encrypted
int is_encrypted = yq_crypto_is_encrypted(txn, &key);
printf("Data is %sencrypted\n", is_encrypted ? "" : "not ");

// Get encryption statistics
int64_t encrypted_count = yq_crypto_stats(db);
int64_t key_count = yq_crypto_key_count(db);
printf("Encrypted %ld items with %ld keys\n", encrypted_count, key_count);
```

### Encryption Algorithms

| Algorithm | Security | Speed | Memory Usage | Best For |
|-----------|----------|-------|--------------|----------|
| **AES-256** | Very High | Good | Medium | General purpose, high security |
| **ChaCha20** | High | Excellent | Low | Performance-critical applications |
| **XOR** | Low (demo) | Very Fast | Low | Educational purposes |

### Key Management

```c
// Generate random key
uint8_t *key = yq_crypto_generate_key(32);  // 256-bit key
free(key);

// Derive key from password
uint8_t *salt = yq_crypto_generate_salt(16);
uint8_t *derived_key = yq_crypto_derive_key("user_password", 32, salt, 16, 10000);
free(salt);
free(derived_key);

// Generate random IV
uint8_t *iv = yq_crypto_generate_iv(16);
free(iv);
```

### Data Integrity

```c
// Generate hash for data integrity
uint8_t *data_hash = yq_crypto_hash(sensitive_data, data_size, "SHA256");

// Verify data integrity
int is_valid = yq_crypto_verify(retrieved_data, data_size, data_hash, "SHA256");
free(data_hash);
```

### Configuration Options

- `algorithm`: Encryption algorithm selection
- `mode`: Encryption mode (ECB, CBC, GCM)
- `key_size`: Key size in bytes
- `iv_size`: Initialization vector size
- `min_size`: Minimum size for encryption (avoid overhead for small data)

### Use Cases

- **Sensitive Data**: Store passwords, API keys, personal information
- **Compliance**: Meet regulatory requirements for data protection
- **Privacy**: Ensure user data remains confidential
- **Secure Storage**: Encrypt data at rest in the database

### Performance Considerations

- **CPU Usage**: Encryption consumes CPU cycles
- **Memory Overhead**: Additional memory for encryption buffers
- **Latency**: Increased latency for write operations
- **Security**: Stronger algorithms provide better security but may be slower

---

## Building with New Features

### CMake Build

To build yq-DB with all new features enabled:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DYQ_ENABLE_JSON=ON -DYQ_ENABLE_BATCH=ON -DYQ_ENABLE_INDEX=ON -DYQ_ENABLE_TTL=ON -DYQ_ENABLE_COMPRESS=ON -DYQ_ENABLE_CRYPTO=ON
cmake --build build
```

### Direct GCC Build

```bash
gcc -DYQ_ENABLE_JSON=1 -DYQ_ENABLE_BATCH=1 -DYQ_ENABLE_INDEX=1 -DYQ_ENABLE_TTL=1 -DYQ_ENABLE_COMPRESS=1 -DYQ_ENABLE_CRYPTO=1 -std=c11 -Wall -Wextra -Iinclude -O2 -c src/*.c
ar rcs libyqdb.a *.o
```

### Linking Your Application

```bash
gcc -DYQ_ENABLE_JSON=1 -DYQ_ENABLE_BATCH=1 -std=c11 -Iinclude your_app.c libyqdb.a -o your_app
```

---

## Performance Considerations

### JSON Operations

- JSON parsing and generation involve memory allocation and string copying
- For simple key-value storage, consider using raw strings instead of JSON
- JSON arrays and objects use dynamic memory management

### Batch Operations

- Batch operations reduce transaction overhead for multiple operations
- Batch operations are atomic within a single transaction
- Error handling in batch operations provides detailed feedback
- Large batches may require more memory for error tracking

### Memory Management

- Always free allocated JSON values using `yq_json_value_destroy()`
- Free generated JSON strings using `free()`
- Free batch result structures using `yq_batch_result_destroy()`

---

## Error Handling Best Practices

1. **Check return values** for all JSON and batch operations
2. **Clean up allocated memory** to prevent memory leaks
3. **Handle errors gracefully** with appropriate error messages
4. **Use batch results** to understand which operations failed and why
5. **Validate input parameters** before calling JSON functions

---

## Future Enhancements

Potential future enhancements for yq-DB:

- JSON Schema validation
- Compression for JSON values
- More sophisticated batch operations (conditional updates)
- JSON query capabilities
- TTL (Time-To-Live) support for keys
- Custom comparators for JSON values
- Streaming JSON parser for large documents

---

## Conclusion

The new JSON and batch operation features significantly enhance yq-DB's capabilities for modern applications. These features provide:

1. **Structured data handling** with full JSON support
2. **Improved performance** with batch operations
3. **Better error handling** with detailed result reporting
4. **Memory safety** with proper cleanup functions

These additions make yq-DB more suitable for complex applications while maintaining its lightweight and embedded nature.