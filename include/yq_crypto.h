/*
 * yq_crypto.h — Encryption functionality for yq-DB
 *
 * 版本 : 1.0.0
 * 语言 : C11
 *
 * 提供加密功能，支持多种加密算法和密钥管理
 */

#ifndef YQ_CRYPTO_H
#define YQ_CRYPTO_H

#include "yq.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════
 * 加密算法类型
 * ═══════════════════════════════════════════════════════════════════════ */

typedef enum yq_crypto_algorithm {
    YQ_CRYPTO_NONE = 0,      /* 不加密 */
    YQ_CRYPTO_AES256 = 1,    /* AES-256 加密 */
    YQ_CRYPTO_CHACHA20 = 2,  /* ChaCha20 加密 */
    YQ_CRYPTO_XOR = 3,      /* 简单XOR加密 */
    YQ_CRYPTO_MAX = 4       /* 最大加密算法数量 */
} yq_crypto_algorithm;

/* ═══════════════════════════════════════════════════════════════════════
 * 加密模式
 * ═══════════════════════════════════════════════════════════════════════ */

typedef enum yq_crypto_mode {
    YQ_CRYPTO_MODE_ECB = 0,    /* 电子密码本模式 */
    YQ_CRYPTO_MODE_CBC = 1,    /* 密码分组链接模式 */
    YQ_CRYPTO_MODE_GCM = 2,    /* Galois/Counter Mode */
    YQ_CRYPTO_MODE_MAX = 3     /* 最大加密模式数量 */
} yq_crypto_mode;

/* ═══════════════════════════════════════════════════════════════════════
 * 加密配置
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct yq_crypto_opts {
    uint32_t struct_size;      /* 必须 = sizeof(yq_crypto_opts) */
    yq_crypto_algorithm algorithm;  /* 加密算法 */
    yq_crypto_mode mode;              /* 加密模式 */
    uint32_t key_size;         /* 密钥大小（字节） */
    uint32_t iv_size;          /* 初始化向量大小（字节） */
    uint32_t min_size;         /* 最小加密大小（字节），小于此值不加密 */
    uint32_t reserved[8];      /* 必须为 0 */
} yq_crypto_opts;

/* ═══════════════════════════════════════════════════════════════════════
 * 加密结果
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct yq_crypto_result {
    uint32_t struct_size;      /* 必须 = sizeof(yq_crypto_result) */
    int success;               /* 加密是否成功 */
    size_t original_size;      /* 原始数据大小 */
    size_t encrypted_size;     /* 加密后数据大小 */
    uint8_t *iv;               /* 初始化向量（如果有） */
    size_t iv_size;            /* 初始化向量大小 */
    uint32_t reserved[8];      /* 必须为 0 */
} yq_crypto_result;

/* ═══════════════════════════════════════════════════════════════════════
 * 密钥管理
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * 生成随机密钥
 * 输入：size - 密钥大小（字节）
 * 返回：生成的密钥，调用者负责释放
 */
uint8_t *yq_crypto_generate_key(size_t size);

/*
 * 从密码生成密钥（PBKDF2）
 * 输入：password - 密码，size - 密钥大小，salt - 盐值，salt_size - 盐值大小，iterations - 迭代次数
 * 返回：派生密钥，调用者负责释放
 */
uint8_t *yq_crypto_derive_key(const char *password, size_t size, const uint8_t *salt, size_t salt_size, uint32_t iterations);

/*
 * 生成随机盐值
 * 输入：size - 盐值大小
 * 返回：生成的盐值，调用者负责释放
 */
uint8_t *yq_crypto_generate_salt(size_t size);

/*
 * 生成随机初始化向量
 * 输入：size - IV大小
 * 返回：生成的IV，调用者负责释放
 */
uint8_t *yq_crypto_generate_iv(size_t size);

/* ═══════════════════════════════════════════════════════════════════════
 * 加密操作
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * 设置加密配置
 * 输入：db - 数据库句柄，opts - 加密配置
 * 返回：YQ_OK 成功，其他错误码见 ERRORS.md
 */
int yq_crypto_configure(yq_db *db, const yq_crypto_opts *opts);

/*
 * 获取加密配置
 * 输入：db - 数据库句柄，opts - 输出配置
 * 返回：YQ_OK 成功，其他错误码见 ERRORS.md
 */
int yq_crypto_get_config(yq_db *db, yq_crypto_opts *opts);

