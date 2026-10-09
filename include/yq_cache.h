/*
 * yq_cache.h — Caching mechanisms for yq-DB
 *
 * 版本   : 1.0.0
 * 语言   : C11
 * 格式版本: 1（见 FORMAT.md）
 *
 * 设计约束（改动本头文件前请先读）：
 *   1. 本头文件提供缓存机制实现
 *   2. 支持多种缓存策略（LRU, LFU, FIFO）
 *   3. 实现线程安全的缓存操作
 *   4. 支持缓存预热和失效
 *   5. 提供缓存统计和性能监控
 */

#ifndef YQ_CACHE_H
#define YQ_CACHE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════
 * 缓存配置常量
 * ═══════════════════════════════════════════════════════════════════════ */

#define YQ_CACHE_MAX_SIZE       1024
#define YQ_CACHE_DEFAULT_SIZE   256
#define YQ_CACHE_MAX_KEY_SIZE   256
#define YQ_CACHE_MAX_VALUE_SIZE (1 << 20)  /* 1MB */
#define YQ_CACHE_MAX_TTL        3600  /* 1 hour */

/* ═══════════════════════════════════════════════════════════════════════
 * 缓存策略枚举
 * ═══════════════════════════════════════════════════════════════════════ */

typedef enum {
    YQ_CACHE_LRU,  /* Least Recently Used */
    YQ_CACHE_LFU,  /* Least Frequently Used */
    YQ_CACHE_FIFO, /* First In First Out */
    YQ_CACHE_RANDOM
} yq_cache_policy;

/* ═══════════════════════════════════════════════════════════════════════
 * 缓存统计结构
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct yq_cache_stats {
    size_t hits;
    size_t misses;
    size_t evictions;
    size_t insertions;
    size_t updates;
    size_t deletions;
    size_t expired;
    size_t current_size;
    size_t peak_size;
    double hit_ratio;
    double avg_access_time;
} yq_cache_stats;

/* ═══════════════════════════════════════════════════════════════════════
 * 缓存节点结构
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct yq_cache_node {
    char key[YQ_CACHE_MAX_KEY_SIZE];
    void *value;
    size_t value_size;
    uint64_t expiration;
    uint64_t access_time;
    uint64_t access_count;
    struct yq_cache_node *prev;
    struct yq_cache_node *next;
    struct yq_cache_node *hash_next;
} yq_cache_node;

/* ═══════════════════════════════════════════════════════════════════════
 * 缓存结构
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct yq_cache {
    yq_cache_node **hash_table;
    size_t hash_size;
    size_t capacity;
    size_t current_size;
    yq_cache_policy policy;
    yq_cache_node *head;
    yq_cache_node *tail;
    yq_cache_stats stats;
    volatile uint32_t generation;
    volatile uint32_t active_writers;
    bool auto_cleanup;
    uint64_t cleanup_interval;
    uint64_t last_cleanup;
} yq_cache;

/* ═══════════════════════════════════════════════════════════════════════
 * 缓存函数
 * ═══════════════════════════════════════════════════════════════════════ */

/* 创建缓存 */
yq_cache *yq_cache_create(size_t capacity, yq_cache_policy policy);

/* 销毁缓存 */
void yq_cache_destroy(yq_cache *cache);

/* 获取缓存项 */
void *yq_cache_get(yq_cache *cache, const char *key, size_t *value_size);

/* 设置缓存项 */
int yq_cache_set(yq_cache *cache, const char *key, const void *value, size_t value_size, uint64_t ttl);

/* 删除缓存项 */
int yq_cache_delete(yq_cache *cache, const char *key);

/* 清空缓存 */
void yq_cache_clear(yq_cache *cache);

/* 获取缓存统计 */
void yq_cache_get_stats(yq_cache *cache, yq_cache_stats *stats);

/* 重置缓存统计 */
void yq_cache_reset_stats(yq_cache *cache);

/* 预热缓存 */
int yq_cache_warmup(yq_cache *cache, const char **keys, size_t key_count, const void **values, size_t *value_sizes);

/* 设置缓存策略 */
int yq_cache_set_policy(yq_cache *cache, yq_cache_policy policy);

/* 设置自动清理 */
void yq_cache_set_auto_cleanup(yq_cache *cache, bool enabled, uint64_t interval);

/* 检查缓存项是否存在 */
bool yq_cache_exists(yq_cache *cache, const char *key);

/* 获取缓存项TTL */
uint64_t yq_cache_get_ttl(yq_cache *cache, const char *key);

/* 设置缓存项TTL */
int yq_cache_set_ttl(yq_cache *cache, const char *key, uint64_t ttl);

/* ═══════════════════════════════════════════════════════════════════════
 * 缓存宏
 * ═══════════════════════════════════════════════════════════════════════ */

/* 快速缓存操作 */
#define yq_cache_new(cache, key, value) \
    yq_cache_set(cache, key, value, sizeof(*(value)), 0)

#define yq_cache_get_type(cache, key, type) \
    ((type *)yq_cache_get(cache, key, NULL))

#define yq_cache_delete_type(cache, key, type) \
    yq_cache_delete(cache, key)

/* ═══════════════════════════════════════════════════════════════════════
 * 缓存工具函数
 * ═══════════════════════════════════════════════════════════════════════ */

/* 计算哈希值 */
static inline uint32_t yq_cache_hash(const char *key) {
    uint32_t hash = 5381;
    int c;
    while ((c = *key++)) {
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
    }
    return hash;
}

/* 验证缓存键 */
static inline bool yq_cache_validate_key(const char *key) {
    if (!key) return false;
    if (strlen(key) >= YQ_CACHE_MAX_KEY_SIZE) return false;
    return true;
}

/* 验证缓存值 */
static inline bool yq_cache_validate_value(const void *value, size_t size) {
    if (!value && size > 0) return false;
    if (size > YQ_CACHE_MAX_VALUE_SIZE) return false;
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════
 * 缓存调试
 * ═══════════════════════════════════════════════════════════════════════ */

/* 缓存断言 */
#define YQ_CACHE_ASSERT(cache, key) \
    do { \
        if (!(cache) || !(key)) { \
            return NULL; \
        } \
    } while (0)

/* 缓存边界检查 */
#define YQ_CACHE_BOUND_CHECK(cache, key, value, size) \
    do { \
        if (!(cache) || !(key) || (!(value) && (size) > 0)) { \
            return -1; \
        } \
        if ((size) > YQ_CACHE_MAX_VALUE_SIZE) { \
            return -1; \
        } \
    } while (0)

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* YQ_CACHE_H */