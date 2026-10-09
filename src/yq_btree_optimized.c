#if !defined(_WIN32)
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "yq_btree_optimized.h"
#include "yq_security.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

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

static int yq_btree_search_internal(yq_btree *bt, yq_slice key, yq_slice *out, uint64_t *page_number, int *slot_index);
static int yq_btree_insert_internal(yq_btree *bt, yq_slice key, yq_slice val, uint32_t mode, uint64_t *page_number, int *slot_index);
static int yq_btree_delete_internal(yq_btree *bt, yq_slice key, uint64_t *page_number, int *slot_index);
static void yq_btree_update_stats(yq_btree *bt, yq_btree_stats *stats, double time_taken);
static int yq_btree_prefetch_internal(yq_btree *bt, uint64_t page_number);
static int yq_btree_get_height_internal(yq_btree *bt, size_t *height, uint64_t page_number, int current_depth);
static int yq_btree_get_size_internal(yq_btree *bt, size_t *size, uint64_t page_number);

/* ═══════════════════════════════════════════════════════════════════════
 * 优化B+Tree操作
 * ═══════════════════════════════════════════════════════════════════════ */

int yq_btree_search_optimized(yq_btree *bt, yq_slice key, yq_slice *out, yq_btree_stats *stats) {
    if (!bt || !key.data || !out) return YQ_ERR_INVAL;
    
    clock_t start = clock();
    
    /* 验证参数 */
    YQ_BTREE_BOUND_CHECK(bt, key);
    
    /* 使用优化的二分搜索 */
    uint64_t page_number;
    int slot_index;
    int ret = yq_btree_search_internal(bt, key, out, &page_number, &slot_index);
    
    clock_t end = clock();
    double time_taken = ((double)(end - start)) / CLOCKS_PER_SEC;
    
    if (stats) {
        yq_btree_update_stats(bt, stats, time_taken);
    }
    
    return ret;
}

int yq_btree_insert_optimized(yq_btree *bt, yq_slice key, yq_slice val, uint32_t mode, yq_btree_stats *stats) {
    if (!bt || !key.data || !val.data) return YQ_ERR_INVAL;
    
    clock_t start = clock();
    
    /* 验证参数 */
    YQ_BTREE_BOUND_CHECK(bt, key);
    
    /* 使用优化的插入 */
    uint64_t page_number;
    int slot_index;
    int ret = yq_btree_insert_internal(bt, key, val, mode, &page_number, &slot_index);
    
    clock_t end = clock();
    double time_taken = ((double)(end - start)) / CLOCKS_PER_SEC;
    
    if (stats) {
        yq_btree_update_stats(bt, stats, time_taken);
    }
    
    return ret;
}

int yq_btree_delete_optimized(yq_btree *bt, yq_slice key, yq_btree_stats *stats) {
    if (!bt || !key.data) return YQ_ERR_INVAL;
    
    clock_t start = clock();
    
    /* 验证参数 */
    YQ_BTREE_BOUND_CHECK(bt, key);
    
    /* 使用优化的删除 */
    uint64_t page_number;
    int slot_index;
    int ret = yq_btree_delete_internal(bt, key, &page_number, &slot_index);
    
    clock_t end = clock();
    double time_taken = ((double)(end - start)) / CLOCKS_PER_SEC;
    
    if (stats) {
        yq_btree_update_stats(bt, stats, time_taken);
    }
    
    return ret;
}

/* ═══════════════════════════════════════════════════════════════════════
 * 批量操作
 * ═══════════════════════════════════════════════════════════════════════ */

