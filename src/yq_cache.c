#if !defined(_WIN32)
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "yq_cache.h"
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

static yq_cache_node *yq_cache_node_create(const char *key, const void *value, size_t value_size, uint64_t ttl);
static void yq_cache_node_destroy(yq_cache_node *node);
static void yq_cache_remove_node(yq_cache *cache, yq_cache_node *node);
static void yq_cache_add_node(yq_cache *cache, yq_cache_node *node);
static void yq_cache_evict_node(yq_cache *cache);
static void yq_cache_update_lru(yq_cache *cache, yq_cache_node *node);
static void yq_cache_update_lfu(yq_cache *cache, yq_cache_node *node);
static void yq_cache_cleanup_expired(yq_cache *cache);
static bool yq_cache_is_expired(yq_cache_node *node);
static uint32_t yq_cache_get_hash_index(yq_cache *cache, const char *key);
static yq_cache_node *yq_cache_find_node(yq_cache *cache, const char *key);

/* ═══════════════════════════════════════════════════════════════════════
 * 缓存创建和销毁
 * ═══════════════════════════════════════════════════════════════════════ */

yq_cache *yq_cache_create(size_t capacity, yq_cache_policy policy) {
    if (capacity == 0) capacity = YQ_CACHE_DEFAULT_SIZE;
    if (capacity > YQ_CACHE_MAX_SIZE) capacity = YQ_CACHE_MAX_SIZE;
    
    yq_cache *cache = calloc(1, sizeof(yq_cache));
    if (!cache) return NULL;
    
    /* 初始化哈希表 */
    cache->hash_size = capacity * 2;  /* 双倍哈希表大小以减少冲突 */
    cache->hash_table = calloc(cache->hash_size, sizeof(yq_cache_node *));
    if (!cache->hash_table) {
        free(cache);
        return NULL;
    }
    
    /* 初始化缓存参数 */
    cache->capacity = capacity;
    cache->policy = policy;
    cache->generation = 1;
    cache->active_writers = 0;
    cache->auto_cleanup = true;
    cache->cleanup_interval = 300;  /* 5分钟清理间隔 */
    cache->last_cleanup = time(NULL);
    
    /* 初始化统计信息 */
    memset(&cache->stats, 0, sizeof(cache->stats));
    
    return cache;
}

void yq_cache_destroy(yq_cache *cache) {
    if (!cache) return;
    
    /* 标记为正在销毁 */
    cache->generation = 0xDEADBEEF;
    memory_barrier();
    
    /* 清空所有缓存项 */
    yq_cache_clear(cache);
    
    /* 释放哈希表 */
    if (cache->hash_table) {
        free(cache->hash_table);
    }
    
    /* 清零敏感数据 */
    yq_security_zero(cache, sizeof(*cache));
    free(cache);
}

/* ═══════════════════════════════════════════════════════════════════════
 * 缓存操作
 * ═══════════════════════════════════════════════════════════════════════ */

void *yq_cache_get(yq_cache *cache, const char *key, size_t *value_size) {
    if (!cache || !key) return NULL;
    
    /* 验证缓存状态 */
    if (cache->generation != 1) {
        return NULL;
    }
    
    /* 验证键 */
    if (!yq_cache_validate_key(key)) {
        return NULL;
    }
    
    /* 查找缓存项 */
    yq_cache_node *node = yq_cache_find_node(cache, key);
    if (!node) {
        cache->stats.misses++;
        return NULL;
    }
    
    /* 检查是否过期 */
    if (yq_cache_is_expired(node)) {
        yq_cache_remove_node(cache, node);
        yq_cache_node_destroy(node);
        cache->stats.misses++;
        cache->stats.expired++;
        return NULL;
    }
    
    /* 更新访问统计 */
    node->access_time = time(NULL);
    node->access_count++;
    
    /* 根据策略更新缓存 */
    switch (cache->policy) {
        case YQ_CACHE_LRU:
            yq_cache_update_lru(cache, node);
            break;
        case YQ_CACHE_LFU:
            yq_cache_update_lfu(cache, node);
            break;
        default:
            break;
    }
    
    /* 更新统计 */
    cache->stats.hits++;
    cache->stats.current_size++;
    if (cache->stats.current_size > cache->stats.peak_size) {
        cache->stats.peak_size = cache->stats.current_size;
    }
    
    /* 返回值 */
    if (value_size) {
        *value_size = node->value_size;
    }
    
    return node->value;
}

