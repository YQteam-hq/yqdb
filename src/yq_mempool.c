#if !defined(_WIN32)
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "yq_mempool.h"
#include "yq_security.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef _WIN32
#include <windows.h>
#define memory_barrier() MemoryBarrier()
#else
#include <stdatomic.h>
#include <unistd.h>
#include <sys/mman.h>
#ifndef MAP_FAILED
#define MAP_FAILED ((void *) -1)
#endif
#define memory_barrier() __sync_synchronize()
#endif

/* ═══════════════════════════════════════════════════════════════════════
 * 内部函数声明
 * ═══════════════════════════════════════════════════════════════════════ */

static yq_mempool_slab *yq_mempool_slab_create(yq_mempool *pool, size_t object_size);
static void yq_mempool_slab_destroy(yq_mempool_slab *slab);
static void *yq_mempool_slab_alloc(yq_mempool_slab *slab);
static void yq_mempool_slab_free(yq_mempool_slab *slab, void *ptr);
static yq_mempool_cache *yq_mempool_cache_create(size_t object_size, size_t capacity);
static void yq_mempool_cache_destroy(yq_mempool_cache *cache);
static void *yq_mempool_cache_alloc(yq_mempool_cache *cache);
static void yq_mempool_cache_free(yq_mempool_cache *cache, void *ptr);
static void yq_mempool_cache_shrink(yq_mempool_cache *cache, size_t target_size);
static void yq_mempool_slab_shrink(yq_mempool_slab *slab, size_t target_size);

/* ═══════════════════════════════════════════════════════════════════════
 * 内存池创建和销毁
 * ═══════════════════════════════════════════════════════════════════════ */

yq_mempool *yq_mempool_create(size_t slab_size, size_t cache_capacity) {
    if (slab_size == 0) slab_size = YQ_MEMPOOL_SLAB_SIZE;
    if (cache_capacity == 0) cache_capacity = YQ_MEMPOOL_CACHE_SIZE;
    
    /* 验证参数 */
    if (slab_size < YQ_MEMPOOL_MIN_SIZE || slab_size > YQ_MEMPOOL_MAX_SIZE) {
        return NULL;
    }
    if (cache_capacity > YQ_MEMPOOL_CACHE_SIZE) {
        cache_capacity = YQ_MEMPOOL_CACHE_SIZE;
    }
    
    yq_mempool *pool = calloc(1, sizeof(yq_mempool));
    if (!pool) return NULL;
    
    /* 初始化内存池 */
    pool->slab_size = slab_size;
    pool->cache_capacity = cache_capacity;
    pool->generation = 1;
    pool->active_writers = 0;
    pool->use_arena = false;
    
    /* 初始化统计信息 */
    memset(&pool->stats, 0, sizeof(pool->stats));
    
    /* 初始化slab和cache数组 */
    for (int i = 0; i < YQ_MEMPOOL_SLAB_COUNT; i++) {
        pool->slabs[i] = NULL;
        pool->caches[i] = NULL;
    }
    
    return pool;
}

void yq_mempool_destroy(yq_mempool *pool) {
    if (!pool) return;
    
    /* 标记为正在销毁 */
    pool->generation = 0xDEADBEEF;
    memory_barrier();
    
    /* 销毁所有slab */
    for (int i = 0; i < YQ_MEMPOOL_SLAB_COUNT; i++) {
        yq_mempool_slab *slab = pool->slabs[i];
        while (slab) {
            yq_mempool_slab *next = slab->next;
            yq_mempool_slab_destroy(slab);
            slab = next;
        }
        pool->slabs[i] = NULL;
    }
    
    /* 销毁所有cache */
    for (int i = 0; i < YQ_MEMPOOL_SLAB_COUNT; i++) {
        yq_mempool_cache *cache = pool->caches[i];
        while (cache) {
            yq_mempool_cache *next = cache->next;
            yq_mempool_cache_destroy(cache);
            cache = next;
        }
        pool->caches[i] = NULL;
    }
    
    /* 清零敏感数据 */
    yq_security_zero(pool, sizeof(*pool));
    free(pool);
}

/* ═══════════════════════════════════════════════════════════════════════
 * 内存分配和释放
 * ═══════════════════════════════════════════════════════════════════════ */

