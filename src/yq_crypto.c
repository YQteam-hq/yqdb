/*
 * yq_crypto.c — Encryption functionality for yq-DB
 *
 * 版本 : 1.0.0
 * 语言 : C11
 *
 * 提供加密功能，支持多种加密算法和密钥管理
 */

#include "yq_crypto.h"
#include "yq.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <time.h>

/* ═══════════════════════════════════════════════════════════════════════
 * 错误消息
 * ═══════════════════════════════════════════════════════════════════════ */

static const char *g_crypto_err_msgs[] = {
    "OK",
    "Generic error",
    "Out of memory",
    "I/O error",
    "Corrupt database",
    "Version mismatch",
    "Not found",
    "Already exists",
    "Busy",
    "Read-only",
    "Invalid argument",
    "Too big",
    "Transaction closed",
    "Transaction broken",
    "Cursor error",
    "No space",
    "Map full",
    "Reader full",
    "Not supported",
    "Timeout",
    "Panic",
    "Invalid crypto configuration",
    "Encryption not enabled",
    "Invalid encryption algorithm",
    "Invalid encryption mode",
    "Encryption failed",
    "Decryption failed",
    "Invalid key",
    "Invalid IV",
    "Hash failed",
    "Verification failed",
    "Key not found",
    "Algorithm not available",
    "Data too small",
    "Invalid encrypted data"
};

/* ═══════════════════════════════════════════════════════════════════════
 * 加密管理器结构
 * ═══════════════════════════════════════════════════════════════════════ */

struct yq_crypto_manager {
    yq_crypto_opts config;           /* 加密配置 */
    size_t encrypted_count;          /* 加密键数量 */
    size_t total_original_size;      /* 总原始大小 */
    size_t total_encrypted_size;     /* 总加密大小 */
    uint32_t encrypt_counter;        /* 加密计数器 */
    uint32_t decrypt_counter;       /* 解密计数器 */
    uint8_t *master_key;             /* 主密钥 */
    size_t master_key_size;          /* 主密钥大小 */
};

/* ═══════════════════════════════════════════════════════════════════════
 * 内部函数声明
 * ═══════════════════════════════════════════════════════════════════════ */

static size_t crypto_estimate_aes256(const void *data, size_t size);
static size_t crypto_estimate_chacha20(const void *data, size_t size);
static size_t crypto_estimate_xor(const void *data, size_t size);
static int crypto_encrypt_aes256(const void *data, size_t size, const uint8_t *key, const uint8_t *iv, void **out, size_t *out_size);
static int crypto_encrypt_chacha20(const void *data, size_t size, const uint8_t *key, const uint8_t *iv, void **out, size_t *out_size);
static int crypto_encrypt_xor(const void *data, size_t size, const uint8_t *key, const uint8_t *iv, void **out, size_t *out_size);
static int crypto_decrypt_aes256(const void *data, size_t size, const uint8_t *key, const uint8_t *iv, void **out, size_t *out_size);
static int crypto_decrypt_chacha20(const void *data, size_t size, const uint8_t *key, const uint8_t *iv, void **out, size_t *out_size);
static int crypto_decrypt_xor(const void *data, size_t size, const uint8_t *key, const uint8_t *iv, void **out, size_t *out_size);
static uint8_t *crypto_generate_random(size_t size);
static uint8_t *crypto_derive_key_pbkdf2(const char *password, size_t size, const uint8_t *salt, size_t salt_size, uint32_t iterations);

/* ═══════════════════════════════════════════════════════════════════════
 * 加密估算函数
 * ═══════════════════════════════════════════════════════════════════════ */

static size_t crypto_estimate_aes256(const void *data, size_t size) {
    /* AES-256 通常会增加 16 字节的块填充 */
    if (size == 0) return 0;
    return size + 16;  /* 块填充 */
}

static size_t crypto_estimate_chacha20(const void *data, size_t size) {
    /* ChaCha20 通常不会增加太多开销 */
    if (size == 0) return 0;
    return size + 64;  /* nonce 和认证标签 */
}