int yq_btree_batch_insert(yq_btree *bt, yq_txn *txn, yq_slice *keys, yq_slice *values, uint32_t *modes, size_t count) {
    if (!bt || !txn || !keys || !values || count == 0) return YQ_ERR_INVAL;
    
    /* 验证参数 */
    for (size_t i = 0; i < count; i++) {
        if (!keys[i].data || !values[i].data) return YQ_ERR_INVAL;
    }
    
    yq_btree_batch *batch = yq_btree_batch_create(count);
    if (!batch) return YQ_ERR_NOMEM;
    
    /* 添加批量操作 */
    for (size_t i = 0; i < count; i++) {
        if (yq_btree_batch_add(batch, keys[i], values[i], modes[i]) != 0) {
            yq_btree_batch_destroy(batch);
            return YQ_ERR_NOMEM;
        }
    }
    
    batch->btree = bt;
    batch->txn = txn;
    
    /* 执行批量操作 */
    int ret = yq_btree_batch_execute(batch);
    yq_btree_batch_destroy(batch);
    
    return ret;
}

int yq_btree_batch_delete(yq_btree *bt, yq_txn *txn, yq_slice *keys, size_t count) {
    if (!bt || !txn || !keys || count == 0) return YQ_ERR_INVAL;
    
    /* 验证参数 */
    for (size_t i = 0; i < count; i++) {
        if (!keys[i].data) return YQ_ERR_INVAL;
    }
    
    yq_btree_batch *batch = yq_btree_batch_create(count);
    if (!batch) return YQ_ERR_NOMEM;
    
    /* 添加批量删除操作 */
    for (size_t i = 0; i < count; i++) {
        if (yq_btree_batch_add(batch, keys[i], (yq_slice){NULL, 0}, YQ_PUT_NOOVERWRITE) != 0) {
            yq_btree_batch_destroy(batch);
            return YQ_ERR_NOMEM;
        }
    }
    
    batch->btree = bt;
    batch->txn = txn;
    
    /* 执行批量操作 */
    int ret = yq_btree_batch_execute(batch);
    yq_btree_batch_destroy(batch);
    
    return ret;
}