void *yq_mempool_alloc(yq_mempool *pool, size_t size) {
    if (!pool || size == 0) return NULL;
    
    /* 验证内存池状态 */
    if (pool->generation != 1) {
        return NULL;
    }
    
    /* 检查大小限制 */
    if (size > YQ_MEMPOOL_MAX_SIZE) {
        return NULL;
    }
    
    /* 获取slab大小和索引 */
    size_t slab_size = yq_mempool_get_slab_size(size);
    int slab_index = yq_mempool_get_slab_index(size);
    
    /* 尝试从cache分配 */
    if (pool->caches[slab_index]) {
        void *ptr = yq_mempool_cache_alloc(pool->caches[slab_index]);
        if (ptr) {
            pool->stats.cache_hits++;
            pool->stats.current_usage += slab_size;
            if (pool->stats.current_usage > pool->stats.peak_usage) {
                pool->stats.peak_usage = pool->stats.current_usage;
            }
            pool->stats.allocation_count++;
            return ptr;
        }
    }
    
    pool->stats.cache_misses++;
    
    /* 创建新的slab如果需要 */
    if (!pool->slabs[slab_index]) {
        yq_mempool_slab *slab = yq_mempool_slab_create(pool, slab_size);
        if (!slab) return NULL;
        pool->slabs[slab_index] = slab;
        pool->stats.slab_count++;
    }
    
    /* 从slab分配 */
    void *ptr = yq_mempool_slab_alloc(pool->slabs[slab_index]);
    if (ptr) {
        pool->stats.total_allocated += slab_size;
        pool->stats.current_usage += slab_size;
        if (pool->stats.current_usage > pool->stats.peak_usage) {
            pool->stats.peak_usage = pool->stats.current_usage;
        }
        pool->stats.allocation_count++;
    }
    
    return ptr;
}

void yq_mempool_free(yq_mempool *pool, void *ptr, size_t size) {
    if (!pool || !ptr || size == 0) return;
    
    /* 验证内存池状态 */
    if (pool->generation != 1) {
        return;
    }
    
    /* 检查大小限制 */
    if (size > YQ_MEMPOOL_MAX_SIZE) {
        return;
    }
    
    /* 获取slab大小和索引 */
    size_t slab_size = yq_mempool_get_slab_size(size);
    int slab_index = yq_mempool_get_slab_index(size);
    
    /* 尝试释放到cache */
    if (pool->caches[slab_index] && 
        pool->caches[slab_index]->cache_count < pool->caches[slab_index]->cache_capacity) {
        yq_mempool_cache_free(pool->caches[slab_index], ptr);
        pool->stats.free_count++;
        pool->stats.current_usage -= slab_size;
        return;
    }
    
    /* 直接释放到slab */
    yq_mempool_slab_free(pool->slabs[slab_index], ptr);
    pool->stats.total_freed += slab_size;
    pool->stats.free_count++;
    pool->stats.current_usage -= slab_size;
}

void *yq_mempool_calloc(yq_mempool *pool, size_t nmemb, size_t size) {
    if (!pool || nmemb == 0 || size == 0) return NULL;
    
    /* 检查乘法溢出 */
    if (size > SIZE_MAX / nmemb) {
        return NULL;
    }
    
    size_t total_size = nmemb * size;
    void *ptr = yq_mempool_alloc(pool, total_size);
    if (ptr) {
        memset(ptr, 0, total_size);
    }
    return ptr;
}

void *yq_mempool_realloc(yq_mempool *pool, void *ptr, size_t old_size, size_t new_size) {
    if (!pool) return NULL;
    
    if (new_size == 0) {
        yq_mempool_free(pool, ptr, old_size);
        return NULL;
    }
    
    if (!ptr) {
        return yq_mempool_alloc(pool, new_size);
    }
    
    if (old_size == 0) {
        return yq_mempool_alloc(pool, new_size);
    }
    
    /* 如果大小相同，直接返回 */
    if (old_size == new_size) {
        return ptr;
    }
    
    /* 分配新内存 */
    void *new_ptr = yq_mempool_alloc(pool, new_size);
    if (!new_ptr) {
        return NULL;
    }
    
    /* 复制数据 */
    size_t copy_size = old_size < new_size ? old_size : new_size;
    memcpy(new_ptr, ptr, copy_size);
    
    /* 释放旧内存 */
    yq_mempool_free(pool, ptr, old_size);
    
    return new_ptr;
}

/* ═══════════════════════════════════════════════════════════════════════
 * 内存池统计和管理
 * ═══════════════════════════════════════════════════════════════════════ */

void yq_mempool_get_stats(yq_mempool *pool, yq_mempool_stats *stats) {
    if (!pool || !stats) return;
    
    /* 复制统计信息 */
    memcpy(stats, &pool->stats, sizeof(*stats));
    
    /* 计算当前cache数量 */
    stats->cache_count = 0;
    for (int i = 0; i < YQ_MEMPOOL_SLAB_COUNT; i++) {
        if (pool->caches[i]) {
            stats->cache_count++;
        }
    }
}