int yq_cache_set(yq_cache *cache, const char *key, const void *value, size_t value_size, uint64_t ttl) {
    if (!cache || !key || (!value && value_size > 0)) return -1;
    
    /* 验证缓存状态 */
    if (cache->generation != 1) {
        return -1;
    }
    
    /* 验证参数 */
    if (!yq_cache_validate_key(key) || !yq_cache_validate_value(value, value_size)) {
        return -1;
    }
    
    /* 检查容量限制 */
    if (cache->current_size >= cache->capacity) {
        yq_cache_evict_node(cache);
    }
    
    /* 计算过期时间 */
    uint64_t expiration = 0;
    if (ttl > 0) {
        expiration = time(NULL) + ttl;
    }
    
    /* 创建新节点 */
    yq_cache_node *node = yq_cache_node_create(key, value, value_size, expiration);
    if (!node) return -1;
    
    /* 检查是否已存在 */
    uint32_t hash_index = yq_cache_get_hash_index(cache, key);
    yq_cache_node *existing = yq_cache_find_node(cache, key);
    
    if (existing) {
        /* 删除现有节点 */
        yq_cache_remove_node(cache, existing);
        yq_cache_node_destroy(existing);
        cache->stats.updates++;
    } else {
        cache->stats.insertions++;
    }
    
    /* 添加新节点 */
    yq_cache_add_node(cache, node);
    
    return 0;
}

int yq_cache_delete(yq_cache *cache, const char *key) {
    if (!cache || !key) return -1;
    
    /* 验证缓存状态 */
    if (cache->generation != 1) {
        return -1;
    }
    
    /* 验证键 */
    if (!yq_cache_validate_key(key)) {
        return -1;
    }
    
    /* 查找并删除节点 */
    yq_cache_node *node = yq_cache_find_node(cache, key);
    if (!node) return -1;
    
    yq_cache_remove_node(cache, node);
    yq_cache_node_destroy(node);
    
    cache->stats.deletions++;
    cache->stats.current_size--;
    
    return 0;
}

void yq_cache_clear(yq_cache *cache) {
    if (!cache) return;
    
    /* 清空哈希表 */
    for (size_t i = 0; i < cache->hash_size; i++) {
        yq_cache_node *node = cache->hash_table[i];
        while (node) {
            yq_cache_node *next = node->hash_next;
            yq_cache_node_destroy(node);
            node = next;
        }
        cache->hash_table[i] = NULL;
    }
    
    /* 清空链表 */
    cache->head = NULL;
    cache->tail = NULL;
    
    /* 重置统计 */
    cache->current_size = 0;
    cache->stats.current_size = 0;
}

void yq_cache_get_stats(yq_cache *cache, yq_cache_stats *stats) {
    if (!cache || !stats) return;
    
    /* 复制统计信息 */
    memcpy(stats, &cache->stats, sizeof(*stats));
    
    /* 计算命中率 */
    if (cache->stats.hits + cache->stats.misses > 0) {
        stats->hit_ratio = (double)cache->stats.hits / (cache->stats.hits + cache->stats.misses);
    }
    
    /* 计算平均访问时间 */
    if (cache->stats.hits > 0) {
        stats->avg_access_time = (double)cache->stats.access_time / cache->stats.hits;
    }
}

void yq_cache_reset_stats(yq_cache *cache) {
    if (!cache) return;
    
    memset(&cache->stats, 0, sizeof(cache->stats));
    cache->stats.current_size = cache->current_size;
}

