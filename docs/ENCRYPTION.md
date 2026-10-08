# Encryption for yq-DB

[English](ENCRYPTION.md) · [简体中文](ENCRYPTION.zh-CN.md)

**Encryption** functionality provides secure storage capabilities for yq-DB, protecting sensitive data with multiple encryption algorithms and key management.

## Overview

yq-DB supports automatic encryption of values using multiple encryption algorithms. This is essential for:

- **Data Security**: Protect sensitive information from unauthorized access
- **Compliance**: Meet regulatory requirements for data protection
- **Privacy**: Ensure user data remains confidential
- **Secure Storage**: Encrypt data at rest in the database

## Features

| Feature | Description | Benefits |
|---------|-------------|----------|
| **Multiple Algorithms** | Support for AES-256, ChaCha20, XOR encryption | Choose best algorithm for security/performance |
| **Automatic Encryption** | Configurable minimum size thresholds | No manual intervention required |
| **Key Management** | Key generation, derivation, and management | Secure key handling and rotation |
| **Encryption Modes** | ECB, CBC, GCM modes | Different security and performance characteristics |
| **Statistics & Monitoring** | Track encryption effectiveness | Monitor security metrics |
| **Hash Verification** | Data integrity verification | Ensure data hasn't been tampered with |

## Quick Start

```c
#include "yq.h"
#include "yq_crypto.h"

int main() {
    // Initialize database
    yq_db *db = NULL;
    yq_opts opts = {sizeof(yq_opts), 0, 0, 0, 0, 0, 0, 0, 0, {0}};
    yq_open("secure_database.yqdb", &opts, &db);
    
    // Configure encryption (AES-256, CBC mode, min 1KB)
    yq_crypto_opts crypto_opts = {
        .struct_size = sizeof(yq_crypto_opts),
        .algorithm = YQ_CRYPTO_AES256,
        .mode = YQ_CRYPTO_MODE_CBC,
        .key_size = 32,  // 256-bit key
        .iv_size = 16,   // 128-bit IV
        .min_size = 1024, // Only encrypt values > 1KB
        .reserved = {0}
    };
    yq_crypto_configure(db, &crypto_opts);
    
    // Store sensitive data (will be automatically encrypted)
    yq_txn *txn = NULL;
    yq_txn_begin(db, YQ_TXN_READWRITE, &txn);
    
    yq_slice key = {"user_password", 13};
    yq_slice value = {password_ptr, password_size};
    yq_put(txn, key, value, YQ_PUT_UPSERT);
    
    // Data is automatically encrypted if size > min_size
    yq_txn_commit(txn);
    
    // Check if data is encrypted
    int is_encrypted = yq_crypto_is_encrypted(txn, &key);
    printf("Data is %sencrypted\n", is_encrypted ? "" : "not ");
    
    // Get encryption statistics
    int64_t encrypted_count = yq_crypto_stats(db);
    int64_t key_count = yq_crypto_key_count(db);
    printf("Encrypted %ld items with %ld keys\n", encrypted_count, key_count);
    
    yq_close(db);
    return 0;
}
```

## API Reference

### Configuration

#### `yq_crypto_configure()`
Set encryption configuration.

```c
int yq_crypto_configure(yq_db *db, const yq_crypto_opts *opts);
```

**Parameters:**
- `db`: Database handle
- `opts`: Encryption configuration options

**Returns:** `YQ_OK` on success, error code on failure

**Configuration Options:**
- `algorithm`: Encryption algorithm (AES-256, ChaCha20, XOR)
- `mode`: Encryption mode (ECB, CBC, GCM)
- `key_size`: Key size in bytes
- `iv_size`: Initialization vector size in bytes
- `min_size`: Minimum size for encryption (bytes)

#### `yq_crypto_get_config()`
Get current encryption configuration.

```c
int yq_crypto_get_config(yq_db *db, yq_crypto_opts *opts);
```

**Parameters:**
- `db`: Database handle
- `opts`: Output parameter for configuration

**Returns:** `YQ_OK` on success, error code on failure

### Key Management

#### `yq_crypto_generate_key()`
Generate random key.

```c
uint8_t *yq_crypto_generate_key(size_t size);
```

**Parameters:**
- `size`: Key size in bytes

**Returns:** Generated key, caller must free

#### `yq_crypto_derive_key()`
Derive key from password using PBKDF2.

```c
uint8_t *yq_crypto_derive_key(const char *password, size_t size, const uint8_t *salt, size_t salt_size, uint32_t iterations);
```