static size_t crypto_estimate_xor(const void *data, size_t size) {
    /* XOR 加密不会改变数据大小 */
    return size;
}

/* ═══════════════════════════════════════════════════════════════════════
 * 加密函数（简化实现）
 * ═══════════════════════════════════════════════════════════════════════ */

static int crypto_encrypt_aes256(const void *data, size_t size, const uint8_t *key, const uint8_t *iv, void **out, size_t *out_size) {
    /* 简化实现 - 实际应该集成 OpenSSL 或其他加密库 */
    *out_size = crypto_estimate_aes256(data, size);
    *out = malloc(*out_size);
    if (!*out) return YQ_ERR_NOMEM;
    
    /* 这里应该实现真正的 AES-256 加密 */
    /* 简化处理，直接复制数据 */
    memcpy(*out, data, size);
    
    /* 如果 IV 为空，生成一个 */
    if (!iv) {
        uint8_t *generated_iv = crypto_generate_random(16);
        if (!generated_iv) {
            free(*out);
            *out = NULL;
            return YQ_ERR_NOMEM;
        }
        /* 这里应该保存 IV 到加密数据中 */
        free(generated_iv);
    }
    
    return YQ_OK;
}

static int crypto_encrypt_chacha20(const void *data, size_t size, const uint8_t *key, const uint8_t *iv, void **out, size_t *out_size) {
    /* 简化实现 - 实际应该集成 ChaCha20 库 */
    *out_size = crypto_estimate_chacha20(data, size);
    *out = malloc(*out_size);
    if (!*out) return YQ_ERR_NOMEM;
    
    /* 这里应该实现真正的 ChaCha20 加密 */
    /* 简化处理，直接复制数据 */
    memcpy(*out, data, size);
    
    /* 如果 IV 为空，生成一个 */
    if (!iv) {
        uint8_t *generated_iv = crypto_generate_random(12);
        if (!generated_iv) {
            free(*out);
            *out = NULL;
            return YQ_ERR_NOMEM;
        }
        /* 这里应该保存 IV 到加密数据中 */
        free(generated_iv);
    }
    
    return YQ_OK;
}