void yq_mempool_reset_cache(yq_mempool *pool) {
    if (!pool) return;
    
    /* 重置所有cache */
    for (int i = 0; i < YQ_MEMPOOL_SLAB_COUNT; i++) {
        if (pool->caches[i]) {
            yq_mempool_cache_shrink(pool->caches[i], 0);
        }
    }
}

int yq_mempool_prealloc(yq_mempool *pool, size_t size, size_t count) {
    if (!pool || size == 0 || count == 0) return -1;
    
    /* 检查乘法溢出 */
    if (count > SIZE_MAX / size) {
        return -1;
    }
    
    size_t total_size = size * count;
    if (total_size > YQ_MEMPOOL_MAX_SIZE) {
        return -1;
    }
    
    int slab_index = yq_mempool_get_slab_index(size);
    yq_mempool_slab *slab = yq_mempool_slab_create(pool, size);
    if (!slab) return -1;
    
    /* 添加到slab链表 */
    slab->next = pool->slabs[slab_index];
    pool->slabs[slab_index] = slab;
    pool->stats.slab_count++;
    
    return 0;
}

void yq_mempool_analyze(yq_mempool *pool, FILE *output) {
    if (!pool || !output) return;
    
    fprintf(output, "=== Memory Pool Analysis ===\n");
    fprintf(output, "Slab Size: %zu\n", pool->slab_size);
    fprintf(output, "Cache Capacity: %zu\n", pool->cache_capacity);
    fprintf(output, "Generation: %u\n", pool->generation);
    fprintf(output, "Active Writers: %u\n", pool->active_writers);
    fprintf(output, "\n=== Statistics ===\n");
    fprintf(output, "Total Allocated: %zu\n", pool->stats.total_allocated);
    fprintf(output, "Total Freed: %zu\n", pool->stats.total_freed);
    fprintf(output, "Current Usage: %zu\n", pool->stats.current_usage);
    fprintf(output, "Peak Usage: %zu\n", pool->stats.peak_usage);
    fprintf(output, "Allocation Count: %zu\n", pool->stats.allocation_count);
    fprintf(output, "Free Count: %zu\n", pool->stats.free_count);
    fprintf(output, "Cache Hits: %zu\n", pool->stats.cache_hits);
    fprintf(output, "Cache Misses: %zu\n", pool->stats.cache_misses);
    fprintf(output, "Slab Count: %zu\n", pool->stats.slab_count);
    fprintf(output, "Cache Count: %zu\n", pool->stats.cache_count);
    fprintf(output, "\n=== Slab Analysis ===\n");
    
    for (int i = 0; i < YQ_MEMPOOL_SLAB_COUNT; i++) {
        if (pool->slabs[i]) {
            fprintf(output, "Slab %d (Size %zu): %zu free, %zu objects\n", 
                   i, pool->slabs[i]->object_size, 
                   pool->slabs[i]->free_count, 
                   (pool->slabs[i]->slab_size / pool->slabs[i]->object_size));
        }
    }
    
    fprintf(output, "\n=== Cache Analysis ===\n");
    for (int i = 0; i < YQ_MEMPOOL_SLAB_COUNT; i++) {
        if (pool->caches[i]) {
            fprintf(output, "Cache %d (Size %zu): %zu/%zu used\n", 
                   i, pool->caches[i]->object_size,
                   pool->caches[i]->cache_count, 
                   pool->caches[i]->cache_capacity);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 * 内部函数实现
 * ═══════════════════════════════════════════════════════════════════════ */

static yq_mempool_slab *yq_mempool_slab_create(yq_mempool *pool, size_t object_size) {
    yq_mempool_slab *slab = calloc(1, sizeof(yq_mempool_slab));
    if (!slab) return NULL;
    
    /* 分配slab内存 */
    slab->slab_size = pool->slab_size;
    slab->object_size = object_size;
    
#ifdef _WIN32
    slab->slab_base = (uint8_t *)VirtualAlloc(NULL, slab->slab_size, 
                                              MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    slab->slab_base = (uint8_t *)mmap(NULL, slab->slab_size, 
                                      PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
    
    if (!slab->slab_base) {
        free(slab);
        return NULL;
    }
    
    /* 初始化bitmap */
    size_t bitmap_size = (slab->slab_size / object_size + 63) / 64;
    for (size_t i = 0; i < bitmap_size; i++) {
        slab->free_bitmap[i] = 0xFFFF;
    }
    
    /* 计算空闲数量 */
    slab->free_count = slab->slab_size / object_size;
    slab->generation = 1;
    slab->ref_count = 0;
    
    return slab;
}

static void yq_mempool_slab_destroy(yq_mempool_slab *slab) {
    if (!slab) return;
    
    /* 释放slab内存 */
    if (slab->slab_base) {
#ifdef _WIN32
        VirtualFree(slab->slab_base, 0, MEM_RELEASE);
#else
        munmap(slab->slab_base, slab->slab_size);
#endif
    }
    
    /* 清零敏感数据 */
    yq_security_zero(slab, sizeof(*slab));
    free(slab);
}

static void *yq_mempool_slab_alloc(yq_mempool_slab *slab) {
    if (!slab || slab->free_count == 0) return NULL;
    
    /* 查找空闲对象 */
    for (size_t i = 0; i < sizeof(slab->free_bitmap) / sizeof(slab->free_bitmap[0]); i++) {
        if (slab->free_bitmap[i] != 0) {
            /* 找到空闲位 */
            int bit = __builtin_ffs(slab->free_bitmap[i]) - 1;
            slab->free_bitmap[i] &= ~(1 << bit);
            
            /* 计算对象地址 */
            size_t object_index = i * 64 + bit;
            void *ptr = slab->slab_base + object_index * slab->object_size;
            
            slab->free_count--;
            slab->ref_count++;
            
            return ptr;
        }
    }
    
    return NULL;
}

static void yq_mempool_slab_free(yq_mempool_slab *slab, void *ptr) {
    if (!slab || !ptr) return;
    
    /* 计算对象索引 */
    ptrdiff_t offset = (uint8_t *)ptr - slab->slab_base;
    if (offset < 0 || offset >= slab->slab_size) {
        return;
    }
    
    size_t object_index = offset / slab->object_size;
    if (object_index * slab->object_size != offset) {
        return;  /* 未对齐 */
    }
    
    /* 计算bitmap位置 */
    size_t bitmap_index = object_index / 64;
    size_t bit_index = object_index % 64;
    
    /* 检查是否已分配 */
    if (slab->free_bitmap[bitmap_index] & (1 << bit_index)) {
        return;  /* 已空闲 */
    }
    
    /* 标记为空闲 */
    slab->free_bitmap[bitmap_index] |= (1 << bit_index);
    slab->free_count++;
    slab->ref_count--;
}

static yq_mempool_cache *yq_mempool_cache_create(size_t object_size, size_t capacity) {
    yq_mempool_cache *cache = calloc(1, sizeof(yq_mempool_cache));
    if (!cache) return NULL;
    
    cache->object_size = object_size;
    cache->cache_capacity = capacity;
    cache->cache_count = 0;
    cache->next = NULL;
    
    return cache;
}

static void yq_mempool_cache_destroy(yq_mempool_cache *cache) {
    if (!cache) return;
    
    /* 释放所有缓存对象 */
    for (size_t i = 0; i < cache->cache_count; i++) {
        if (cache->objects[i]) {
            yq_security_zero(cache->objects[i], cache->object_size);
        }
    }
    
    /* 清零敏感数据 */
    yq_security_zero(cache, sizeof(*cache));
    free(cache);
}

static void *yq_mempool_cache_alloc(yq_mempool_cache *cache) {
    if (!cache || cache->cache_count == 0) return NULL;
    
    /* 从缓存末尾取出 */
    cache->cache_count--;
    void *ptr = cache->objects[cache->cache_count];
    cache->objects[cache->cache_count] = NULL;
    
    return ptr;
}

static void yq_mempool_cache_free(yq_mempool_cache *cache, void *ptr) {
    if (!cache || !ptr) return;
    
    /* 检查缓存是否已满 */
    if (cache->cache_count >= cache->cache_capacity) {
        return;
    }
    
    /* 添加到缓存 */
    cache->objects[cache->cache_count] = ptr;
    cache->cache_count++;
}

static void yq_mempool_cache_shrink(yq_mempool_cache *cache, size_t target_size) {
    if (!cache) return;
    
    /* 释放多余的对象 */
    while (cache->cache_count > target_size) {
        cache->cache_count--;
        void *ptr = cache->objects[cache->cache_count];
        if (ptr) {
            yq_security_zero(ptr, cache->object_size);
        }
        cache->objects[cache->cache_count] = NULL;
    }
}

static void yq_mempool_slab_shrink(yq_mempool_slab *slab, size_t target_size) {
    if (!slab) return;
    
    /* 缩减slab大小（这里简化处理，实际可能需要更复杂的逻辑） */
    if (slab->free_count > target_size) {
        /* 释放slab */
        yq_mempool_slab_destroy(slab);
    }
}