**Parameters:**
- `password`: Password string
- `size`: Key size in bytes
- `salt`: Salt value
- `salt_size`: Salt size in bytes
- `iterations`: PBKDF2 iterations

**Returns:** Derived key, caller must free

#### `yq_crypto_generate_salt()`
Generate random salt.

```c
uint8_t *yq_crypto_generate_salt(size_t size);
```

**Parameters:**
- `size`: Salt size in bytes

**Returns:** Generated salt, caller must free

#### `yq_crypto_generate_iv()`
Generate random initialization vector.

```c
uint8_t *yq_crypto_generate_iv(size_t size);
```

**Parameters:**
- `size`: IV size in bytes

**Returns:** Generated IV, caller must free

### Encryption Operations

#### `yq_crypto_encrypt()`
Encrypt data manually.

```c
int yq_crypto_encrypt(const void *data, size_t size, const uint8_t *key, const uint8_t *iv, yq_crypto_result **result);
```

**Parameters:**
- `data`: Data to encrypt
- `size`: Size of data
- `key`: Encryption key
- `iv`: Initialization vector
- `result`: Output parameter for encryption results

**Returns:** `YQ_OK` on success, error code on failure

#### `yq_crypto_decrypt()`
Decrypt data manually.

```c
int yq_crypto_decrypt(const void *data, size_t size, const uint8_t *key, const uint8_t *iv, yq_crypto_result **result);
```

**Parameters:**
- `data`: Data to decrypt
- `size`: Size of encrypted data
- `key`: Decryption key
- `iv`: Initialization vector
- `result`: Output parameter for decryption results

**Returns:** `YQ_OK` on success, error code on failure

#### `yq_crypto_is_encrypted()`
Check if data is encrypted.

```c
int yq_crypto_is_encrypted(yq_txn *txn, const yq_slice *key);
```

**Parameters:**
- `txn`: Transaction handle
- `key`: Key to check

**Returns:**
- `1`: Data is encrypted
- `0`: Data is not encrypted
- `-1`: Error

### Statistics

#### `yq_crypto_stats()`
Get number of encrypted items.

```c
int64_t yq_crypto_stats(yq_db *db);
```

**Parameters:**
- `db`: Database handle

**Returns:** Number of encrypted items, or `-1` on error

#### `yq_crypto_memory_usage()`
Get memory usage of encryption system.

```c
int64_t yq_crypto_memory_usage(yq_db *db);
```

**Parameters:**
- `db`: Database handle

**Returns:** Memory usage in bytes, or `-1` on error

#### `yq_crypto_key_count()`
Get number of keys.

```c
int64_t yq_crypto_key_count(yq_db *db);
```

**Parameters:**
- `db`: Database handle

**Returns:** Number of keys, or `-1` on error

### Utility Functions

#### `yq_crypto_result_free()`
Free encryption results.

```c
void yq_crypto_result_free(yq_crypto_result *result);
```

**Parameters:**
- `result`: Encryption results to free

#### `yq_crypto_algorithm_name()`
Get algorithm name.

```c
const char *yq_crypto_algorithm_name(yq_crypto_algorithm algorithm);
```

**Parameters:**
- `algorithm`: Encryption algorithm

**Returns:** Algorithm name string

#### `yq_crypto_mode_name()`
Get encryption mode name.

```c
const char *yq_crypto_mode_name(yq_crypto_mode mode);
```

**Parameters:**
- `mode`: Encryption mode

**Returns:** Mode name string

#### `yq_crypto_estimate_size()`
Estimate encrypted size.

```c
size_t yq_crypto_estimate_size(const void *data, size_t size, yq_crypto_algorithm algorithm);
```

**Parameters:**
- `data`: Data to estimate
- `size`: Size of data
- `algorithm`: Encryption algorithm

**Returns:** Estimated encrypted size, or `0` on error

#### `yq_crypto_is_available()`
Check if algorithm is available.

```c
int yq_crypto_is_available(yq_crypto_algorithm algorithm);
```

**Parameters:**
- `algorithm`: Encryption algorithm

**Returns:** `1` if available, `0` if not

#### `yq_crypto_hash()`
Calculate hash of data.

```c
uint8_t *yq_crypto_hash(const void *data, size_t size, const char *algorithm);
```

**Parameters:**
- `data`: Data to hash
- `size`: Size of data
- `algorithm`: Hash algorithm

**Returns:** Hash value, caller must free

