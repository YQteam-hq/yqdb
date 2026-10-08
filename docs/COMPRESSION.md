# Compression for yq-DB

[English](COMPRESSION.md) · [简体中文](COMPRESSION.zh-CN.md)

**Compression** functionality enables automatic compression of large values, reducing storage space and improving I/O performance for yq-DB.

## Overview

yq-DB supports automatic compression of values using multiple compression algorithms. This is particularly useful for:

- **Reduced Storage**: Compress large values to save disk space
- **Faster I/O**: Smaller data means faster read/write operations
- **Network Efficiency**: Reduced data transfer for remote databases
- **Memory Efficiency**: Lower memory usage for cached data

## Features

| Feature | Description | Benefits |
|---------|-------------|----------|
| **Multiple Algorithms** | Support for Snappy, LZ4, Zstd, Zlib | Choose best algorithm for your use case |
| **Automatic Compression** | Configurable minimum size thresholds | No manual intervention required |
| **Compression Levels** | Fast, Normal, Max compression levels | Balance between speed and compression ratio |
| **Statistics & Monitoring** | Track compression effectiveness | Monitor storage savings and performance |
| **Configurable Thresholds** | Minimum size for compression | Avoid overhead for small values |

## Quick Start

```c
#include "yq.h"
#include "yq_compress.h"

int main() {
    // Initialize database
    yq_db *db = NULL;
    yq_opts opts = {sizeof(yq_opts), 0, 0, 0, 0, 0, 0, 0, 0, {0}};
    yq_open("my_database.yqdb", &opts, &db);
    
    // Configure compression (LZ4, normal level, min 1KB)
    yq_compress_opts compress_opts = {
        .struct_size = sizeof(yq_compress_opts),
        .algorithm = YQ_COMPRESS_LZ4,
        .level = YQ_COMPRESS_LEVEL_NORMAL,
        .min_size = 1024,  // Only compress values > 1KB
        .reserved = {0}
    };
    yq_compress_configure(db, &compress_opts);
    
    // Store large data (will be automatically compressed)
    yq_txn *txn = NULL;
    yq_txn_begin(db, YQ_TXN_READWRITE, &txn);
    
    yq_slice key = {"large_data", 10};
    yq_slice value = {large_data_ptr, large_data_size};
    yq_put(txn, key, value, YQ_PUT_UPSERT);
    
    // Data is automatically compressed if size > min_size
    yq_txn_commit(txn);
    
    // Check if data is compressed
    int is_compressed = yq_compress_is_compressed(txn, &key);
    printf("Data is %scompressed\n", is_compressed ? "" : "not ");
    
    // Get compression statistics
    int64_t compressed_count = yq_compress_stats(db);
    int64_t saved_space = yq_compress_saved_space(db);
    printf("Compressed %ld items, saved %ld bytes\n", compressed_count, saved_space);
    
    yq_close(db);
    return 0;
}
```

## API Reference

### Configuration

#### `yq_compress_configure()`
Set compression configuration.

```c
int yq_compress_configure(yq_db *db, const yq_compress_opts *opts);
```

**Parameters:**
- `db`: Database handle
- `opts`: Compression configuration options

**Returns:** `YQ_OK` on success, error code on failure

**Configuration Options:**
- `algorithm`: Compression algorithm (Snappy, LZ4, Zstd, Zlib)
- `level`: Compression level (Fast, Normal, Max)
- `min_size`: Minimum size for compression (bytes)

#### `yq_compress_get_config()`
Get current compression configuration.

```c
int yq_compress_get_config(yq_db *db, yq_compress_opts *opts);
```

**Parameters:**
- `db`: Database handle
- `opts`: Output parameter for configuration

**Returns:** `YQ_OK` on success, error code on failure

### Compression Operations

#### `yq_compress_data()`
Compress data manually.

```c
int yq_compress_data(const void *data, size_t size, yq_compress_level level, yq_compress_result **result);
```

**Parameters:**
- `data`: Data to compress
- `size`: Size of data
- `level`: Compression level
- `result`: Output parameter for compression results

**Returns:** `YQ_OK` on success, error code on failure

#### `yq_decompress_data()`
Decompress data manually.

```c
int yq_decompress_data(const void *data, size_t size, yq_compress_result **result);
```

**Parameters:**
- `data`: Data to decompress
- `size`: Size of compressed data
- `result`: Output parameter for decompression results

**Returns:** `YQ_OK` on success, error code on failure

#### `yq_compress_is_compressed()`
Check if data is compressed.

```c
int yq_compress_is_compressed(yq_txn *txn, const yq_slice *key);
```

**Parameters:**
- `txn`: Transaction handle
- `key`: Key to check

