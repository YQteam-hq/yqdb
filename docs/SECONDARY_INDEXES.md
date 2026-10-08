# Secondary Indexes for yq-DB

[English](SECONDARY_INDEXES.md) · [简体中文](SECONDARY_INDEXES.zh-CN.md)

**Secondary indexes** allow you to create indexes on values and perform efficient lookups, range queries, and filtering operations.

## Overview

Secondary indexes enable yq-DB to function more like a traditional database by allowing you to:

- Create indexes on values (not just keys)
- Perform exact matches, range queries, and prefix searches
- Support multiple index types (string, int64, double, binary)
- Improve query performance for complex data access patterns

## Features

| Index Type | Supported Operations | Use Cases |
|------------|---------------------|-----------|
| **String** | Exact match, prefix search, range queries | Usernames, email addresses, text data |
| **Int64** | Exact match, range queries (>, >=, <, <=) | IDs, timestamps, counters, numeric data |
| **Double** | Exact match, range queries (>, >=, <, <=) | Prices, measurements, scientific data |
| **Binary** | Exact match | Raw binary data, encrypted data |

## Quick Start

```c
#include "yq.h"
#include "yq_index.h"

int main() {
    // Initialize database
    yq_db *db = NULL;
    yq_opts opts = {sizeof(yq_opts), 0, 0, 0, 0, 0, 0, 0, 0, {0}};
    yq_open("my_database.yqdb", &opts, &db);
    
    // Create a string index
    yq_index_opts index_opts = {sizeof(yq_index_opts), YQ_INDEX_STRING, 0, {0}};
    yq_index_create(db, "email_index", &index_opts);
    
    // Insert data with transaction
    yq_txn *txn = NULL;
    yq_txn_begin(db, YQ_TXN_READWRITE, &txn);
    
    yq_slice key1 = {"user1", 5};
    yq_slice value1 = {"john@example.com", 16};
    yq_put(txn, key1, value1, YQ_PUT_UPSERT);
    
    yq_slice key2 = {"user2", 5};
    yq_slice value2 = {"jane@example.com", 16};
    yq_put(txn, key2, value2, YQ_PUT_UPSERT);
    
    yq_txn_commit(txn);
    
    // Query by email
    yq_index_result *result = NULL;
    yq_slice email = {"john@example.com", 16};
    yq_index_find_exact(txn, "email_index", &email, &result);
    
    if (result->count > 0) {
        printf("Found user: %.*s\n", (int)result->keys[0].size, (const char *)result->keys[0].data);
    }
    
    yq_index_result_free(result);
    yq_close(db);
    return 0;
}
```

## API Reference

### Index Management

#### `yq_index_create()`
Create a new secondary index.

```c
int yq_index_create(yq_db *db, const char *name, const yq_index_opts *opts);
```

**Parameters:**
- `db`: Database handle
- `name`: Index name (must be unique)
- `opts`: Index configuration

**Returns:** `YQ_OK` on success, error code on failure

#### `yq_index_drop()`
Drop an existing index.

```c
int yq_index_drop(yq_db *db, const char *name);
```

**Parameters:**
- `db`: Database handle
- `name`: Index name to drop

**Returns:** `YQ_OK` on success, `YQ_ERR_NOTFOUND` if index doesn't exist

#### `yq_index_exists()`
Check if an index exists.

```c
int yq_index_exists(yq_db *db, const char *name);
```

**Parameters:**
- `db`: Database handle
- `name`: Index name to check

**Returns:** `1` if index exists, `0` otherwise

### Query Operations

#### `yq_index_find_exact()`
Find keys with exact value match.

```c
int yq_index_find_exact(yq_txn *txn, const char *index_name, const yq_slice *value, yq_index_result **result);
```

**Parameters:**
- `txn`: Transaction handle
- `index_name`: Name of the index to query
- `value`: Value to match exactly
- `result`: Output parameter for query results

**Returns:** `YQ_OK` on success, error code on failure

#### `yq_index_find_range()`
Find keys within a value range.

```c
int yq_index_find_range(yq_txn *txn, const char *index_name, const yq_slice *start, const yq_slice *end, yq_index_result **result);
```

