/*
 * yq_btree_optimized.h — Optimized B+Tree operations for yq-DB
 *
 * 版本   : 1.0.0
 * 语言   : C11
 * 格式版本: 1（见 FORMAT.md）
 *
 * 设计约束（改动本头文件前请先读）：
 *   1. 本头文件提供优化的B+Tree操作
 *   2. 实现二分搜索算法优化
 *   3. 添加批量操作支持
 *   4. 实现预取和缓存优化
 *   5. 提供性能统计和监控
 */

#ifndef YQ_BTREE_OPTIMIZED_H
#define YQ_BTREE_OPTIMIZED_H

#include "yq_btree.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════
 * B+Tree优化配置常量
 * ═══════════════════════════════════════════════════════════════════════ */

#define YQ_BTREE_PREFETCH_DISTANCE    4
#define YQ_BTREE_BATCH_SIZE           32
#define YQ_BTREE_CACHE_SIZE           256
#define YQ_BTREE_OPTIMIZED_THRESHOLD  1000
#define YQ_BTREE_SEARCH_CACHE_SIZE    128

/* ═══════════════════════════════════════════════════════════════════════
 * B+Tree优化统计结构
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct yq_btree_stats {
    size_t search_count;
    size_t search_cache_hits;
    size_t search_cache_misses;
    size_t batch_operations;
    size_t optimization_hits;
    size_t tree_height;
    size_t average_key_size;
    size_t average_value_size;
    double average_search_time;
    double average_insert_time;
    double average_delete_time;
} yq_btree_stats;

/* ═══════════════════════════════════════════════════════════════════════
 * B+Tree搜索缓存结构
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct yq_btree_search_cache {
    char keys[YQ_BTREE_SEARCH_CACHE_SIZE][256];
    uint64_t page_numbers[YQ_BTREE_SEARCH_CACHE_SIZE];
    uint64_t timestamps[YQ_BTREE_SEARCH_CACHE_SIZE];
    size_t key_sizes[YQ_BTREE_SEARCH_CACHE_SIZE];
    size_t current_size;
    size_t current_index;
} yq_btree_search_cache;

/* ═══════════════════════════════════════════════════════════════════════
 * B+Tree批量操作结构
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct yq_btree_batch {
    yq_slice *keys;
    yq_slice *values;
    uint32_t *modes;
    size_t size;
    size_t capacity;
    yq_btree *btree;
    yq_txn *txn;
} yq_btree_batch;

/* ═══════════════════════════════════════════════════════════════════════
 * 优化B+Tree函数
 * ═══════════════════════════════════════════════════════════════════════ */

/* 优化的搜索函数 */
int yq_btree_search_optimized(yq_btree *bt, yq_slice key, yq_slice *out, yq_btree_stats *stats);

/* 优化的插入函数 */
int yq_btree_insert_optimized(yq_btree *bt, yq_slice key, yq_slice val, uint32_t mode, yq_btree_stats *stats);

/* 优化的删除函数 */
int yq_btree_delete_optimized(yq_btree *bt, yq_slice key, yq_btree_stats *stats);

/* 批量插入操作 */
int yq_btree_batch_insert(yq_btree *bt, yq_txn *txn, yq_slice *keys, yq_slice *values, uint32_t *modes, size_t count);

/* 批量删除操作 */
int yq_btree_batch_delete(yq_btree *bt, yq_txn *txn, yq_slice *keys, size_t count);

/* 批量搜索操作 */
int yq_btree_batch_search(yq_btree *bt, yq_slice *keys, yq_slice *results, size_t count);

/* 创建批量操作结构 */
yq_btree_batch *yq_btree_batch_create(size_t capacity);

/* 销毁批量操作结构 */
void yq_btree_batch_destroy(yq_btree_batch *batch);

/* 执行批量操作 */
int yq_btree_batch_execute(yq_btree_batch *batch);

/* 添加批量操作项 */
int yq_btree_batch_add(yq_btree_batch *batch, yq_slice key, yq_slice val, uint32_t mode);