**Returns:**
- `1`: Data is compressed
- `0`: Data is not compressed
- `-1`: Error

### Statistics

#### `yq_compress_stats()`
Get number of compressed items.

```c
int64_t yq_compress_stats(yq_db *db);
```

**Parameters:**
- `db`: Database handle

**Returns:** Number of compressed items, or `-1` on error

#### `yq_compress_saved_space()`
Get total space saved by compression.

```c
int64_t yq_compress_saved_space(yq_db *db);
```

**Parameters:**
- `db`: Database handle

**Returns:** Space saved in bytes, or `-1` on error

#### `yq_compress_memory_usage()`
Get memory usage of compression system.

```c
int64_t yq_compress_memory_usage(yq_db *db);
```

**Parameters:**
- `db`: Database handle

**Returns:** Memory usage in bytes, or `-1` on error

### Utility Functions

#### `yq_compress_result_free()`
Free compression results.

```c
void yq_compress_result_free(yq_compress_result *result);
```

**Parameters:**
- `result`: Compression results to free

#### `yq_compress_algorithm_name()`
Get algorithm name.

```c
const char *yq_compress_algorithm_name(yq_compress_algorithm algorithm);
```

**Parameters:**
- `algorithm`: Compression algorithm

**Returns:** Algorithm name string

#### `yq_compress_level_name()`
Get compression level name.

```c
const char *yq_compress_level_name(yq_compress_level level);
```

**Parameters:**
- `level`: Compression level

**Returns:** Level name string

#### `yq_compress_estimate_size()`
Estimate compressed size.

```c
size_t yq_compress_estimate_size(const void *data, size_t size, yq_compress_algorithm algorithm);
```

**Parameters:**
- `data`: Data to estimate
- `size`: Size of data
- `algorithm`: Compression algorithm

**Returns:** Estimated compressed size, or `0` on error

#### `yq_compress_is_available()`
Check if algorithm is available.

```c
int yq_compress_is_available(yq_compress_algorithm algorithm);
```

**Parameters:**
- `algorithm`: Compression algorithm

**Returns:** `1` if available, `0` if not

## Configuration Examples

### Basic Compression
```c
// Configure basic LZ4 compression
yq_compress_opts opts = {
    .struct_size = sizeof(yq_compress_opts),
    .algorithm = YQ_COMPRESS_LZ4,
    .level = YQ_COMPRESS_LEVEL_NORMAL,
    .min_size = 1024,  // Compress values > 1KB
    .reserved = {0}
};
yq_compress_configure(db, &opts);
```

### High Compression
```c
// Configure maximum compression with Zstd
yq_compress_opts opts = {
    .struct_size = sizeof(yq_compress_opts),
    .algorithm = YQ_COMPRESS_ZSTD,
    .level = YQ_COMPRESS_LEVEL_MAX,
    .min_size = 512,   // Compress values > 512B
    .reserved = {0}
};
yq_compress_configure(db, &opts);
```

### Fast Compression
```c
// Configure fast compression for performance
yq_compress_opts opts = {
    .struct_size = sizeof(yq_compress_opts),
    .algorithm = YQ_COMPRESS_SNAPPY,
    .level = YQ_COMPRESS_LEVEL_FAST,
    .min_size = 2048,  // Compress values > 2KB
    .reserved = {0}
};
yq_compress_configure(db, &opts);
```

## Compression Algorithms

### Snappy
- **Speed**: Very fast compression/decompression
- **Compression Ratio**: Low (60-80%)
- **Memory Usage**: Low
- **Best For**: Real-time applications, speed-critical systems

### LZ4
- **Speed**: Fast compression/decompression
- **Compression Ratio**: Medium (50-70%)
- **Memory Usage**: Low
- **Best For**: General purpose, balanced performance

### Zstandard (Zstd)
- **Speed**: Good compression/decompression
- **Compression Ratio**: High (40-60%)
- **Memory Usage**: Medium
- **Best For**: Storage optimization, network transfer

### Zlib
- **Speed**: Medium compression/decompression
- **Compression Ratio**: Medium (50-70%)
- **Memory Usage**: Medium
- **Best For**: Compatibility, universal compression

## Use Cases

### Large Text Storage
```c
// Store large text documents
yq_slice doc_key = {"document_123", 12};
yq_slice doc_data = {large_text_ptr, large_text_size};  // 50KB
yq_put(txn, doc_key, doc_data, YQ_PUT_UPSERT);

// Document will be automatically compressed if > 1KB
```

### Binary Data Storage
```c
// Store large binary data
yq_slice binary_key = {"image_data", 10};
yq_slice binary_data = {image_ptr, image_size};  // 2MB
yq_put(txn, binary_key, binary_data, YQ_PUT_UPSERT);

// Binary data will be compressed to save space
```

