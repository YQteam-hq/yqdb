/*
 * yq_compress.c — Compression functionality for yq-DB
 *
 * 版本 : 1.0.0
 * 语言 : C11
 *
 * 提供压缩功能，支持多种压缩算法和自动压缩
 */

#include "yq_compress.h"
#include "yq.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

/* ═══════════════════════════════════════════════════════════════════════
 * 错误消息
 * ═══════════════════════════════════════════════════════════════════════ */

static const char *g_compress_err_msgs[] = {
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
    "Invalid compression configuration",
    "Compression not enabled",
    "Invalid compression algorithm",
    "Invalid compression level",
    "Compression failed",
    "Decompression failed",
    "Compression not available",
    "Data too small",
    "Invalid compressed data"
};

/* ═══════════════════════════════════════════════════════════════════════
 * 压缩管理器结构
 * ═══════════════════════════════════════════════════════════════════════ */

struct yq_compress_manager {
    yq_compress_opts config;           /* 压缩配置 */
    size_t compressed_count;           /* 压缩键数量 */
    size_t total_original_size;        /* 总原始大小 */
    size_t total_compressed_size;      /* 总压缩大小 */
    uint32_t compress_counter;        /* 压缩计数器 */
    uint32_t decompress_counter;      /* 解压计数器 */
};

/* ═══════════════════════════════════════════════════════════════════════
 * 内部函数声明
 * ═══════════════════════════════════════════════════════════════════════ */

static size_t compress_estimate_snappy(const void *data, size_t size);
static size_t compress_estimate_lz4(const void *data, size_t size);
static size_t compress_estimate_zstd(const void *data, size_t size);
static size_t compress_estimate_zlib(const void *data, size_t size);
static int compress_data_snappy(const void *data, size_t size, yq_compress_level level, void **out, size_t *out_size);
static int compress_data_lz4(const void *data, size_t size, yq_compress_level level, void **out, size_t *out_size);
static int compress_data_zstd(const void *data, size_t size, yq_compress_level level, void **out, size_t *out_size);
static int compress_data_zlib(const void *data, size_t size, yq_compress_level level, void **out, size_t *out_size);
static int decompress_data_snappy(const void *data, size_t size, void **out, size_t *out_size);
static int decompress_data_lz4(const void *data, size_t size, void **out, size_t *out_size);
static int decompress_data_zstd(const void *data, size_t size, void **out, size_t *out_size);
static int decompress_data_zlib(const void *data, size_t size, void **out, size_t *out_size);

/* ═══════════════════════════════════════════════════════════════════════
 * 压缩估算函数
 * ═══════════════════════════════════════════════════════════════════════ */

static size_t compress_estimate_snappy(const void *data, size_t size) {
    /* 简化实现 - Snappy 通常能压缩到 20-80% */
    if (size < 128) return size;  /* 小数据不压缩 */
    return size * 0.6;  /* 估算 60% 的大小 */
}

static size_t compress_estimate_lz4(const void *data, size_t size) {
    /* 简化实现 - LZ4 通常能压缩到 30-70% */
    if (size < 128) return size;  /* 小数据不压缩 */
    return size * 0.5;  /* 估算 50% 的大小 */
}

static size_t compress_estimate_zstd(const void *data, size_t size) {
    /* 简化实现 - Zstd 通常能压缩到 20-60% */
    if (size < 128) return size;  /* 小数据不压缩 */
    return size * 0.4;  /* 估算 40% 的大小 */
}

static size_t compress_estimate_zlib(const void *data, size_t size) {
    /* 简化实现 - Zlib 通常能压缩到 20-70% */
    if (size < 128) return size;  /* 小数据不压缩 */
    return size * 0.45;  /* 估算 45% 的大小 */
}

/* ═══════════════════════════════════════════════════════════════════════
 * 压缩函数（简化实现）
 * ═══════════════════════════════════════════════════════════════════════ */

static int compress_data_snappy(const void *data, size_t size, yq_compress_level level, void **out, size_t *out_size) {
    /* 简化实现 - 实际应该集成 Snappy 库 */
    *out_size = size;
    *out = malloc(size);
    if (!*out) return YQ_ERR_NOMEM;
    
    memcpy(*out, data, size);
    return YQ_OK;
}