/* 获取B+Tree统计信息 */
void yq_btree_get_stats(yq_btree *bt, yq_btree_stats *stats);

/* 重置B+Tree统计信息 */
void yq_btree_reset_stats(yq_btree *bt);

/* 优化B+Tree结构 */
int yq_btree_optimize(yq_btree *bt);

/* 预取B+Tree页面 */
int yq_btree_prefetch(yq_btree *bt, uint64_t page_number);

/* 获取B+Tree高度 */
int yq_btree_get_height(yq_btree *bt, size_t *height);

/* 获取B+Tree大小 */
int yq_btree_get_size(yq_btree *bt, size_t *size);

/* ═══════════════════════════════════════════════════════════════════════
 * B+Tree搜索缓存函数
 * ═══════════════════════════════════════════════════════════════════════ */

/* 创建搜索缓存 */
yq_btree_search_cache *yq_btree_search_cache_create(void);

/* 销毁搜索缓存 */
void yq_btree_search_cache_destroy(yq_btree_search_cache *cache);

/* 搜索缓存操作 */
int yq_btree_search_cache_get(yq_btree_search_cache *cache, const char *key, size_t key_size, uint64_t *page_number);

int yq_btree_search_cache_put(yq_btree_search_cache *cache, const char *key, size_t key_size, uint64_t page_number);

int yq_btree_search_cache_clear(yq_btree_search_cache *cache);

/* ═══════════════════════════════════════════════════════════════════════
 * B+Tree优化宏
 * ═══════════════════════════════════════════════════════════════════════ */

/* 快速批量操作 */
#define yq_btree_batch_new(bt, txn, count) \
    yq_btree_batch_create(count)

#define yq_btree_batch_free(batch) \
    yq_btree_batch_destroy(batch)

#define yq_btree_batch_add_put(batch, key, val) \
    yq_btree_batch_add(batch, key, val, 0)

#define yq_btree_batch_add_delete(batch, key) \
    yq_btree_batch_add(batch, key, (yq_slice){NULL, 0}, YQ_PUT_NOOVERWRITE)

/* ═══════════════════════════════════════════════════════════════════════
 * B+Tree优化工具函数
 * ═══════════════════════════════════════════════════════════════════════ */

/* 优化的二分搜索 */
static inline int yq_btree_binary_search(const uint8_t *page, yq_slice key, int *out_pos) {
    if (!page || !key.data || out_pos) return -1;
    
    yq_page_header hdr;
    read_page_header(page, &hdr);
    
    int low = 0;
    int high = hdr.nkeys - 1;
    
    while (low <= high) {
        int mid = low + (high - low) / 2;
        uint8_t *key_ptr = page + get_slot(page, mid);
        size_t key_len = 0;
        
        /* 优化的键读取 */
        if (yq_varint_decode(key_ptr, &key_len) != YQ_OK) return -1;
        
        /* 比较键 */
        int cmp = memcmp(key_ptr + key_len, key.data, key.size > key_len ? key_len : key.size);
        if (cmp == 0 && key.size == key_len) {
            *out_pos = mid;
            return 0;
        } else if (cmp < 0) {
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }
    
    *out_pos = low;
    return 0;
}

/* 验证B+Tree参数 */
static inline bool yq_btree_validate_params(yq_btree *bt, yq_slice key) {
    if (!bt || !key.data) return false;
    if (key.size == 0 || key.size > 1024) return false;
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════
 * B+Tree调试
 * ═══════════════════════════════════════════════════════════════════════ */

/* B+Tree断言 */
#define YQ_BTREE_ASSERT(bt, key) \
    do { \
        if (!(bt) || !(key).data) { \
            return YQ_ERR_INVAL; \
        } \
    } while (0)

/* B+Tree边界检查 */
#define YQ_BTREE_BOUND_CHECK(bt, key) \
    do { \
        if (!(bt) || !(key).data || (key).size == 0 || (key).size > 1024) { \
            return YQ_ERR_INVAL; \
        } \
    } while (0)

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* YQ_BTREE_OPTIMIZED_H */