### Log Data Compression
```c
// Configure compression for log data
yq_compress_opts log_opts = {
    .struct_size = sizeof(yq_compress_opts),
    .algorithm = YQ_COMPRESS_LZ4,
    .level = YQ_COMPRESS_LEVEL_NORMAL,
    .min_size = 512,  // Compress log entries > 512B
    .reserved = {0}
};
yq_compress_configure(db, &log_opts);

// Store log entries
for (int i = 0; i < 1000; i++) {
    yq_slice log_key = {"log_entry", 10};
    yq_slice log_data = {log_ptr, log_size};
    yq_put(txn, log_key, log_data, YQ_PUT_UPSERT);
}
```

### Monitoring Compression Effectiveness
```c
// Monitor compression statistics
int64_t compressed_count = yq_compress_stats(db);
int64_t saved_space = yq_compress_saved_space(db);
int64_t memory_usage = yq_compress_memory_usage(db);

printf("Compression Statistics:\n");
printf("  Compressed items: %ld\n", compressed_count);
printf("  Space saved: %ld bytes (%.1f%%)\n", 
       saved_space, 
       compressed_count > 0 ? (double)saved_space / (double)(compressed_count * 1024) * 100 : 0);
printf("  Memory usage: %ld bytes\n", memory_usage);

// Estimate compression for new data
size_t estimated_size = yq_compress_estimate_size(new_data, new_size, YQ_COMPRESS_ZSTD);
printf("Estimated compressed size: %zu bytes\n", estimated_size);
```

## Performance Considerations

### Compression Overhead
- **CPU Usage**: Compression consumes CPU cycles
- **Memory Usage**: Additional memory for compression buffers
- **Latency**: Increased latency for write operations

### Optimization Tips
1. **Choose appropriate algorithms** for your use case
2. **Set reasonable minimum sizes** to avoid overhead for small data
3. **Monitor compression effectiveness** regularly
4. **Consider hardware acceleration** if available

### Storage Efficiency
- **Large values benefit most** from compression
- **Text data compresses better** than binary data
- **Repeated patterns** achieve better compression ratios

## Error Handling

### Common Error Codes
- `YQ_ERR_INVAL`: Invalid parameters (NULL pointers, wrong size)
- `YQ_ERR_NOMEM`: Memory allocation failed
- `YQ_ERR_NOSUP`: Compression not supported
- `YQ_ERR_DATA`: Invalid compressed data

### Error Messages
Use `yq_compress_strerror()` to get human-readable error messages:

```c
int rc = yq_compress_data(data, size, level, &result);
if (rc != YQ_OK) {
    printf("Error compressing data: %s\n", yq_compress_strerror(rc));
}
```

## Building

To compile with compression support:

```bash
gcc -std=c11 -Iinclude -DYQ_ENABLE_COMPRESS -O2 -c src/*.c
ar rcs libyqdb.a *.o
```

Or with CMake:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DYQ_ENABLE_COMPRESS=..
cmake --build build
```

## Best Practices

1. **Choose Right Algorithm**: Balance between speed and compression ratio
2. **Set Appropriate Thresholds**: Avoid compression overhead for small data
3. **Monitor Performance**: Track compression effectiveness and resource usage
4. **Test Different Configurations**: Find optimal settings for your data
5. **Consider Hardware**: Use CPU acceleration if available
6. **Regular Maintenance**: Monitor compression statistics and adjust as needed

## Limitations

1. **Small Data Overhead**: Compression adds overhead for small values
2. **CPU Usage**: Compression consumes CPU resources
3. **Memory Requirements**: Additional memory for compression buffers
4. **Algorithm Availability**: Some algorithms may not be available on all systems
5. **Compression Ratio**: Limited by the compression algorithm used

## Troubleshooting

### Common Issues

**No Compression Happening**
- Check `min_size` configuration
- Verify data size exceeds minimum threshold
- Ensure compression is enabled

**Poor Compression Ratio**
- Try different algorithms (Zstd for better ratio)
- Adjust compression level
- Check data characteristics (binary vs text)

**Performance Issues**
- Use faster algorithms (Snappy, LZ4)
- Increase `min_size` to reduce overhead
- Consider hardware acceleration

### Debug Tips

```c
// Check compression configuration
yq_compress_opts config;
yq_compress_get_config(db, &config);
printf("Algorithm: %s, Level: %s, Min size: %u\n",
       yq_compress_algorithm_name(config.algorithm),
       yq_compress_level_name(config.level),
       config.min_size);

// Check algorithm availability
for (int i = 1; i < YQ_COMPRESS_MAX; i++) {
    if (yq_compress_is_available(i)) {
        printf("%s is available\n", yq_compress_algorithm_name(i));
    }
}
```