int yq_cache_warmup(yq_cache *cache, const char **keys, size_t key_count, const void **values, size_t *value_sizes) {
    if (!cache || !keys || !values || key_count == 0) return -1;
    
    for (size_t i = 0; i < key_count; i++) {
        if (!keys[i] || !values[i]) continue;
        
        size_t value_size = value_sizes ? value_sizes[i] : strlen(values[i]);
        if (yq_cache_set(cache, keys[i], values[i], value_size, 0) != 0) {
            return -1;
        }
    }
    
    return 0;
}

int yq_cache_set_policy(yq_cache *cache, yq_cache_policy policy) {
    if (!cache) return -1;
    
    cache->policy = policy;
    return 0;
}

void yq_cache_set_auto_cleanup(yq_cache *cache, bool enabled, uint64_t interval) {
    if (!cache) return;
    
    cache->auto_cleanup = enabled;
    if (interval > 0) {
        cache->cleanup_interval = interval;
    }
}

bool yq_cache_exists(yq_cache *cache, const char *key) {
    if (!cache || !key) return false;
    
    yq_cache_node *node = yq_cache_find_node(cache, key);
    if (!node) return false;
    
    if (yq_cache_is_expired(node)) {
        yq_cache_remove_node(cache, node);
        yq_cache_node_destroy(node);
        cache->stats.expired++;
        return false;
    }
    
    return true;
}

uint64_t yq_cache_get_ttl(yq_cache *cache, const char *key) {
    if (!cache || !key) return 0;
    
    yq_cache_node *node = yq_cache_find_node(cache, key);
    if (!node) return 0;
    
    if (yq_cache_is_expired(node)) {
        yq_cache_remove_node(cache, node);
        yq_cache_node_destroy(node);
        cache->stats.expired++;
        return 0;
    }
    
    if (node->expiration == 0) return 0;  /* 永不过期 */
    
    uint64_t current_time = time(NULL);
    if (node->expiration <= current_time) return 0;
    
    return node->expiration - current_time;
}