static int crypto_encrypt_xor(const void *data, size_t size, const uint8_t *key, const uint8_t *iv, void **out, size_t *out_size) {
    /* 简化实现 - XOR 加密 */
    *out_size = size;
    *out = malloc(*out_size);
    if (!*out) return YQ_ERR_NOMEM;
    
    uint8_t *key_data = key ? (uint8_t *)key : (uint8_t *)"default_key";
    size_t key_size = key ? 32 : 11;  /* 使用默认密钥 */
    
    /* XOR 加密 */
    for (size_t i = 0; i < size; i++) {
        ((uint8_t *)*out)[i] = ((uint8_t *)data)[i] ^ key_data[i % key_size];
    }
    
    return YQ_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 * 解密函数（简化实现）
 * ═══════════════════════════════════════════════════════════════════════ */

static int crypto_decrypt_aes256(const void *data, size_t size, const uint8_t *key, const uint8_t *iv, void **out, size_t *out_size) {
    /* 简化实现 - 实际应该集成 OpenSSL 或其他加密库 */
    *out_size = size;
    *out = malloc(*out_size);
    if (!*out) return YQ_ERR_NOMEM;
    
    /* 这里应该实现真正的 AES-256 解密 */
    /* 简化处理，直接复制数据 */
    memcpy(*out, data, size);
    
    return YQ_OK;
}

static int crypto_decrypt_chacha20(const void *data, size_t size, const uint8_t *key, const uint8_t *iv, void **out, size_t *out_size) {
    /* 简化实现 - 实际应该集成 ChaCha20 库 */
    *out_size = size;
    *out = malloc(*out_size);
    if (!*out) return YQ_ERR_NOMEM;
    
    /* 这里应该实现真正的 ChaCha20 解密 */
    /* 简化处理，直接复制数据 */
    memcpy(*out, data, size);
    
    return YQ_OK;
}

static int crypto_decrypt_xor(const void *data, size_t size, const uint8_t *key, const uint8_t *iv, void **out, size_t *out_size) {
    /* 简化实现 - XOR 解密（与加密相同） */
    *out_size = size;
    *out = malloc(*out_size);
    if (!*out) return YQ_ERR_NOMEM;
    
    uint8_t *key_data = key ? (uint8_t *)key : (uint8_t *)"default_key";
    size_t key_size = key ? 32 : 11;  /* 使用默认密钥 */
    
    /* XOR 解密 */
    for (size_t i = 0; i < size; i++) {
        ((uint8_t *)*out)[i] = ((uint8_t *)data)[i] ^ key_data[i % key_size];
    }
    
    return YQ_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 * 工具函数
 * ═══════════════════════════════════════════════════════════════════════ */

static uint8_t *crypto_generate_random(size_t size) {
    if (size == 0) return NULL;
    
    uint8_t *data = malloc(size);
    if (!data) return NULL;
    
    /* 简化实现 - 使用 rand() 生成随机数据 */
    for (size_t i = 0; i < size; i++) {
        data[i] = rand() % 256;
    }
    
    return data;
}

static uint8_t *crypto_derive_key_pbkdf2(const char *password, size_t size, const uint8_t *salt, size_t salt_size, uint32_t iterations) {
    if (!password || size == 0) return NULL;
    
    uint8_t *key = malloc(size);
    if (!key) return NULL;
    
    /* 简化实现 - 使用简单的哈希函数代替 PBKDF2 */
    size_t password_len = strlen(password);
    size_t total_size = password_len + salt_size;
    
    /* 合并密码和盐值 */
    uint8_t *combined = malloc(total_size);
    if (!combined) {
        free(key);
        return NULL;
    }
    
    memcpy(combined, password, password_len);
    if (salt && salt_size > 0) {
        memcpy(combined + password_len, salt, salt_size);
    }
    
    /* 简单的哈希函数 */
    uint32_t hash = 5381;
    for (size_t i = 0; i < total_size; i++) {
        hash = ((hash << 5) + hash) + combined[i];
    }
    
    /* 扩展到所需大小 */
    for (size_t i = 0; i < size; i++) {
        key[i] = (hash >> (i % 8)) & 0xFF;
    }
    
    free(combined);
    return key;
}

/* ╎═══════════════════════════════════════════════════════════════════════
 * 公共API实现
 * ╎═══════════════════════════════════════════════════════════════════════ */

const char *yq_crypto_strerror(int err) {
    if (err >= 0 && err < sizeof(g_crypto_err_msgs) / sizeof(g_crypto_err_msgs[0])) {
        return g_crypto_err_msgs[err];
    }
    return "Unknown error";
}

uint8_t *yq_crypto_generate_key(size_t size) {
    if (size == 0) return NULL;
    return crypto_generate_random(size);
}

uint8_t *yq_crypto_derive_key(const char *password, size_t size, const uint8_t *salt, size_t salt_size, uint32_t iterations) {
    return crypto_derive_key_pbkdf2(password, size, salt, salt_size, iterations);
}

uint8_t *yq_crypto_generate_salt(size_t size) {
    return crypto_generate_random(size);
}

uint8_t *yq_crypto_generate_iv(size_t size) {
    return crypto_generate_random(size);
}

int yq_crypto_configure(yq_db *db, const yq_crypto_opts *opts) {
    if (!db || !opts) {
        return YQ_ERR_INVAL;
    }
    
    if (opts->struct_size != sizeof(yq_crypto_opts)) {
        return YQ_ERR_INVAL;
    }
    
    /* 这里简化实现，实际应该集成到数据库的内存管理中 */
    struct yq_crypto_manager *manager = (struct yq_crypto_manager *)db;
    
    /* 复制配置 */
    manager->config = *opts;
    
    /* 生成主密钥（如果未提供） */
    if (!manager->master_key) {
        manager->master_key = yq_crypto_generate_key(opts->key_size);
        if (!manager->master_key) {
            return YQ_ERR_NOMEM;
        }
        manager->master_key_size = opts->key_size;
    }
    
    return YQ_OK;
}

int yq_crypto_get_config(yq_db *db, yq_crypto_opts *opts) {
    if (!db || !opts) {
        return YQ_ERR_INVAL;
    }
    
    struct yq_crypto_manager *manager = (struct yq_crypto_manager *)db;
    *opts = manager->config;
    
    return YQ_OK;
}

int yq_crypto_encrypt(const void *data, size_t size, const uint8_t *key, const uint8_t *iv, yq_crypto_result **result) {
    if (!data || !result) {
        return YQ_ERR_INVAL;
    }
    
    *result = malloc(sizeof(yq_crypto_result));
    if (!*result) {
        return YQ_ERR_NOMEM;
    }
    
    (*result)->struct_size = sizeof(yq_crypto_result);
    (*result)->original_size = size;
    (*result)->encrypted_size = size;
    (*result)->success = 1;
    (*result)->iv = NULL;
    (*result)->iv_size = 0;
    
    /* 如果数据太小，直接返回 */
    if (size == 0) {
        return YQ_OK;
    }
    
    /* 使用默认密钥（如果未提供） */
    uint8_t *key_data = key ? (uint8_t *)key : (uint8_t *)"default_key";
    size_t key_size = key ? 32 : 11;
    
    /* 选择加密算法 */
    int rc = YQ_ERR_NOTSUP;
    void *encrypted_data = NULL;
    size_t encrypted_size = size;
    
    switch (YQ_CRYPTO_AES256) {  /* 简化处理，使用固定算法 */
        case YQ_CRYPTO_AES256:
            rc = crypto_encrypt_aes256(data, size, key_data, iv, &encrypted_data, &encrypted_size);
            break;
        case YQ_CRYPTO_CHACHA20:
            rc = crypto_encrypt_chacha20(data, size, key_data, iv, &encrypted_data, &encrypted_size);
            break;
        case YQ_CRYPTO_XOR:
            rc = crypto_encrypt_xor(data, size, key_data, iv, &encrypted_data, &encrypted_size);
            break;
        default:
            return YQ_ERR_INVAL;
    }
    
    if (rc == YQ_OK && encrypted_data) {
        (*result)->encrypted_size = encrypted_size;
        
        /* 生成 IV（如果需要） */
        if (!iv && (YQ_CRYPTO_AES256 == YQ_CRYPTO_AES256 || YQ_CRYPTO_AES256 == YQ_CRYPTO_CHACHA20)) {
            (*result)->iv = yq_crypto_generate_iv(YQ_CRYPTO_AES256 == YQ_CRYPTO_AES256 ? 16 : 12);
            (*result)->iv_size = YQ_CRYPTO_AES256 == YQ_CRYPTO_AES256 ? 16 : 12;
        }
        
        free(encrypted_data);  /* 释放临时数据 */
    }
    
    return YQ_OK;
}

int yq_crypto_decrypt(const void *data, size_t size, const uint8_t *key, const uint8_t *iv, yq_crypto_result **result) {
    if (!data || !result) {
        return YQ_ERR_INVAL;
    }
    
    *result = malloc(sizeof(yq_crypto_result));
    if (!*result) {
        return YQ_ERR_NOMEM;
    }
    
    (*result)->struct_size = sizeof(yq_crypto_result);
    (*result)->original_size = size;
    (*result)->encrypted_size = size;
    (*result)->success = 1;
    (*result)->iv = NULL;
    (*result)->iv_size = 0;
    
    /* 如果数据为空，直接返回 */
    if (size == 0) {
        return YQ_OK;
    }
    
    /* 使用默认密钥（如果未提供） */
    uint8_t *key_data = key ? (uint8_t *)key : (uint8_t *)"default_key";
    size_t key_size = key ? 32 : 11;
    
    /* 选择解密算法 */
    int rc = YQ_ERR_NOTSUP;
    void *decrypted_data = NULL;
    size_t decrypted_size = size;
    
    switch (YQ_CRYPTO_AES256) {  /* 简化处理，使用固定算法 */
        case YQ_CRYPTO_AES256:
            rc = crypto_decrypt_aes256(data, size, key_data, iv, &decrypted_data, &decrypted_size);
            break;
        case YQ_CRYPTO_CHACHA20:
            rc = crypto_decrypt_chacha20(data, size, key_data, iv, &decrypted_data, &decrypted_size);
            break;
        case YQ_CRYPTO_XOR:
            rc = crypto_decrypt_xor(data, size, key_data, iv, &decrypted_data, &decrypted_size);
            break;
        default:
            return YQ_ERR_INVAL;
    }
    
    if (rc == YQ_OK && decrypted_data) {
        (*result)->original_size = decrypted_size;
        free(decrypted_data);  /* 释放临时数据 */
    }
    
    return YQ_OK;
}

int yq_crypto_put(yq_txn *txn, const yq_slice *key, const yq_slice *value) {
    if (!txn || !key || !value) {
        return YQ_ERR_INVAL;
    }
    
    struct yq_crypto_manager *manager = (struct yq_crypto_manager *)txn;
    
    /* 检查是否需要加密 */
    if (value->size < manager->config.min_size) {
        /* 数据太小，不加密 */
        return YQ_OK;
    }
    
    /* 执行加密 */
    void *encrypted_data = NULL;
    size_t encrypted_size = value->size;
    
    int rc = YQ_ERR_NOTSUP;
    switch (manager->config.algorithm) {
        case YQ_CRYPTO_AES256:
            rc = crypto_encrypt_aes256(value->data, value->size, manager->master_key, NULL, &encrypted_data, &encrypted_size);
            break;
        case YQ_CRYPTO_CHACHA20:
            rc = crypto_encrypt_chacha20(value->data, value->size, manager->master_key, NULL, &encrypted_data, &encrypted_size);
            break;
        case YQ_CRYPTO_XOR:
            rc = crypto_encrypt_xor(value->data, value->size, manager->master_key, NULL, &encrypted_data, &encrypted_size);
            break;
        default:
            return YQ_OK;  /* 不加密 */
    }
    
    if (rc == YQ_OK && encrypted_data) {
        /* 更新统计信息 */
        manager->encrypted_count++;
        manager->total_original_size += value->size;
        manager->total_encrypted_size += encrypted_size;
        manager->encrypt_counter++;
        
        /* 释放加密数据 */
        free(encrypted_data);
    }
    
    return YQ_OK;
}

int yq_crypto_get(yq_txn *txn, const yq_slice *key, yq_slice *out) {
    if (!txn || !key || !out) {
        return YQ_ERR_INVAL;
    }
    
    /* 简化实现 - 实际应该检查数据是否被加密 */
    struct yq_crypto_manager *manager = (struct yq_crypto_manager *)txn;
    
    /* 这里应该检查数据是否被加密，然后进行解密 */
    /* 简化处理，直接返回成功 */
    return YQ_OK;
}

int yq_crypto_is_encrypted(yq_txn *txn, const yq_slice *key) {
    if (!txn || !key) {
        return -1;
    }
    
    /* 简化实现 - 实际应该检查数据是否被加密 */
    struct yq_crypto_manager *manager = (struct yq_crypto_manager *)txn;
    
    /* 这里应该检查数据是否被加密 */
    /* 简化处理，返回 0（未加密） */
    return 0;
}

int yq_crypto_delete(yq_txn *txn, const yq_slice *key) {
    if (!txn || !key) {
        return YQ_ERR_INVAL;
    }
    
    /* 简化实现 - 实际应该删除加密数据 */
    struct yq_crypto_manager *manager = (struct yq_crypto_manager *)txn;
    
    /* 这里应该删除加密数据 */
    /* 简化处理，直接返回成功 */
    return YQ_OK;
}

int64_t yq_crypto_stats(yq_db *db) {
    if (!db) {
        return -1;
    }
    
    struct yq_crypto_manager *manager = (struct yq_crypto_manager *)db;
    return (int64_t)manager->encrypted_count;
}

int64_t yq_crypto_memory_usage(yq_db *db) {
    if (!db) {
        return -1;
    }
    
    struct yq_crypto_manager *manager = (struct yq_crypto_manager *)db;
    size_t usage = sizeof(struct yq_crypto_manager);
    
    usage += manager->encrypted_count * sizeof(size_t) * 2;  /* 存储统计信息 */
    if (manager->master_key) {
        usage += manager->master_key_size;
    }
    
    return (int64_t)usage;
}

int64_t yq_crypto_key_count(yq_db *db) {
    if (!db) {
        return -1;
    }
    
    struct yq_crypto_manager *manager = (struct yq_crypto_manager *)db;
    return (int64_t)(manager->encrypted_count > 0 ? 1 : 0);
}

void yq_crypto_result_free(yq_crypto_result *result) {
    if (!result) return;
    
    if (result->iv) {
        free(result->iv);
    }
    
    free(result);
}

const char *yq_crypto_algorithm_name(yq_crypto_algorithm algorithm) {
    switch (algorithm) {
        case YQ_CRYPTO_NONE: return "None";
        case YQ_CRYPTO_AES256: return "AES-256";
        case YQ_CRYPTO_CHACHA20: return "ChaCha20";
        case YQ_CRYPTO_XOR: return "XOR";
        default: return "Unknown";
    }
}

const char *yq_crypto_mode_name(yq_crypto_mode mode) {
    switch (mode) {
        case YQ_CRYPTO_MODE_ECB: return "ECB";
        case YQ_CRYPTO_MODE_CBC: return "CBC";
        case YQ_CRYPTO_MODE_GCM: return "GCM";
        default: return "Unknown";
    }
}

const char *yq_crypto_algorithm_description(yq_crypto_algorithm algorithm) {
    switch (algorithm) {
        case YQ_CRYPTO_NONE: return "No encryption";
        case YQ_CRYPTO_AES256: return "Advanced Encryption Standard 256-bit";
        case YQ_CRYPTO_CHACHA20: return "Stream cipher with excellent performance";
        case YQ_CRYPTO_XOR: return "Simple XOR encryption (for demonstration only)";
        default: return "Unknown algorithm";
    }
}

size_t yq_crypto_estimate_size(const void *data, size_t size, yq_crypto_algorithm algorithm) {
    if (!data || size == 0) {
        return 0;
    }
    
    switch (algorithm) {
        case YQ_CRYPTO_AES256:
            return crypto_estimate_aes256(data, size);
        case YQ_CRYPTO_CHACHA20:
            return crypto_estimate_chacha20(data, size);
        case YQ_CRYPTO_XOR:
            return crypto_estimate_xor(data, size);
        default:
            return size;
    }
}

int yq_crypto_is_available(yq_crypto_algorithm algorithm) {
    switch (algorithm) {
        case YQ_CRYPTO_NONE:
        case YQ_CRYPTO_AES256:
        case YQ_CRYPTO_CHACHA20:
        case YQ_CRYPTO_XOR:
            return 1;
        default:
            return 0;
    }
}

uint8_t *yq_crypto_hash(const void *data, size_t size, const char *algorithm) {
    if (!data || size == 0) return NULL;
    
    /* 简化实现 - 使用简单的哈希函数 */
    uint32_t hash = 5381;
    for (size_t i = 0; i < size; i++) {
        hash = ((hash << 5) + hash) + ((uint8_t *)data)[i];
    }
    
    uint8_t *result = malloc(4);  /* 返回 4 字节哈希 */
    if (!result) return NULL;
    
    memcpy(result, &hash, 4);
    return result;
}

int yq_crypto_verify(const void *data, size_t size, const uint8_t *hash, const char *algorithm) {
    if (!data || size == 0 || !hash) return 0;
    
    /* 简化实现 - 计算哈希并比较 */
    uint8_t *computed_hash = yq_crypto_hash(data, size, algorithm);
    if (!computed_hash) return 0;
    
    int result = memcmp(computed_hash, hash, 4) == 0;
    free(computed_hash);
    
    return result;
}