#### `yq_crypto_verify()`
Verify data integrity.

```c
int yq_crypto_verify(const void *data, size_t size, const uint8_t *hash, const char *algorithm);
```

**Parameters:**
- `data`: Data to verify
- `size`: Size of data
- `hash`: Expected hash
- `algorithm`: Hash algorithm

**Returns:** `1` if verification successful, `0` if failed

## Configuration Examples

### Basic Encryption
```c
// Configure basic AES-256 encryption
yq_crypto_opts opts = {
    .struct_size = sizeof(yq_crypto_opts),
    .algorithm = YQ_CRYPTO_AES256,
    .mode = YQ_CRYPTO_MODE_CBC,
    .key_size = 32,  // 256-bit key
    .iv_size = 16,   // 128-bit IV
    .min_size = 1024, // Encrypt values > 1KB
    .reserved = {0}
};
yq_crypto_configure(db, &opts);
```

### High Security
```c
// Configure maximum security with ChaCha20
yq_crypto_opts opts = {
    .struct_size = sizeof(yq_crypto_opts),
    .algorithm = YQ_CRYPTO_CHACHA20,
    .mode = YQ_CRYPTO_MODE_GCM,
    .key_size = 32,  // 256-bit key
    .iv_size = 12,   // 96-bit IV
    .min_size = 512, // Encrypt values > 512B
    .reserved = {0}
};
yq_crypto_configure(db, &opts);
```

### Password-Based Encryption
```c
// Configure password-based encryption
yq_crypto_opts opts = {
    .struct_size = sizeof(yq_crypto_opts),
    .algorithm = YQ_CRYPTO_AES256,
    .mode = YQ_CRYPTO_MODE_CBC,
    .key_size = 32,
    .iv_size = 16,
    .min_size = 256, // Encrypt values > 256B
    .reserved = {0}
};
yq_crypto_configure(db, &opts);

// Generate key from password
uint8_t *salt = yq_crypto_generate_salt(16);
uint8_t *key = yq_crypto_derive_key("user_password", 32, salt, 16, 10000);
free(salt);

// Use key for encryption
yq_crypto_result *result = NULL;
yq_crypto_encrypt(data, size, key, NULL, &result);
free(key);
```

## Encryption Algorithms

### AES-256
- **Security**: Very high (256-bit key)
- **Speed**: Good performance
- **Modes**: ECB, CBC, GCM
- **Best For**: General purpose encryption, high security requirements

### ChaCha20
- **Security**: High (256-bit key)
- **Speed**: Excellent performance
- **Modes**: Stream cipher
- **Best For**: Performance-critical applications, mobile devices

### XOR
- **Security**: Low (for demonstration only)
- **Speed**: Very fast
- **Modes**: Simple XOR
- **Best For**: Educational purposes, lightweight encryption

## Use Cases

### Sensitive Data Storage
```c
// Store user passwords with encryption
yq_slice password_key = {"user_123_password", 19};
yq_slice password_data = {password_ptr, password_size};  // 64B
yq_put(txn, password_key, password_data, YQ_PUT_UPSERT);

// Password will be automatically encrypted if > 256B
```

### Secure Configuration
```c
// Configure for secure configuration storage
yq_crypto_opts config_opts = {
    .struct_size = sizeof(yq_crypto_opts),
    .algorithm = YQ_CRYPTO_AES256,
    .mode = YQ_CRYPTO_MODE_GCM,
    .key_size = 32,
    .iv_size = 12,
    .min_size = 0,  // Encrypt all data
    .reserved = {0}
};
yq_crypto_configure(db, &config_opts);

// Store configuration data
yq_slice config_key = {"api_key", 7};
yq_slice config_data = {api_key_ptr, api_key_size};
yq_put(txn, config_key, config_data, YQ_PUT_UPSERT);
```

### Data Integrity Verification
```c
// Generate hash for data integrity
uint8_t *data_hash = yq_crypto_hash(sensitive_data, data_size, "SHA256");

// Store data with hash
yq_slice data_key = {"sensitive_data", 13};
yq_slice data_value = {sensitive_data, data_size};
yq_put(txn, data_key, data_value, YQ_PUT_UPSERT);

// Later verify data integrity
int is_valid = yq_crypto_verify(retrieved_data, data_size, data_hash, "SHA256");
if (!is_valid) {
    printf("Data integrity check failed!\n");
}
free(data_hash);
```