**Parameters:**
- `txn`: Transaction handle
- `index_name`: Name of the index to query
- `start`: Start of range (inclusive)
- `end`: End of range (inclusive)
- `result`: Output parameter for query results

**Returns:** `YQ_OK` on success, error code on failure

#### `yq_index_find_prefix()`
Find keys with string prefix (string indexes only).

```c
int yq_index_find_prefix(yq_txn *txn, const char *index_name, const yq_slice *prefix, yq_index_result **result);
```

**Parameters:**
- `txn`: Transaction handle
- `index_name`: Name of the index to query
- `prefix`: Prefix string to match
- `result`: Output parameter for query results

**Returns:** `YQ_OK` on success, error code on failure

#### Comparison Operations
```c
int yq_index_find_gt(yq_txn *txn, const char *index_name, const yq_slice *value, yq_index_result **result);
int yq_index_find_gte(yq_txn *txn, const char *index_name, const yq_slice *value, yq_index_result **result);
int yq_index_find_lt(yq_txn *txn, const char *index_name, const yq_slice *value, yq_index_result **result);
int yq_index_find_lte(yq_txn *txn, const char *index_name, const yq_slice *value, yq_index_result **result);
```

### Index Statistics

#### `yq_index_size()`
Get the number of entries in an index.

```c
int64_t yq_index_size(yq_db *db, const char *name);
```

**Parameters:**
- `db`: Database handle
- `name`: Index name

**Returns:** Number of entries, or `-1` on error

#### `yq_index_memory_usage()`
Get memory usage of an index.

```c
int64_t yq_index_memory_usage(yq_db *db, const char *name);
```

**Parameters:**
- `db`: Database handle
- `name`: Index name

**Returns:** Memory usage in bytes, or `-1` on error

### Result Management

#### `yq_index_result_free()`
Free query results.

```c
void yq_index_result_free(yq_index_result *result);
```

**Parameters:**
- `result`: Query result to free

## Index Types

### String Index (`YQ_INDEX_STRING`)
- **Operations**: Exact match, prefix search, range queries
- **Value Format**: Null-terminated string or binary data
- **Use Cases**: Usernames, email addresses, text fields, tags

### Int64 Index (`YQ_INDEX_INT64`)
- **Operations**: Exact match, range queries
- **Value Format**: 64-bit integer (8 bytes)
- **Use Cases**: IDs, timestamps, counters, numeric data

### Double Index (`YQ_INDEX_DOUBLE`)
- **Operations**: Exact match, range queries
- **Value Format**: 64-bit double-precision float (8 bytes)
- **Use Cases**: Prices, measurements, scientific data, coordinates

### Binary Index (`YQ_INDEX_BINARY`)
- **Operations**: Exact match only
- **Value Format**: Binary data of any length
- **Use Cases**: Raw binary data, encrypted data, blobs

## Index Flags

### `YQ_INDEX_UNIQUE`
Enforce unique values in the index. Inserting duplicate values will fail.

```c
yq_index_opts opts = {sizeof(yq_index_opts), YQ_INDEX_STRING, YQ_INDEX_UNIQUE, {0}};
```

### `YQ_INDEX_CASE_SENSITIVE`
For string indexes, make comparisons case-sensitive (default is case-insensitive).

```c
yq_index_opts opts = {sizeof(yq_index_opts), YQ_INDEX_STRING, YQ_INDEX_CASE_SENSITIVE, {0}};
```

### `YQ_INDEX_DESCENDING`
Return results in descending order.

```c
yq_index_opts opts = {sizeof(yq_index_opts), YQ_INDEX_INT64, YQ_INDEX_DESCENDING, {0}};
```

### `YQ_INDEX_NULLS_FIRST`
Place NULL values at the beginning of sorted results.

```c
yq_index_opts opts = {sizeof(yq_index_opts), YQ_INDEX_STRING, YQ_INDEX_NULLS_FIRST, {0}};
```

## Performance Considerations

