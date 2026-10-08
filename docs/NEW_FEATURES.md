# New Features Documentation

This document describes the new features added to yq-DB in this release.

## Table of Contents

- [JSON Support](#json-support)
- [Batch Operations](#batch-operations)
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

## Building with New Features

### CMake Build

To build yq-DB with JSON and batch support enabled:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DYQ_ENABLE_JSON=ON -DYQ_ENABLE_BATCH=ON
cmake --build build
```

### Direct GCC Build

```bash
gcc -DYQ_ENABLE_JSON=1 -DYQ_ENABLE_BATCH=1 -std=c11 -Wall -Wextra -Iinclude -O2 -c src/*.c
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