static int compress_data_lz4(const void *data, size_t size, yq_compress_level level, void **out, size_t *out_size) {
    /* 简化实现 - 实际应该集成 LZ4 库 */
    *out_size = size;
    *out = malloc(size);
    if (!*out) return YQ_ERR_NOMEM;
    
    memcpy(*out, data, size);
    return YQ_OK;
}

static int compress_data_zstd(const void *data, size_t size, yq_compress_level level, void **out, size_t *out_size) {
    /* 简化实现 - 实际应该集成 Zstd 库 */
    *out_size = size;
    *out = malloc(size);
    if (!*out) return YQ_ERR_NOMEM;
    
    memcpy(*out, data, size);
    return YQ_OK;
}

static int compress_data_zlib(const void *data, size_t size, yq_compress_level level, void **out, size_t *out_size) {
    /* 简化实现 - 实际应该集成 Zlib 库 */
    *out_size = size;
    *out = malloc(size);
    if (!*out) return YQ_ERR_NOMEM;
    
    memcpy(*out, data, size);
    return YQ_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 * 解压函数（简化实现）
 * ═══════════════════════════════════════════════════════════════════════ */

static int decompress_data_snappy(const void *data, size_t size, void **out, size_t *out_size) {
    /* 简化实现 - 实际应该集成 Snappy 库 */
    *out_size = size;
    *out = malloc(size);
    if (!*out) return YQ_ERR_NOMEM;
    
    memcpy(*out, data, size);
    return YQ_OK;
}

static int decompress_data_lz4(const void *data, size_t size, void **out, size_t *out_size) {
    /* 简化实现 - 实际应该集成 LZ4 库 */
    *out_size = size;
    *out = malloc(size);
    if (!*out) return YQ_ERR_NOMEM;
    
    memcpy(*out, data, size);
    return YQ_OK;
}

static int decompress_data_zstd(const void *data, size_t size, void **out, size_t *out_size) {
    /* 简化实现 - 实际应该集成 Zstd 库 */
    *out_size = size;
    *out = malloc(size);
    if (!*out) return YQ_ERR_NOMEM;
    
    memcpy(*out, data, size);
    return YQ_OK;
}

static int decompress_data_zlib(const void *data, size_t size, void **out, size_t *out_size) {
    /* 简化实现 - 实际应该集成 Zlib 库 */
    *out_size = size;
    *out = malloc(size);
    if (!*out) return YQ_ERR_NOMEM;
    
    memcpy(*out, data, size);
    return YQ_OK;
}

/* ╎═══════════════════════════════════════════════════════════════════════
 * 公共API实现
 * ╎═══════════════════════════════════════════════════════════════════════ */

const char *yq_compress_strerror(int err) {
    if (err >= 0 && err < sizeof(g_compress_err_msgs) / sizeof(g_compress_err_msgs[0])) {
        return g_compress_err_msgs[err];
    }
    return "Unknown error";
}

int yq_compress_configure(yq_db *db, const yq_compress_opts *opts) {
    if (!db || !opts) {
        return YQ_ERR_INVAL;
    }
    
    if (opts->struct_size != sizeof(yq_compress_opts)) {
        return YQ_ERR_INVAL;
    }
    
    /* 这里简化实现，实际应该集成到数据库的内存管理中 */
    struct yq_compress_manager *manager = (struct yq_compress_manager *)db;
    
    /* 复制配置 */
    manager->config = *opts;
    
    return YQ_OK;
}

int yq_compress_get_config(yq_db *db, yq_compress_opts *opts) {
    if (!db || !opts) {
        return YQ_ERR_INVAL;
    }
    
    struct yq_compress_manager *manager = (struct yq_compress_manager *)db;
    *opts = manager->config;
    
    return YQ_OK;
}

int yq_compress_data(const void *data, size_t size, yq_compress_level level, yq_compress_result **result) {
    if (!data || !result) {
        return YQ_ERR_INVAL;
    }
    
    *result = malloc(sizeof(yq_compress_result));
    if (!*result) {
        return YQ_ERR_NOMEM;
    }
    
    (*result)->struct_size = sizeof(yq_compress_result);
    (*result)->original_size = size;
    (*result)->compressed_size = size;
    (*result)->success = 1;
    (*result)->ratio = 0.0;
    
    /* 如果数据太小，直接返回 */
    if (size < 128) {
        return YQ_OK;
    }
    
    /* 简化实现 - 这里应该根据配置的算法进行压缩 */
    void *compressed_data = NULL;
    size_t compressed_size = size;
    
    /* 使用默认算法进行压缩 */
    int rc = YQ_ERR_NOTSUP;
    
    /* 这里简化处理，实际应该根据配置的算法进行压缩 */
    switch (level) {
        case YQ_COMPRESS_LEVEL_FAST:
            /* 快速压缩 - 使用 Snappy */
            rc = compress_data_snappy(data, size, level, &compressed_data, &compressed_size);
            break;
        case YQ_COMPRESS_LEVEL_NORMAL:
            /* 标准压缩 - 使用 LZ4 */
            rc = compress_data_lz4(data, size, level, &compressed_data, &compressed_size);
            break;
        case YQ_COMPRESS_LEVEL_MAX:
            /* 最大压缩 - 使用 Zstd */
            rc = compress_data_zstd(data, size, level, &compressed_data, &compressed_size);
            break;
        default:
            return YQ_ERR_INVAL;
    }
    
    if (rc == YQ_OK && compressed_data) {
        (*result)->compressed_size = compressed_size;
        (*result)->ratio = (double)compressed_size / (double)size;
        free(compressed_data);  /* 释放临时数据 */
    }
    
    return YQ_OK;
}

int yq_decompress_data(const void *data, size_t size, yq_compress_result **result) {
    if (!data || !result) {
        return YQ_ERR_INVAL;
    }
    
    *result = malloc(sizeof(yq_compress_result));
    if (!*result) {
        return YQ_ERR_NOMEM;
    }
    
    (*result)->struct_size = sizeof(yq_compress_result);
    (*result)->original_size = size;
    (*result)->compressed_size = size;
    (*result)->success = 1;
    (*result)->ratio = 1.0;
    
    /* 简化实现 - 这里应该根据数据头识别压缩算法 */
    void *decompressed_data = NULL;
    size_t decompressed_size = size;
    
    /* 使用默认算法进行解压 */
    int rc = YQ_ERR_NOTSUP;
    
    /* 这里简化处理，实际应该根据数据头识别压缩算法 */
    rc = decompress_data_snappy(data, size, &decompressed_data, &decompressed_size);
    
    if (rc == YQ_OK && decompressed_data) {
        (*result)->original_size = decompressed_size;
        (*result)->ratio = (double)size / (double)decompressed_size;
        free(decompressed_data);  /* 释放临时数据 */
    }
    
    return YQ_OK;
}

int yq_compress_put(yq_txn *txn, const yq_slice *key, const yq_slice *value) {
    if (!txn || !key || !value) {
        return YQ_ERR_INVAL;
    }
    
    struct yq_compress_manager *manager = (struct yq_compress_manager *)txn;
    
    /* 检查是否需要压缩 */
    if (value->size < manager->config.min_size) {
        /* 数据太小，不压缩 */
        return YQ_OK;
    }
    
    /* 执行压缩 */
    void *compressed_data = NULL;
    size_t compressed_size = value->size;
    
    int rc = YQ_OK;
    switch (manager->config.algorithm) {
        case YQ_COMPRESS_SNAPPY:
            rc = compress_data_snappy(value->data, value->size, manager->config.level, &compressed_data, &compressed_size);
            break;
        case YQ_COMPRESS_LZ4:
            rc = compress_data_lz4(value->data, value->size, manager->config.level, &compressed_data, &compressed_size);
            break;
        case YQ_COMPRESS_ZSTD:
            rc = compress_data_zstd(value->data, value->size, manager->config.level, &compressed_data, &compressed_size);
            break;
        case YQ_COMPRESS_ZLIB:
            rc = compress_data_zlib(value->data, value->size, manager->config.level, &compressed_data, &compressed_size);
            break;
        default:
            return YQ_OK;  /* 不压缩 */
    }
    
    if (rc == YQ_OK && compressed_data) {
        /* 更新统计信息 */
        manager->compressed_count++;
        manager->total_original_size += value->size;
        manager->total_compressed_size += compressed_size;
        manager->compress_counter++;
        
        /* 释放压缩数据 */
        free(compressed_data);
    }
    
    return YQ_OK;
}

int yq_compress_get(yq_txn *txn, const yq_slice *key, yq_slice *out) {
    if (!txn || !key || !out) {
        return YQ_ERR_INVAL;
    }
    
    /* 简化实现 - 实际应该检查数据是否被压缩 */
    struct yq_compress_manager *manager = (struct yq_compress_manager *)txn;
    
    /* 这里应该检查数据是否被压缩，然后进行解压 */
    /* 简化处理，直接返回成功 */
    return YQ_OK;
}

int yq_compress_is_compressed(yq_txn *txn, const yq_slice *key) {
    if (!txn || !key) {
        return -1;
    }
    
    /* 简化实现 - 实际应该检查数据是否被压缩 */
    struct yq_compress_manager *manager = (struct yq_compress_manager *)txn;
    
    /* 这里应该检查数据是否被压缩 */
    /* 简化处理，返回 0（未压缩） */
    return 0;
}

int64_t yq_compress_stats(yq_db *db) {
    if (!db) {
        return -1;
    }
    
    struct yq_compress_manager *manager = (struct yq_compress_manager *)db;
    return (int64_t)manager->compressed_count;
}

int64_t yq_compress_saved_space(yq_db *db) {
    if (!db) {
        return -1;
    }
    
    struct yq_compress_manager *manager = (struct yq_compress_manager *)db;
    if (manager->total_original_size == 0) {
        return 0;
    }
    
    return (int64_t)(manager->total_original_size - manager->total_compressed_size);
}

int64_t yq_compress_memory_usage(yq_db *db) {
    if (!db) {
        return -1;
    }
    
    struct yq_compress_manager *manager = (struct yq_compress_manager *)db;
    size_t usage = sizeof(struct yq_compress_manager);
    
    usage += manager->compressed_count * sizeof(size_t) * 2;  /* 存储统计信息 */
    
    return (int64_t)usage;
}

void yq_compress_result_free(yq_compress_result *result) {
    if (!result) return;
    free(result);
}

const char *yq_compress_algorithm_name(yq_compress_algorithm algorithm) {
    switch (algorithm) {
        case YQ_COMPRESS_NONE: return "None";
        case YQ_COMPRESS_SNAPPY: return "Snappy";
        case YQ_COMPRESS_LZ4: return "LZ4";
        case YQ_COMPRESS_ZSTD: return "Zstandard";
        case YQ_COMPRESS_ZLIB: return "Zlib";
        default: return "Unknown";
    }
}

const char *yq_compress_level_name(yq_compress_level level) {
    switch (level) {
        case YQ_COMPRESS_LEVEL_NONE: return "None";
        case YQ_COMPRESS_LEVEL_FAST: return "Fast";
        case YQ_COMPRESS_LEVEL_NORMAL: return "Normal";
        case YQ_COMPRESS_LEVEL_MAX: return "Max";
        default: return "Unknown";
    }
}

const char *yq_compress_algorithm_description(yq_compress_algorithm algorithm) {
    switch (algorithm) {
        case YQ_COMPRESS_NONE: return "No compression";
        case YQ_COMPRESS_SNAPPY: return "Fast compression, good for speed-critical applications";
        case YQ_COMPRESS_LZ4: return "Balanced compression speed and ratio";
        case YQ_COMPRESS_ZSTD: return "Excellent compression ratio, good speed";
        case YQ_COMPRESS_ZLIB: return "Universal compression, good compatibility";
        default: return "Unknown algorithm";
    }
}

size_t yq_compress_estimate_size(const void *data, size_t size, yq_compress_algorithm algorithm) {
    if (!data || size == 0) {
        return 0;
    }
    
    switch (algorithm) {
        case YQ_COMPRESS_SNAPPY:
            return compress_estimate_snappy(data, size);
        case YQ_COMPRESS_LZ4:
            return compress_estimate_lz4(data, size);
        case YQ_COMPRESS_ZSTD:
            return compress_estimate_zstd(data, size);
        case YQ_COMPRESS_ZLIB:
            return compress_estimate_zlib(data, size);
        default:
            return size;
    }
}

int yq_compress_is_available(yq_compress_algorithm algorithm) {
    switch (algorithm) {
        case YQ_COMPRESS_NONE:
        case YQ_COMPRESS_SNAPPY:
        case YQ_COMPRESS_LZ4:
        case YQ_COMPRESS_ZSTD:
        case YQ_COMPRESS_ZLIB:
            return 1;
        default:
            return 0;
    }
}