/*
 * 加密数据
 * 输入：data - 原始数据，size - 数据大小，key - 密钥，iv - 初始化向量
 * 返回：加密结果，调用者负责释放
 */
int yq_crypto_encrypt(const void *data, size_t size, const uint8_t *key, const uint8_t *iv, yq_crypto_result **result);

/*
 * 解密数据
 * 输入：data - 加密数据，size - 数据大小，key - 密钥，iv - 初始化向量
 * 返回：解密结果，调用者负责释放
 */
int yq_crypto_decrypt(const void *data, size_t size, const uint8_t *key, const uint8_t *iv, yq_crypto_result **result);

/*
 * 自动加密存储（内部使用）
 * 输入：txn - 事务句柄，key - 键，value - 值
 * 返回：YQ_OK 成功，其他错误码见 ERRORS.md
 */
int yq_crypto_put(yq_txn *txn, const yq_slice *key, const yq_slice *value);

/*
 * 自动解密读取（内部使用）
 * 输入：txn - 事务句柄，key - 键，out - 输出值
 * 返回：YQ_OK 成功，其他错误码见 ERRORS.md
 */
int yq_crypto_get(yq_txn *txn, const yq_slice *key, yq_slice *out);

/*
 * 检查键是否被加密
 * 输入：txn - 事务句柄，key - 键
 * 返回：1 表示已加密，0 表示未加密，-1 表示错误
 */
int yq_crypto_is_encrypted(yq_txn *txn, const yq_slice *key);

/*
 * 删除加密数据
 * 输入：txn - 事务句柄，key - 键
 * 返回：YQ_OK 成功，其他错误码见 ERRORS.md
 */
int yq_crypto_delete(yq_txn *txn, const yq_slice *key);

/* ═══════════════════════════════════════════════════════════════════════
 * 加密统计
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * 获取加密键数量
 * 输入：db - 数据库句柄
 * 返回：加密键数量，-1 表示错误
 */
int64_t yq_crypto_stats(yq_db *db);

/*
 * 获取加密内存使用量
 * 输入：db - 数据库句柄
 * 返回：内存使用量（字节），-1 表示错误
 */
int64_t yq_crypto_memory_usage(yq_db *db);

/*
 * 获取加密密钥数量
 * 输入：db - 数据库句柄
 * 返回：密钥数量，-1 表示错误
 */
int64_t yq_crypto_key_count(yq_db *db);

/* ═══════════════════════════════════════════════════════════════════════
 * 加密结果清理
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * 释放加密结果
 * 输入：result - 加密结果
 */
void yq_crypto_result_free(yq_crypto_result *result);

/* ═══════════════════════════════════════════════════════════════════════
 * 加密工具函数
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * 获取加密算法名称
 * 输入：algorithm - 加密算法
 * 返回：算法名称字符串
 */
const char *yq_crypto_algorithm_name(yq_crypto_algorithm algorithm);

/*
 * 获取加密模式名称
 * 输入：mode - 加密模式
 * 返回：模式名称字符串
 */
const char *yq_crypto_mode_name(yq_crypto_mode mode);

/*
 * 获取加密算法描述
 * 输入：algorithm - 加密算法
 * 返回：算法描述字符串
 */
const char *yq_crypto_algorithm_description(yq_crypto_algorithm algorithm);

/*
 * 估算加密后大小
 * 输入：data - 原始数据，size - 数据大小，algorithm - 加密算法
 * 返回：估算的加密后大小，0 表示无法估算
 */
size_t yq_crypto_estimate_size(const void *data, size_t size, yq_crypto_algorithm algorithm);

/*
 * 检查加密算法是否可用
 * 输入：algorithm - 加密算法
 * 返回：1 表示可用，0 表示不可用
 */
int yq_crypto_is_available(yq_crypto_algorithm algorithm);

/*
 * 计算数据哈希
 * 输入：data - 数据，size - 数据大小，algorithm - 哈希算法
 * 返回：哈希值，调用者负责释放
 */
uint8_t *yq_crypto_hash(const void *data, size_t size, const char *algorithm);

/*
 * 验证数据完整性
 * 输入：data - 数据，size - 数据大小，hash - 哈希值，algorithm - 哈希算法
 * 返回：1 表示验证成功，0 表示验证失败
 */
int yq_crypto_verify(const void *data, size_t size, const uint8_t *hash, const char *algorithm);

#ifdef __cplusplus
}
#endif

#endif /* YQ_CRYPTO_H */