### Memory Usage
- Each index consumes additional memory proportional to the number of entries
- Hash-based indexes provide O(1) average time complexity for lookups
- Memory usage can be monitored with `yq_index_memory_usage()`

### Index Maintenance
- Indexes are automatically updated when data is inserted, updated, or deleted
- Large indexes may impact write performance due to additional processing
- Use `yq_index_drop()` to remove unused indexes and free memory

### Query Performance
- Exact matches are fastest for all index types
- Range queries are supported for numeric and string types
- Prefix searches are optimized for string indexes
- Results are returned in sorted order for range queries

## Error Handling

### Common Error Codes
- `YQ_ERR_INVAL`: Invalid parameters (NULL pointers, wrong size)
- `YQ_ERR_NOMEM`: Memory allocation failed
- `YQ_ERR_NOTFOUND`: Index not found
- `YQ_ERR_EXISTS`: Index already exists
- `YQ_ERR_NOTSUP`: Operation not supported for index type

### Error Messages
Use `yq_index_strerror()` to get human-readable error messages:

```c
int rc = yq_index_create(db, "my_index", &opts);
if (rc != YQ_OK) {
    printf("Error creating index: %s\n", yq_index_strerror(rc));
}
```

## Examples

### User Management System

```c
// Create user indexes
yq_index_opts email_index = {sizeof(yq_index_opts), YQ_INDEX_STRING, YQ_INDEX_UNIQUE, {0}};
yq_index_opts age_index = {sizeof(yq_index_opts), YQ_INDEX_INT64, 0, {0}};
yq_index_create(db, "email_index", &email_index);
yq_index_create(db, "age_index", &age_index);

// Find users by email
yq_slice email = {"john@example.com", 16};
yq_index_result *result = NULL;
yq_index_find_exact(txn, "email_index", &email, &result);

// Find users in age range
yq_slice min_age = {&age, sizeof(age)}; // age = 25
yq_slice max_age = {&age2, sizeof(age2)}; // age2 = 35
yq_index_find_range(txn, "age_index", &min_age, &max_age, &result);
```

### Product Catalog

```c
// Create product indexes
yq_index_opts name_index = {sizeof(yq_index_opts), YQ_INDEX_STRING, 0, {0}};
yq_index_opts price_index = {sizeof(yq_index_opts), YQ_INDEX_DOUBLE, 0, {0}};
yq_index_opts category_index = {sizeof(yq_index_opts), YQ_INDEX_STRING, 0, {0}};
yq_index_create(db, "name_index", &name_index);
yq_index_create(db, "price_index", &price_index);
yq_index_create(db, "category_index", &category_index);

// Find products by category prefix
yq_slice category = {"electronic", 10};
yq_index_find_prefix(txn, "category_index", &category, &result);

// Find products by price range
double min_price = 100.0;
double max_price = 1000.0;
yq_slice min_price_slice = {&min_price, sizeof(min_price)};
yq_slice max_price_slice = {&max_price, sizeof(max_price)};
yq_index_find_range(txn, "price_index", &min_price_slice, &max_price_slice, &result);
```

## Limitations

1. **Single Writer**: Only one read-write transaction can be active at a time
2. **Memory Usage**: Large indexes consume significant memory
3. **No Partial Updates**: Entire values must be updated (indexes are rebuilt)
4. **No Composite Indexes**: Only single-column indexes are supported
5. **No Index Joins**: Complex queries requiring multiple indexes are not supported

## Best Practices

1. **Choose Appropriate Index Types**: Use the right index type for your data
2. **Monitor Memory Usage**: Regularly check index memory usage with `yq_index_memory_usage()`
3. **Drop Unused Indexes**: Remove indexes that are no longer needed
4. **Use Transactions**: Always perform index operations within transactions
5. **Handle Errors Gracefully**: Check return values and use `yq_index_strerror()` for debugging

## Building

To compile with secondary indexes support:

```bash
gcc -std=c11 -Iinclude -DYQ_ENABLE_INDEX -O2 -c src/*.c
ar rcs libyqdb.a *.o
```

Or with CMake:

```bash
cmake -S . -B build -DYQ_ENABLE_INDEX=..
cmake --build build
```