### Key Management
```c
// Generate secure keys
uint8_t *master_key = yq_crypto_generate_key(32);  // 256-bit key
uint8_t *salt = yq_crypto_generate_salt(16);

// Derive user-specific keys
uint8_t *user_key = yq_crypto_derive_key("user_password", 32, salt, 16, 10000);

// Use keys for encryption
yq_crypto_result *result = NULL;
yq_crypto_encrypt(user_data, data_size, user_key, NULL, &result);

// Cleanup
free(master_key);
free(salt);
free(user_key);
yq_crypto_result_free(result);
```

## Performance Considerations

### Encryption Overhead
- **CPU Usage**: Encryption consumes CPU cycles
- **Memory Usage**: Additional memory for encryption buffers
- **Latency**: Increased latency for write operations
- **Storage Overhead**: Encrypted data may be slightly larger

### Optimization Tips
1. **Choose appropriate algorithms** for your security/performance needs
2. **Set reasonable minimum sizes** to avoid overhead for small data
3. **Use hardware acceleration** if available
4. **Cache decrypted data** when possible
5. **Consider key rotation** for long-term security

### Security Best Practices
1. **Use strong algorithms** (AES-256, ChaCha20)
2. **Implement proper key management**
3. **Use unique IVs for each encryption**
4. **Store keys securely** separately from data
5. **Regular key rotation** for sensitive data

## Error Handling

### Common Error Codes
- `YQ_ERR_INVAL`: Invalid parameters (NULL pointers, wrong size)
- `YQ_ERR_NOMEM`: Memory allocation failed
- `YQ_ERR_NOTSUP`: Algorithm not supported
- `YQ_ERR_CRYPTO`: Encryption/decryption failed
- `YQ_ERR_KEY`: Invalid key
- `YQ_ERR_IV`: Invalid initialization vector

### Error Messages
Use `yq_crypto_strerror()` to get human-readable error messages:

```c
int rc = yq_crypto_encrypt(data, size, key, iv, &result);
if (rc != YQ_OK) {
    printf("Error encrypting data: %s\n", yq_crypto_strerror(rc));
}
```

## Building

To compile with encryption support:

```bash
gcc -std=c11 -Iinclude -DYQ_ENABLE_CRYPTO -O2 -c src/*.c
ar rcs libyqdb.a *.o
```

Or with CMake:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DYQ_ENABLE_CRYPTO=..
cmake --build build
```

## Best Practices

1. **Choose Right Algorithm**: Balance between security and performance
2. **Set Appropriate Thresholds**: Avoid encryption overhead for small data
3. **Monitor Performance**: Track encryption metrics and resource usage
4. **Secure Key Management**: Store keys separately from encrypted data
5. **Regular Key Rotation**: Rotate keys for sensitive data
6. **Use Strong Passwords**: Use strong passwords for key derivation
7. **Test Security**: Regularly test encryption effectiveness

## Limitations

1. **Small Data Overhead**: Encryption adds overhead for small values
2. **CPU Usage**: Encryption consumes CPU resources
3. **Memory Requirements**: Additional memory for encryption buffers
4. **Algorithm Availability**: Some algorithms may not be available on all systems
5. **Export Restrictions**: Some encryption algorithms may have export restrictions

## Troubleshooting

### Common Issues

**No Encryption Happening**
- Check `min_size` configuration
- Verify data size exceeds minimum threshold
- Ensure encryption is enabled

**Performance Issues**
- Use faster algorithms (ChaCha20)
- Increase `min_size` to reduce overhead
- Consider hardware acceleration

**Security Concerns**
- Use stronger algorithms (AES-256)
- Implement proper key management
- Use unique IVs for each encryption

### Debug Tips

```c
// Check encryption configuration
yq_crypto_opts config;
yq_crypto_get_config(db, &config);
printf("Algorithm: %s, Mode: %s, Key size: %u\n",
       yq_crypto_algorithm_name(config.algorithm),
       yq_crypto_mode_name(config.mode),
       config.key_size);

// Check algorithm availability
for (int i = 1; i < YQ_CRYPTO_MAX; i++) {
    if (yq_crypto_is_available(i)) {
        printf("%s is available\n", yq_crypto_algorithm_name(i));
    }
}

// Monitor encryption statistics
int64_t encrypted_count = yq_crypto_stats(db);
int64_t memory_usage = yq_crypto_memory_usage(db);
printf("Encrypted items: %ld, Memory: %ld bytes\n", encrypted_count, memory_usage);
```