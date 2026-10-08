/*
 * yq_compress.h — Compression functionality for yq-DB
 *
 * 版本 : 1.0.0
 * 语言 : C11
 *
 * 提供压缩功能，支持多种压缩算法和自动压缩
 */

#ifndef YQ_COMPRESS_H
#define YQ_COMPRESS_H

#include "yq.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════
 * 压缩算法类型
 * ═══════════════════════════════════════════════════════════════════════ */

typedef enum yq_compress_algorithm {
    YQ_COMPRESS_NONE = 0,    /* 不压缩 */
    YQ_COMPRESS_SNAPPY = 1,  /* Snappy 压缩 */
    YQ_COMPRESS_LZ4 = 2,    /* LZ4 压缩 */
    YQ_COMPRESS_ZSTD = 3,   /* Zstandard 压缩 */
    YQ_COMPRESS_ZLIB = 4,   /* Zlib 压缩 */
    YQ_COMPRESS_MAX = 5      /* 最大压缩算法数量 */
} yq_compress_algorithm;

/* ═══════════════════════════════════════════════════════════════════════
 * 压缩级别
 * ═══════════════════════════════════════════════════════════════════════ */

typedef enum yq_compress_level {
    YQ_COMPRESS_LEVEL_NONE = 0,   /* 不压缩 */
    YQ_COMPRESS_LEVEL_FAST = 1,   /* 快速压缩（速度优先） */
    YQ_COMPRESS_LEVEL_NORMAL = 2, /* 标准压缩（速度/压缩比平衡） */
    YQ_COMPRESS_LEVEL_MAX = 3     /* 最大压缩（压缩比优先） */
} yq_compress_level;

/* ═══════════════════════════════════════════════════════════════════════
 * 压缩配置
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct yq_compress_opts {
    uint32_t struct_size;      /* 必须 = sizeof(yq_compress_opts) */
    yq_compress_algorithm algorithm;  /* 压缩算法 */
    yq_compress_level level;          /* 压缩级别 */
    uint32_t min_size;         /* 最小压缩大小（字节），小于此值不压缩 */
    uint32_t reserved[8];      /* 必须为 0 */
} yq_compress_opts;

/* ═══════════════════════════════════════════════════════════════════════
 * 压缩结果
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct yq_compress_result {
    uint32_t struct_size;      /* 必须 = sizeof(yq_compress_result) */
    int success;               /* 压缩是否成功 */
    size_t original_size;      /* 原始数据大小 */
    size_t compressed_size;    /* 压缩后数据大小 */
    double ratio;             /* 压缩比 (0.0-1.0) */
    uint32_t reserved[8];      /* 必须为 0 */
} yq_compress_result;

/* ═══════════════════════════════════════════════════════════════════════
 * 压缩操作
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * 设置压缩配置
 * 输入：db - 数据库句柄，opts - 压缩配置
 * 返回：YQ_OK 成功，其他错误码见 ERRORS.md
 */
int yq_compress_configure(yq_db *db, const yq_compress_opts *opts);

/*
 * 获取压缩配置
 * 输入：db - 数据库句柄，opts - 输出配置
 * 返回：YQ_OK 成功，其他错误码见 ERRORS.md
 */
int yq_compress_get_config(yq_db *db, yq_compress_opts *opts);

/*
 * 压缩数据
 * 输入：data - 原始数据，size - 数据大小，level - 压缩级别
 * 返回：压缩结果，调用者负责释放
 */
int yq_compress_data(const void *data, size_t size, yq_compress_level level, yq_compress_result **result);

/*
 * 解压数据
 * 输入：data - 压缩数据，size - 数据大小
 * 返回：解压结果，调用者负责释放
 */
int yq_decompress_data(const void *data, size_t size, yq_compress_result **result);

/*
 * 自动压缩存储（内部使用）
 * 输入：txn - 事务句柄，key - 键，value - 值
 * 返回：YQ_OK 成功，其他错误码见 ERRORS.md
 */
int yq_compress_put(yq_txn *txn, const yq_slice *key, const yq_slice *value);

/*
 * 自动解压读取（内部使用）
 * 输入：txn - 事务句柄，key - 键，out - 输出值
 * 返回：YQ_OK 成功，其他错误码见 ERRORS.md
 */
int yq_compress_get(yq_txn *txn, const yq_slice *key, yq_slice *out);

/*
 * 检查键是否被压缩
 * 输入：txn - 事务句柄，key - 键
 * 返回：1 表示已压缩，0 表示未压缩，-1 表示错误
 */
int yq_compress_is_compressed(yq_txn *txn, const yq_slice *key);

/*
 * 获取压缩统计信息
 * 输入：db - 数据库句柄
 * 返回：压缩键数量，-1 表示错误
 */
int64_t yq_compress_stats(yq_db *db);

/*
 * 获取压缩节省的空间
 * 输入：db - 数据库句柄
 * 返回：节省的字节数，-1 表示错误
 */
int64_t yq_compress_saved_space(yq_db *db);

/*
 * 获取压缩内存使用量
 * 输入：db - 数据库句柄
 * 返回：内存使用量（字节），-1 表示错误
 */
int64_t yq_compress_memory_usage(yq_db *db);

/* ═══════════════════════════════════════════════════════════════════════
 * 压缩结果清理
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * 释放压缩结果
 * 输入：result - 压缩结果
 */
void yq_compress_result_free(yq_compress_result *result);

/* ═══════════════════════════════════════════════════════════════════════
 * 压缩工具函数
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * 获取压缩算法名称
 * 输入：algorithm - 压缩算法
 * 返回：算法名称字符串
 */
const char *yq_compress_algorithm_name(yq_compress_algorithm algorithm);

/*
 * 获取压缩级别名称
 * 输入：level - 压缩级别
 * 返回：级别名称字符串
 */
const char *yq_compress_level_name(yq_compress_level level);

/*
 * 获取压缩算法描述
 * 输入：algorithm - 压缩算法
 * 返回：算法描述字符串
 */
const char *yq_compress_algorithm_description(yq_compress_algorithm algorithm);

/*
 * 估算压缩后大小
 * 输入：data - 原始数据，size - 数据大小，algorithm - 压缩算法
 * 返回：估算的压缩后大小，0 表示无法估算
 */
size_t yq_compress_estimate_size(const void *data, size_t size, yq_compress_algorithm algorithm);

/*
 * 检查压缩算法是否可用
 * 输入：algorithm - 压缩算法
 * 返回：1 表示可用，0 表示不可用
 */
int yq_compress_is_available(yq_compress_algorithm algorithm);

#ifdef __cplusplus
}
#endif

#endif /* YQ_COMPRESS_H */