int yq_cache_set_ttl(yq_cache *cache, const char *key, uint64_t ttl) {
    if (!cache || !key) return -1;
    
    yq_cache_node *node = yq_cache_find_node(cache, key);
    if (!node) return -1;
    
    if (ttl > 0) {
        node->expiration = time(NULL) + ttl;
    } else {
        node->expiration = 0;  /* 永不过期 */
    }
    
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 * 内部函数实现
 * ═══════════════════════════════════════════════════════════════════════ */

static yq_cache_node *yq_cache_node_create(const char *key, const void *value, size_t value_size, uint64_t ttl) {
    yq_cache_node *node = calloc(1, sizeof(yq_cache_node));
    if (!node) return NULL;
    
    /* 复制键 */
    strncpy(node->key, key, YQ_CACHE_MAX_KEY_SIZE - 1);
    node->key[YQ_CACHE_MAX_KEY_SIZE - 1] = '\0';
    
    /* 复制值 */
    if (value && value_size > 0) {
        node->value = malloc(value_size);
        if (!node->value) {
            free(node);
            return NULL;
        }
        memcpy(node->value, value, value_size);
        node->value_size = value_size;
    }
    
    /* 设置过期时间 */
    node->expiration = ttl;
    
    /* 初始化访问统计 */
    node->access_time = time(NULL);
    node->access_count = 0;
    
    return node;
}

static void yq_cache_node_destroy(yq_cache_node *node) {
    if (!node) return;
    
    /* 释放值 */
    if (node->value) {
        yq_security_zero(node->value, node->value_size);
        free(node->value);
    }
    
    /* 清零敏感数据 */
    yq_security_zero(node, sizeof(*node));
    free(node);
}

static void yq_cache_remove_node(yq_cache *cache, yq_cache_node *node) {
    if (!cache || !node) return;
    
    /* 从哈希表中移除 */
    uint32_t hash_index = yq_cache_get_hash_index(cache, node->key);
    yq_cache_node *current = cache->hash_table[hash_index];
    yq_cache_node *prev = NULL;
    
    while (current) {
        if (current == node) {
            if (prev) {
                prev->hash_next = current->hash_next;
            } else {
                cache->hash_table[hash_index] = current->hash_next;
            }
            break;
        }
        prev = current;
        current = current->hash_next;
    }
    
    /* 从链表中移除 */
    if (node->prev) {
        node->prev->next = node->next;
    } else {
        cache->head = node->next;
    }
    
    if (node->next) {
        node->next->prev = node->prev;
    } else {
        cache->tail = node->prev;
    }
    
    cache->current_size--;
}

static void yq_cache_add_node(yq_cache *cache, yq_cache_node *node) {
    if (!cache || !node) return;
    
    /* 添加到哈希表 */
    uint32_t hash_index = yq_cache_get_hash_index(cache, node->key);
    node->hash_next = cache->hash_table[hash_index];
    cache->hash_table[hash_index] = node;
    
    /* 添加到链表头部 */
    node->next = cache->head;
    node->prev = NULL;
    
    if (cache->head) {
        cache->head->prev = node;
    } else {
        cache->tail = node;
    }
    
    cache->head = node;
    cache->current_size++;
}

static void yq_cache_evict_node(yq_cache *cache) {
    if (!cache || cache->current_size == 0) return;
    
    yq_cache_node *node_to_evict = NULL;
    
    switch (cache->policy) {
        case YQ_CACHE_LRU:
            node_to_evict = cache->tail;
            break;
        case YQ_CACHE_LFU:
            /* 找到访问次数最少的节点 */
            node_to_evict = cache->tail;
            yq_cache_node *current = cache->head;
            while (current) {
                if (current->access_count < node_to_evict->access_count) {
                    node_to_evict = current;
                }
                current = current->next;
            }
            break;
        case YQ_CACHE_FIFO:
            node_to_evict = cache->tail;
            break;
        case YQ_CACHE_RANDOM:
            /* 随机选择一个节点 */
            size_t index = rand() % cache->current_size;
            yq_cache_node *current = cache->head;
            for (size_t i = 0; i < index; i++) {
                current = current->next;
            }
            node_to_evict = current;
            break;
    }
    
    if (node_to_evict) {
        yq_cache_remove_node(cache, node_to_evict);
        yq_cache_node_destroy(node_to_evict);
        cache->stats.evictions++;
    }
}

static void yq_cache_update_lru(yq_cache *cache, yq_cache_node *node) {
    if (!cache || !node) return;
    
    /* 将节点移到链表头部 */
    if (node != cache->head) {
        yq_cache_remove_node(cache, node);
        yq_cache_add_node(cache, node);
    }
}

static void yq_cache_update_lfu(yq_cache *cache, yq_cache_node *node) {
    if (!cache || !node) return;
    
    /* LFU不需要调整链表顺序，只需要增加访问计数 */
    node->access_count++;
}

static void yq_cache_cleanup_expired(yq_cache *cache) {
    if (!cache) return;
    
    uint64_t current_time = time(NULL);
    if (current_time - cache->last_cleanup < cache->cleanup_interval) {
        return;
    }
    
    yq_cache_node *current = cache->head;
    while (current) {
        yq_cache_node *next = current->next;
        
        if (yq_cache_is_expired(current)) {
            yq_cache_remove_node(cache, current);
            yq_cache_node_destroy(current);
            cache->stats.expired++;
        }
        
        current = next;
    }
    
    cache->last_cleanup = current_time;
}

static bool yq_cache_is_expired(yq_cache_node *node) {
    if (!node) return false;
    
    if (node->expiration == 0) return false;  /* 永不过期 */
    
    uint64_t current_time = time(NULL);
    return node->expiration <= current_time;
}

static uint32_t yq_cache_get_hash_index(yq_cache *cache, const char *key) {
    uint32_t hash = yq_cache_hash(key);
    return hash % cache->hash_size;
}

static yq_cache_node *yq_cache_find_node(yq_cache *cache, const char *key) {
    if (!cache || !key) return NULL;
    
    uint32_t hash_index = yq_cache_get_hash_index(cache, key);
    yq_cache_node *node = cache->hash_table[hash_index];
    
    while (node) {
        if (strcmp(node->key, key) == 0) {
            return node;
        }
        node = node->hash_next;
    }
    
    return NULL;
}