/*
 * yq_mempool.h — Optimized memory pool for yq-DB
 *
 * 版本   : 1.0.0
 * 语言   : C11
 * 格式版本: 1（见 FORMAT.md）
 *
 * 设计约束（改动本头文件前请先读）：
 *   1. 本头文件提供优化的内存池实现
 *   2. 使用slab分配算法减少内存碎片
 *   3. 实现内存重用和预分配机制
 *   4. 支持多种大小的内存块分配
 *   5. 线程安全的内存池操作
 */

#ifndef YQ_MEMPOOL_H
#define YQ_MEMPOOL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════
 * 内存池配置常量
 * ═══════════════════════════════════════════════════════════════════════ */

#define YQ_MEMPOOL_MIN_SIZE     16
#define YQ_MEMPOOL_MAX_SIZE     (1 << 20)  /* 1MB */
#define YQ_MEMPOOL_SLAB_COUNT   16
#define YQ_MEMPOOL_SLAB_SIZE    1024
#define YQ_MEMPOOL_CACHE_SIZE   32
#define YQ_MEMPOOL_MAX_OBJECTS  1024

/* ═══════════════════════════════════════════════════════════════════════
 * 内存块结构
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct yq_mempool_slab {
    uint8_t *slab_base;
    size_t slab_size;
    size_t object_size;
    size_t free_count;
    uint16_t free_bitmap[YQ_MEMPOOL_SLAB_SIZE / 16];  /* 64-bit bitmap */
    struct yq_mempool_slab *next;
    volatile uint32_t generation;
    volatile uint32_t ref_count;
} yq_mempool_slab;

typedef struct yq_mempool_cache {
    void *objects[YQ_MEMPOOL_CACHE_SIZE];
    size_t object_size;
    size_t cache_count;
    size_t cache_capacity;
    struct yq_mempool_cache *next;
} yq_mempool_cache;

typedef struct yq_mempool_stats {
    size_t total_allocated;
    size_t total_freed;
    size_t current_usage;
    size_t peak_usage;
    size_t allocation_count;
    size_t free_count;
    size_t cache_hits;
    size_t cache_misses;
    size_t slab_count;
    size_t cache_count;
} yq_mempool_stats;

typedef struct yq_mempool {
    yq_mempool_slab *slabs[YQ_MEMPOOL_SLAB_COUNT];
    yq_mempool_cache *caches[YQ_MEMPOOL_SLAB_COUNT];
    yq_mempool_stats stats;
    size_t slab_size;
    size_t cache_capacity;
    volatile uint32_t generation;
    volatile uint32_t active_writers;
    void *arena_base;
    size_t arena_size;
    bool use_arena;
} yq_mempool;

/* ═══════════════════════════════════════════════════════════════════════
 * 内存池函数
 * ═══════════════════════════════════════════════════════════════════════ */

/* 创建内存池 */
yq_mempool *yq_mempool_create(size_t slab_size, size_t cache_capacity);

/* 销毁内存池 */
void yq_mempool_destroy(yq_mempool *pool);

/* 分配内存 */
void *yq_mempool_alloc(yq_mempool *pool, size_t size);

/* 释放内存 */
void yq_mempool_free(yq_mempool *pool, void *ptr, size_t size);

/* 分配并清零内存 */
void *yq_mempool_calloc(yq_mempool *pool, size_t nmemb, size_t size);

/* 重新分配内存 */
void *yq_mempool_realloc(yq_mempool *pool, void *ptr, size_t old_size, size_t new_size);

/* 获取内存池统计信息 */
void yq_mempool_get_stats(yq_mempool *pool, yq_mempool_stats *stats);

/* 重置内存池缓存 */
void yq_mempool_reset_cache(yq_mempool *pool);

/* 预分配内存 */
int yq_mempool_prealloc(yq_mempool *pool, size_t size, size_t count);

/* 内存池性能分析 */
void yq_mempool_analyze(yq_mempool *pool, FILE *output);

/* ═══════════════════════════════════════════════════════════════════════
 * 内存池宏
 * ═══════════════════════════════════════════════════════════════════════ */

/* 快速分配宏 */
#define yq_mempool_new(pool, type) \
    ((type *)yq_mempool_alloc(pool, sizeof(type)))

#define yq_mempool_new_array(pool, type, count) \
    ((type *)yq_mempool_alloc(pool, sizeof(type) * (count)))

#define yq_mempool_new_zero(pool, type) \
    ((type *)yq_mempool_calloc(pool, 1, sizeof(type)))

#define yq_mempool_free_type(pool, ptr, type) \
    yq_mempool_free(pool, ptr, sizeof(type))

/* ═══════════════════════════════════════════════════════════════════════
 * 内存池配置
 * ═══════════════════════════════════════════════════════════════════════ */

/* 获取最适合的slab大小 */
static inline size_t yq_mempool_get_slab_size(size_t size) {
    if (size <= 16) return 16;
    if (size <= 32) return 32;
    if (size <= 64) return 64;
    if (size <= 128) return 128;
    if (size <= 256) return 256;
    if (size <= 512) return 512;
    if (size <= 1024) return 1024;
    if (size <= 2048) return 2048;
    if (size <= 4096) return 4096;
    if (size <= 8192) return 8192;
    if (size <= 16384) return 16384;
    if (size <= 32768) return 32768;
    return 65536;  /* 64KB max */
}

/* 获取slab索引 */
static inline int yq_mempool_get_slab_index(size_t size) {
    size_t slab_size = yq_mempool_get_slab_size(size);
    int index = 0;
    while (slab_size > 16) {
        slab_size >>= 1;
        index++;
    }
    return index;
}

/* ═══════════════════════════════════════════════════════════════════════
 * 内存池调试
 * ═══════════════════════════════════════════════════════════════════════ */

/* 内存池断言 */
#define YQ_MEMPOOL_ASSERT(pool, ptr) \
    do { \
        if (!(pool) || !(ptr)) { \
            return NULL; \
        } \
    } while (0)

/* 内存池边界检查 */
#define YQ_MEMPOOL_BOUND_CHECK(pool, ptr, size) \
    do { \
        if (!(pool) || !(ptr) || (size) == 0) { \
            return NULL; \
        } \
        if ((size) > YQ_MEMPOOL_MAX_SIZE) { \
            return NULL; \
        } \
    } while (0)

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* YQ_MEMPOOL_H */