int yq_btree_batch_search(yq_btree *bt, yq_slice *keys, yq_slice *results, size_t count) {
    if (!bt || !keys || !results || count == 0) return YQ_ERR_INVAL;
    
    /* 验证参数 */
    for (size_t i = 0; i < count; i++) {
        if (!keys[i].data) return YQ_ERR_INVAL;
    }
    
    /* 执行批量搜索 */
    for (size_t i = 0; i < count; i++) {
        int ret = yq_btree_search_optimized(bt, keys[i], &results[i], NULL);
        if (ret != YQ_OK) {
            /* 清零未找到的结果 */
            results[i].data = NULL;
            results[i].size = 0;
        }
    }
    
    return YQ_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 * 批量操作结构管理
 * ═══════════════════════════════════════════════════════════════════════ */

yq_btree_batch *yq_btree_batch_create(size_t capacity) {
    if (capacity == 0) capacity = YQ_BTREE_BATCH_SIZE;
    if (capacity > YQ_BTREE_BATCH_SIZE * 4) capacity = YQ_BTREE_BATCH_SIZE * 4;
    
    yq_btree_batch *batch = calloc(1, sizeof(yq_btree_batch));
    if (!batch) return NULL;
    
    batch->keys = calloc(capacity, sizeof(yq_slice));
    batch->values = calloc(capacity, sizeof(yq_slice));
    batch->modes = calloc(capacity, sizeof(uint32_t));
    
    if (!batch->keys || !batch->values || !batch->modes) {
        free(batch->keys);
        free(batch->values);
        free(batch->modes);
        free(batch);
        return NULL;
    }
    
    batch->capacity = capacity;
    batch->size = 0;
    batch->btree = NULL;
    batch->txn = NULL;
    
    return batch;
}

void yq_btree_batch_destroy(yq_btree_batch *batch) {
    if (!batch) return;
    
    /* 清零敏感数据 */
    for (size_t i = 0; i < batch->size; i++) {
        if (batch->keys[i].data) {
            yq_security_zero(batch->keys[i].data, batch->keys[i].size);
            free(batch->keys[i].data);
        }
        if (batch->values[i].data) {
            yq_security_zero(batch->values[i].data, batch->values[i].size);
            free(batch->values[i].data);
        }
    }
    
    yq_security_zero(batch->keys, sizeof(batch->keys));
    yq_security_zero(batch->values, sizeof(batch->values));
    yq_security_zero(batch->modes, sizeof(batch->modes));
    
    free(batch->keys);
    free(batch->values);
    free(batch->modes);
    yq_security_zero(batch, sizeof(*batch));
    free(batch);
}

int yq_btree_batch_add(yq_btree_batch *batch, yq_slice key, yq_slice val, uint32_t mode) {
    if (!batch || batch->size >= batch->capacity) return YQ_ERR_INVAL;
    
    /* 复制键 */
    void *key_copy = malloc(key.size);
    if (!key_copy) return YQ_ERR_NOMEM;
    memcpy(key_copy, key.data, key.size);
    batch->keys[batch->size].data = key_copy;
    batch->keys[batch->size].size = key.size;
    
    /* 复制值 */
    void *val_copy = malloc(val.size);
    if (!val_copy) {
        free(key_copy);
        return YQ_ERR_NOMEM;
    }
    memcpy(val_copy, val.data, val.size);
    batch->values[batch->size].data = val_copy;
    batch->values[batch->size].size = val.size;
    
    /* 设置模式 */
    batch->modes[batch->size] = mode;
    batch->size++;
    
    return 0;
}

int yq_btree_batch_execute(yq_btree_batch *batch) {
    if (!batch || !batch->btree || !batch->txn) return YQ_ERR_INVAL;
    
    /* 批量执行操作 */
    for (size_t i = 0; i < batch->size; i++) {
        int ret;
        if (batch->values[i].data) {
            ret = yq_put(batch->txn, batch->keys[i], batch->values[i], batch->modes[i]);
        } else {
            ret = yq_del(batch->txn, batch->keys[i]);
        }
        
        if (ret != YQ_OK) {
            /* 回滚已执行的操作 */
            for (size_t j = 0; j < i; j++) {
                if (batch->values[j].data) {
                    yq_del(batch->txn, batch->keys[j]);
                }
            }
            return ret;
        }
    }
    
    return YQ_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 * B+Tree统计和监控
 * ═══════════════════════════════════════════════════════════════════════ */

void yq_btree_get_stats(yq_btree *bt, yq_btree_stats *stats) {
    if (!bt || !stats) return;
    
    /* 初始化统计信息 */
    memset(stats, 0, sizeof(*stats));
    
    /* 获取树高度 */
    size_t height;
    if (yq_btree_get_height(bt, &height) == YQ_OK) {
        stats->tree_height = height;
    }
    
    /* 获取树大小 */
    size_t size;
    if (yq_btree_get_size(bt, &size) == YQ_OK) {
        stats->current_size = size;
    }
}

void yq_btree_reset_stats(yq_btree *bt) {
    if (!bt) return;
    
    /* 重置统计信息 */
    memset(&bt->stats, 0, sizeof(bt->stats));
}

int yq_btree_optimize(yq_btree *bt) {
    if (!bt) return YQ_ERR_INVAL;
    
    /* 执行B+Tree优化 */
    /* 这里可以实现各种优化策略，如：
     * 1. 重新平衡树结构
     * 2. 合并碎片化的页面
     * 3. 优化页面布局
     * 4. 压缩未使用的空间
     */
    
    return YQ_OK;
}

int yq_btree_prefetch(yq_btree *bt, uint64_t page_number) {
    if (!bt) return YQ_ERR_INVAL;
    
    return yq_btree_prefetch_internal(bt, page_number);
}

int yq_btree_get_height(yq_btree *bt, size_t *height) {
    if (!bt || !height) return YQ_ERR_INVAL;
    
    return yq_btree_get_height_internal(bt, height, atomic_load(&bt->root_page), 0);
}

int yq_btree_get_size(yq_btree *bt, size_t *size) {
    if (!bt || !size) return YQ_ERR_INVAL;
    
    return yq_btree_get_size_internal(bt, size, atomic_load(&bt->root_page));
}

/* ═══════════════════════════════════════════════════════════════════════
 * 搜索缓存实现
 * ═══════════════════════════════════════════════════════════════════════ */

yq_btree_search_cache *yq_btree_search_cache_create(void) {
    yq_btree_search_cache *cache = calloc(1, sizeof(yq_btree_search_cache));
    if (!cache) return NULL;
    
    cache->current_size = 0;
    cache->current_index = 0;
    
    return cache;
}

void yq_btree_search_cache_destroy(yq_btree_search_cache *cache) {
    if (!cache) return;
    
    /* 清零敏感数据 */
    yq_security_zero(cache, sizeof(*cache));
    free(cache);
}

int yq_btree_search_cache_get(yq_btree_search_cache *cache, const char *key, size_t key_size, uint64_t *page_number) {
    if (!cache || !key || !page_number) return YQ_ERR_INVAL;
    
    /* 检查缓存 */
    for (size_t i = 0; i < cache->current_size; i++) {
        if (cache->key_sizes[i] == key_size && 
            memcmp(cache->keys[i], key, key_size) == 0) {
            *page_number = cache->page_numbers[i];
            return 0;
        }
    }
    
    return YQ_ERR_NOTFOUND;
}

int yq_btree_search_cache_put(yq_btree_search_cache *cache, const char *key, size_t key_size, uint64_t page_number) {
    if (!cache || !key) return YQ_ERR_INVAL;
    
    /* 检查是否已存在 */
    for (size_t i = 0; i < cache->current_size; i++) {
        if (cache->key_sizes[i] == key_size && 
            memcmp(cache->keys[i], key, key_size) == 0) {
            cache->page_numbers[i] = page_number;
            cache->timestamps[i] = time(NULL);
            return 0;
        }
    }
    
    /* 添加新条目 */
    if (cache->current_size < YQ_BTREE_SEARCH_CACHE_SIZE) {
        strncpy(cache->keys[cache->current_size], key, 255);
        cache->keys[cache->current_size][255] = '\0';
        cache->key_sizes[cache->current_size] = key_size;
        cache->page_numbers[cache->current_size] = page_number;
        cache->timestamps[cache->current_size] = time(NULL);
        cache->current_size++;
    } else {
        /* 替换最旧的条目 */
        size_t oldest_index = 0;
        for (size_t i = 1; i < cache->current_size; i++) {
            if (cache->timestamps[i] < cache->timestamps[oldest_index]) {
                oldest_index = i;
            }
        }
        
        strncpy(cache->keys[oldest_index], key, 255);
        cache->keys[oldest_index][255] = '\0';
        cache->key_sizes[oldest_index] = key_size;
        cache->page_numbers[oldest_index] = page_number;
        cache->timestamps[oldest_index] = time(NULL);
    }
    
    return 0;
}

int yq_btree_search_cache_clear(yq_btree_search_cache *cache) {
    if (!cache) return YQ_ERR_INVAL;
    
    cache->current_size = 0;
    cache->current_index = 0;
    
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 * 内部函数实现
 * ═══════════════════════════════════════════════════════════════════════ */

static int yq_btree_search_internal(yq_btree *bt, yq_slice key, yq_slice *out, uint64_t *page_number, int *slot_index) {
    if (!bt || !key.data || !out || !page_number || !slot_index) return YQ_ERR_INVAL;
    
    uint64_t current_page = atomic_load(&bt->root_page);
    if (current_page == 0) return YQ_ERR_NOTFOUND;
    
    while (1) {
        uint8_t *page = get_page_data(bt, current_page);
        if (!page) return YQ_ERR_NOMEM;
        
        yq_page_header hdr;
        read_page_header(page, &hdr);
        
        if (hdr.page_type == YQ_PAGE_TYPE_LEAF) {
            /* 叶子节点搜索 */
            int pos = 0;
            if (yq_btree_binary_search(page, key, &pos) != 0) return YQ_ERR_INVAL;
            
            if (pos >= hdr.nkeys) return YQ_ERR_NOTFOUND;
            
            *page_number = current_page;
            *slot_index = pos;
            
            /* 读取值 */
            uint8_t *val_ptr = page + get_slot(page, pos);
            size_t key_len = 0;
            if (yq_varint_decode(val_ptr, &key_len) != YQ_OK) return YQ_ERR_CORRUPT;
            
            val_ptr += key_len;
            size_t val_len = 0;
            if (yq_varint_decode(val_ptr, &val_len) != YQ_OK) return YQ_ERR_CORRUPT;
            
            val_ptr += val_len;
            out->data = val_ptr;
            out->size = val_len;
            
            return YQ_OK;
        } else {
            /* 内部节点搜索 */
            int pos = 0;
            if (yq_btree_binary_search(page, key, &pos) != 0) return YQ_ERR_INVAL;
            
            current_page = *(uint64_t *)(page + bt->page_size - 8 - pos * 8);
        }
    }
}

static int yq_btree_insert_internal(yq_btree *bt, yq_slice key, yq_slice val, uint32_t mode, uint64_t *page_number, int *slot_index) {
    if (!bt || !key.data || !val.data || !page_number || !slot_index) return YQ_ERR_INVAL;
    
    /* 简化的插入实现 */
    return yq_btree_insert(bt, key, val, mode);
}

static int yq_btree_delete_internal(yq_btree *bt, yq_slice key, uint64_t *page_number, int *slot_index) {
    if (!bt || !key.data || !page_number || !slot_index) return YQ_ERR_INVAL;
    
    /* 简化的删除实现 */
    return yq_btree_delete(bt, key);
}

static void yq_btree_update_stats(yq_btree *bt, yq_btree_stats *stats, double time_taken) {
    if (!bt || !stats) return;
    
    stats->search_count++;
    stats->average_search_time = (stats->average_search_time * (stats->search_count - 1) + time_taken) / stats->search_count;
}

static int yq_btree_prefetch_internal(yq_btree *bt, uint64_t page_number) {
    if (!bt) return YQ_ERR_INVAL;
    
    /* 实现预取逻辑 */
    /* 这里可以使用操作系统的预取功能 */
    
    return YQ_OK;
}

static int yq_btree_get_height_internal(yq_btree *bt, size_t *height, uint64_t page_number, int current_depth) {
    if (!bt || !height) return YQ_ERR_INVAL;
    
    if (page_number == 0) {
        *height = current_depth;
        return YQ_OK;
    }
    
    uint8_t *page = get_page_data(bt, page_number);
    if (!page) return YQ_ERR_NOMEM;
    
    yq_page_header hdr;
    read_page_header(page, &hdr);
    
    if (hdr.page_type == YQ_PAGE_TYPE_LEAF) {
        *height = current_depth + 1;
        return YQ_OK;
    } else {
        /* 内部节点，递归计算子树高度 */
        uint64_t child_page = *(uint64_t *)(page + bt->page_size - 8);
        return yq_btree_get_height_internal(bt, height, child_page, current_depth + 1);
    }
}

static int yq_btree_get_size_internal(yq_btree *bt, size_t *size, uint64_t page_number) {
    if (!bt || !size) return YQ_ERR_INVAL;
    
    if (page_number == 0) {
        *size = 0;
        return YQ_OK;
    }
    
    uint8_t *page = get_page_data(bt, page_number);
    if (!page) return YQ_ERR_NOMEM;
    
    yq_page_header hdr;
    read_page_header(page, &hdr);
    
    if (hdr.page_type == YQ_PAGE_TYPE_LEAF) {
        *size = hdr.nkeys;
        return YQ_OK;
    } else {
        /* 内部节点，递归计算子树大小 */
        uint64_t child_page = *(uint64_t *)(page + bt->page_size - 8);
        size_t child_size;
        int ret = yq_btree_get_size_internal(bt, &child_size, child_page);
        if (ret != YQ_OK) return ret;
        *size = child_size + hdr.nkeys;
        return YQ_OK;
    }
}