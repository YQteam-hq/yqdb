/*
 *  yq_cache.c - Caching layer implementation for yq-DB
 *  
 *  This file implements the caching functionality for yq-DB,
 *  providing in-memory caching to improve read performance and reduce
 *  disk I/O for frequently accessed data.
 */

#include "yq_cache.h"
#include "yq.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <errno.h>
#include <stdarg.h>

/* Internal cache state */
static struct yq_cache *g_caches[YQ_CACHE_DEFAULT_SHARDS] = {NULL};

/* Logging function for cache operations */
static void yq_cache_log(struct yq_cache *cache, const char *format, ...) {
    if (!cache) return;
    
    va_list args;
    va_start(args, format);
    
    char timestamp[32];
    time_t now = time(NULL);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", localtime(&now));
    
    printf("[%s] [CACHE:%s] ", timestamp, cache->name);
    vprintf(format, args);
    printf("\n");
    
    va_end(args);
}

/* Hash function for cache keys */
uint32_t yq_cache_hash(const char *key, size_t key_len) {
    if (!key) return 0;
    
    /* Simple hash function */
    uint32_t hash = 5381;
    const char *p = key;
    
    for (size_t i = 0; i < key_len; i++) {
        hash = ((hash << 5) + hash) + *p++;
    }
    
    return hash;
}

/* Create new cache item */
static yq_cache_item *yq_cache_item_create(const char *key, size_t key_len, const void *value, size_t value_len) {
    yq_cache_item *item = malloc(sizeof(yq_cache_item));
    if (!item) return NULL;
    
    memset(item, 0, sizeof(yq_cache_item));
    
    item->struct_size = sizeof(yq_cache_item);
    item->key_len = key_len;
    item->value_len = value_len;
    item->value_size = value_len;
    item->hash = yq_cache_hash(key, key_len);
    item->created_at = time(NULL);
    item->last_accessed = item->created_at;
    item->expires_at = 0; /* No expiration by default */
    
    /* Copy key */
    if (key_len > 0) {
        memcpy(item->key, key, key_len);
    }
    
    /* Copy value */
    if (value_len > 0) {
        item->value = malloc(value_len);
        if (!item->value) {
            free(item);
            return NULL;
        }
        memcpy(item->value, value, value_len);
    }
    
    return item;
}

/* Free cache item */
static void yq_cache_item_free(yq_cache_item *item) {
    if (!item) return;
    
    if (item->value) {
        free(item->value);
        item->value = NULL;
    }
    
    free(item);
}

/* Get shard for key */
static uint32_t yq_cache_get_shard(yq_cache *cache, const char *key, size_t key_len) {
    if (!cache || !key) return 0;
    
    uint32_t hash = yq_cache_hash(key, key_len);
    return hash % cache->shard_count;
}

/* Initialize cache shard */
static int yq_cache_shard_init(yq_cache_shard *shard, yq_cache *cache, uint32_t shard_id) {
    /* shard_id parameter is intentionally unused but kept for API consistency */
    if (!shard || !cache) return YQ_CACHE_ERR_INVAL;
    
    shard->config = cache->config;
    shard->cache = cache;
    shard->capacity = cache->config.max_size / cache->shard_count;
    shard->size = 0;
    
    /* Allocate hash table */
    shard->items = calloc(shard->capacity, sizeof(yq_cache_item *));
    if (!shard->items) return YQ_CACHE_ERR_NOMEM;
    
    /* Initialize mutex */
    pthread_mutex_init(&shard->mutex, NULL);
    
    /* Initialize LRU list */
    shard->lru_head = NULL;
    shard->lru_tail = NULL;
    
    return YQ_CACHE_OK;
}

/* Shutdown cache shard */
static void yq_cache_shard_shutdown(yq_cache_shard *shard) {
    if (!shard) return;
    
    pthread_mutex_lock(&shard->mutex);
    
    /* Free all items */
    for (uint32_t i = 0; i < shard->capacity; i++) {
        yq_cache_item *item = shard->items[i];
        while (item) {
            yq_cache_item *next = item->next;
            yq_cache_item_free(item);
            item = next;
        }
        shard->items[i] = NULL;
    }
    
    free(shard->items);
    shard->items = NULL;
    shard->size = 0;
    shard->lru_head = NULL;
    shard->lru_tail = NULL;
    
    pthread_mutex_unlock(&shard->mutex);
    pthread_mutex_destroy(&shard->mutex);
}

/* Find item in shard */
static yq_cache_item *yq_cache_shard_find(yq_cache_shard *shard, const char *key, size_t key_len) {
    if (!shard || !key) return NULL;
    
    uint32_t hash = yq_cache_hash(key, key_len);
    uint32_t index = hash % shard->capacity;
    
    yq_cache_item *item = shard->items[index];
    while (item) {
        if (item->hash == hash && item->key_len == key_len && 
            memcmp(item->key, key, key_len) == 0) {
            return item;
        }
        item = item->next;
    }
    
    return NULL;
}

/* Add item to shard */
static int yq_cache_shard_add(yq_cache_shard *shard, yq_cache_item *item) {
    if (!shard || !item) return YQ_CACHE_ERR_INVAL;
    
    uint32_t hash = item->hash;
    uint32_t index = hash % shard->capacity;
    
    /* Check if item already exists */
    yq_cache_item *existing = shard->items[index];
    while (existing) {
        if (existing->hash == hash && existing->key_len == item->key_len && 
            memcmp(existing->key, item->key, item->key_len) == 0) {
            /* Item exists, replace it */
            yq_cache_item_free(existing);
            break;
        }
        existing = existing->next;
    }
    
    /* Add item to hash table */
    item->next = shard->items[index];
    shard->items[index] = item;
    shard->size++;
    
    /* Add to LRU list (head) */
    item->prev = NULL;
    item->next = shard->lru_head;
    if (shard->lru_head) {
        shard->lru_head->prev = item;
    }
    shard->lru_head = item;
    if (!shard->lru_tail) {
        shard->lru_tail = item;
    }
    
    return YQ_CACHE_OK;
}

/* Remove item from shard */
static int yq_cache_shard_remove(yq_cache_shard *shard, const char *key, size_t key_len) {
    if (!shard || !key) return YQ_CACHE_ERR_INVAL;
    
    uint32_t hash = yq_cache_hash(key, key_len);
    uint32_t index = hash % shard->capacity;
    
    yq_cache_item **current = &shard->items[index];
    yq_cache_item *item = *current;
    
    while (item) {
        if (item->hash == hash && item->key_len == key_len && 
            memcmp(item->key, key, key_len) == 0) {
            /* Remove from hash table */
            *current = item->next;
            
            /* Remove from LRU list */
            if (item->prev) {
                item->prev->next = item->next;
            } else {
                shard->lru_head = item->next;
            }
            
            if (item->next) {
                item->next->prev = item->prev;
            } else {
                shard->lru_tail = item->prev;
            }
            
            shard->size--;
            yq_cache_item_free(item);
            return YQ_CACHE_OK;
        }
        
        current = &item->next;
        item = item->next;
    }
    
    return YQ_CACHE_ERR_NOT_FOUND;
}

/* Evict item from shard */
static yq_cache_item *yq_cache_shard_evict(yq_cache_shard *shard) {
    if (!shard || shard->size == 0) return NULL;
    
    /* Evict LRU item (tail of LRU list) */
    yq_cache_item *item = shard->lru_tail;
    if (!item) return NULL;
    
    /* Remove from shard */
    yq_cache_shard_remove(shard, item->key, item->key_len);
    
    /* Reset item for reuse */
    item->prev = NULL;
    item->next = NULL;
    
    return item;
}

/* Check if item is expired */
static int yq_cache_item_is_expired(yq_cache_item *item) {
    if (!item) return 0;
    
    if (item->expires_at == 0) return 0; /* No expiration */
    
    return time(NULL) > item->expires_at;
}

/* Update item access time */
static void yq_cache_item_update_access(yq_cache_item *item) {
    if (!item) return;
    
    item->last_accessed = time(NULL);
    item->access_count++;
}

/* Move item to LRU head */
static void yq_cache_item_to_head(yq_cache_shard *shard, yq_cache_item *item) {
    if (!shard || !item) return;
    
    /* Remove from current position */
    if (item->prev) {
        item->prev->next = item->next;
    } else {
        shard->lru_head = item->next;
    }
    
    if (item->next) {
        item->next->prev = item->prev;
    } else {
        shard->lru_tail = item->prev;
    }
    
    /* Add to head */
    item->prev = NULL;
    item->next = shard->lru_head;
    if (shard->lru_head) {
        shard->lru_head->prev = item;
    }
    shard->lru_head = item;
    if (!shard->lru_tail) {
        shard->lru_tail = item;
    }
}

/* Initialize cache */
int yq_cache_init(yq_cache_config *config, const char *name, yq_cache **out) {
    if (!config || !out) return YQ_CACHE_ERR_INVAL;
    
    /* Validate configuration */
    int result = yq_cache_validate_config(config);
    if (result != YQ_CACHE_OK) {
        return result;
    }
    
    /* Allocate cache structure */
    *out = malloc(sizeof(yq_cache));
    if (!*out) return YQ_CACHE_ERR_NOMEM;
    
    memset(*out, 0, sizeof(yq_cache));
    
    /* Copy configuration */
    memcpy(&(*out)->config, config, sizeof(yq_cache_config));
    
    /* Set cache name */
    if (name) {
        strncpy((*out)->name, name, YQ_CACHE_MAX_NAME_LEN - 1);
        (*out)->name[YQ_CACHE_MAX_NAME_LEN - 1] = '\0';
    } else {
        snprintf((*out)->name, YQ_CACHE_MAX_NAME_LEN, "cache_%d", getpid());
    }
    
    /* Initialize shards */
    (*out)->shard_count = config->shard_count;
    for (uint32_t i = 0; i < (*out)->shard_count; i++) {
        (*out)->shards[i] = malloc(sizeof(yq_cache_shard));
        if (!(*out)->shards[i]) {
            /* Cleanup already allocated shards */
            for (uint32_t j = 0; j < i; j++) {
                free((*out)->shards[j]);
            }
            free(*out);
            return YQ_CACHE_ERR_NOMEM;
        }
        
        result = yq_cache_shard_init((*out)->shards[i], *out, i);
        if (result != YQ_CACHE_OK) {
            /* Cleanup already allocated shards */
            for (uint32_t j = 0; j <= i; j++) {
                free((*out)->shards[j]);
            }
            free(*out);
            return result;
        }
        
        g_caches[i] = *out;
    }
    
    /* Initialize mutex */
    pthread_mutex_init(&(*out)->mutex, NULL);
    
    (*out)->running = 1;
    (*out)->last_cleanup = time(NULL);
    
    yq_cache_log(*out, "Cache initialized with %u shards", (*out)->shard_count);
    
    return YQ_CACHE_OK;
}

/* Shutdown cache */
int yq_cache_shutdown(yq_cache *cache) {
    if (!cache) return YQ_CACHE_ERR_INVAL;
    
    pthread_mutex_lock(&cache->mutex);
    
    cache->running = 0;
    
    /* Shutdown all shards */
    for (uint32_t i = 0; i < cache->shard_count; i++) {
        if (cache->shards[i]) {
            yq_cache_shard_shutdown(cache->shards[i]);
            free(cache->shards[i]);
            cache->shards[i] = NULL;
        }
    }
    
    pthread_mutex_unlock(&cache->mutex);
    pthread_mutex_destroy(&cache->mutex);
    
    yq_cache_log(cache, "Cache shutdown");
    
    return YQ_CACHE_OK;
}

/* Clear cache */
int yq_cache_clear(yq_cache *cache) {
    if (!cache) return YQ_CACHE_ERR_INVAL;
    
    pthread_mutex_lock(&cache->mutex);
    
    /* Clear all shards */
    for (uint32_t i = 0; i < cache->shard_count; i++) {
        if (cache->shards[i]) {
            yq_cache_shard_shutdown(cache->shards[i]);
            yq_cache_shard_init(cache->shards[i], cache, i);
        }
    }
    
    pthread_mutex_unlock(&cache->mutex);
    
    yq_cache_log(cache, "Cache cleared");
    
    return YQ_CACHE_OK;
}

/* Get value from cache */
int yq_cache_get(yq_cache *cache, const char *key, size_t key_len, void **value, size_t *value_len) {
    if (!cache || !key || !value || !value_len) return YQ_CACHE_ERR_INVAL;
    
    uint32_t shard_id = yq_cache_get_shard(cache, key, key_len);
    yq_cache_shard *shard = cache->shards[shard_id];
    
    pthread_mutex_lock(&shard->mutex);
    
    yq_cache_item *item = yq_cache_shard_find(shard, key, key_len);
    if (!item) {
        pthread_mutex_unlock(&shard->mutex);
        cache->stats.misses++;
        cache->stats.last_miss = time(NULL);
        return YQ_CACHE_ERR_NOT_FOUND;
    }
    
    /* Check if expired */
    if (yq_cache_item_is_expired(item)) {
        yq_cache_shard_remove(shard, key, key_len);
        pthread_mutex_unlock(&shard->mutex);
        cache->stats.expirations++;
        cache->stats.last_miss = time(NULL);
        return YQ_CACHE_ERR_EXPIRED;
    }
    
    /* Update access statistics */
    yq_cache_item_update_access(item);
    yq_cache_item_to_head(shard, item);
    
    /* Copy value */
    *value = malloc(item->value_len);
    if (!*value) {
        pthread_mutex_unlock(&shard->mutex);
        return YQ_CACHE_ERR_NOMEM;
    }
    memcpy(*value, item->value, item->value_len);
    *value_len = item->value_len;
    
    pthread_mutex_unlock(&shard->mutex);
    
    cache->stats.hits++;
    cache->stats.last_hit = time(NULL);
    
    return YQ_CACHE_OK;
}

/* Put value into cache */
int yq_cache_put(yq_cache *cache, const char *key, size_t key_len, const void *value, size_t value_len, uint32_t ttl) {
    if (!cache || !key || !value) return YQ_CACHE_ERR_INVAL;
    
    if (value_len > YQ_CACHE_MAX_VALUE_LEN) {
        return YQ_CACHE_ERR_INVAL;
    }
    
    uint32_t shard_id = yq_cache_get_shard(cache, key, key_len);
    yq_cache_shard *shard = cache->shards[shard_id];
    
    pthread_mutex_lock(&shard->mutex);
    
    /* Create new item */
    yq_cache_item *item = yq_cache_item_create(key, key_len, value, value_len);
    if (!item) {
        pthread_mutex_unlock(&shard->mutex);
        return YQ_CACHE_ERR_NOMEM;
    }
    
    /* Set TTL */
    if (ttl > 0) {
        item->expires_at = time(NULL) + ttl;
    }
    
    /* Check if shard is full */
    if (shard->size >= shard->capacity) {
        /* Evict LRU item */
        yq_cache_item *evicted = yq_cache_shard_evict(shard);
        if (evicted) {
            cache->stats.evictions++;
            cache->stats.last_eviction = time(NULL);
        }
    }
    
    /* Add item to shard */
    int result = yq_cache_shard_add(shard, item);
    if (result != YQ_CACHE_OK) {
        yq_cache_item_free(item);
        pthread_mutex_unlock(&shard->mutex);
        return result;
    }
    
    pthread_mutex_unlock(&shard->mutex);
    
    cache->stats.total_items++;
    cache->stats.updates++;
    
    return YQ_CACHE_OK;
}

/* Update value in cache */
int yq_cache_update(yq_cache *cache, const char *key, size_t key_len, const void *value, size_t value_len) {
    if (!cache || !key || !value) return YQ_CACHE_ERR_INVAL;
    
    uint32_t shard_id = yq_cache_get_shard(cache, key, key_len);
    yq_cache_shard *shard = cache->shards[shard_id];
    
    pthread_mutex_lock(&shard->mutex);
    
    yq_cache_item *item = yq_cache_shard_find(shard, key, key_len);
    if (!item) {
        pthread_mutex_unlock(&shard->mutex);
        return YQ_CACHE_ERR_NOT_FOUND;
    }
    
    /* Update value */
    if (item->value) {
        free(item->value);
    }
    
    item->value = malloc(value_len);
    if (!item->value) {
        pthread_mutex_unlock(&shard->mutex);
        return YQ_CACHE_ERR_NOMEM;
    }
    
    memcpy(item->value, value, value_len);
    item->value_len = value_len;
    item->value_size = value_len;
    item->last_accessed = time(NULL);
    
    pthread_mutex_unlock(&shard->mutex);
    
    cache->stats.updates++;
    
    return YQ_CACHE_OK;
}

/* Delete value from cache */
int yq_cache_delete(yq_cache *cache, const char *key, size_t key_len) {
    if (!cache || !key) return YQ_CACHE_ERR_INVAL;
    
    uint32_t shard_id = yq_cache_get_shard(cache, key, key_len);
    yq_cache_shard *shard = cache->shards[shard_id];
    
    pthread_mutex_lock(&shard->mutex);
    
    int result = yq_cache_shard_remove(shard, key, key_len);
    pthread_mutex_unlock(&shard->mutex);
    
    if (result == YQ_CACHE_OK) {
        cache->stats.total_items--;
        cache->stats.deletions++;
    }
    
    return result;
}

/* Check if key exists in cache */
int yq_cache_exists(yq_cache *cache, const char *key, size_t key_len) {
    if (!cache || !key) return YQ_CACHE_ERR_INVAL;
    
    uint32_t shard_id = yq_cache_get_shard(cache, key, key_len);
    yq_cache_shard *shard = cache->shards[shard_id];
    
    pthread_mutex_lock(&shard->mutex);
    
    yq_cache_item *item = yq_cache_shard_find(shard, key, key_len);
    if (!item) {
        pthread_mutex_unlock(&shard->mutex);
        return YQ_CACHE_ERR_NOT_FOUND;
    }
    
    /* Check if expired */
    if (yq_cache_item_is_expired(item)) {
        yq_cache_shard_remove(shard, key, key_len);
        pthread_mutex_unlock(&shard->mutex);
        cache->stats.expirations++;
        return YQ_CACHE_ERR_EXPIRED;
    }
    
    pthread_mutex_unlock(&shard->mutex);
    
    return YQ_CACHE_OK;
}

/* Get cache statistics */
int yq_cache_get_stats(yq_cache *cache, yq_cache_stats *stats) {
    if (!cache || !stats) return YQ_CACHE_ERR_INVAL;
    
    pthread_mutex_lock(&cache->mutex);
    
    /* Aggregate statistics from all shards */
    memset(stats, 0, sizeof(yq_cache_stats));
    stats->struct_size = sizeof(yq_cache_stats);
    
    for (uint32_t i = 0; i < cache->shard_count; i++) {
        yq_cache_shard *shard = cache->shards[i];
        pthread_mutex_lock(&shard->mutex);
        
        stats->total_items += shard->size;
        pthread_mutex_unlock(&shard->mutex);
    }
    
    /* Copy global stats */
    stats->hits = cache->stats.hits;
    stats->misses = cache->stats.misses;
    stats->evictions = cache->stats.evictions;
    stats->expirations = cache->stats.expirations;
    stats->updates = cache->stats.updates;
    stats->deletions = cache->stats.deletions;
    stats->last_hit = cache->stats.last_hit;
    stats->last_miss = cache->stats.last_miss;
    stats->last_eviction = cache->stats.last_eviction;
    
    /* Calculate ratios */
    if (stats->hits + stats->misses > 0) {
        stats->hit_ratio = (double)stats->hits / (stats->hits + stats->misses);
    }
    if (stats->hits + stats->misses > 0) {
        stats->eviction_rate = (double)stats->evictions / (stats->hits + stats->misses);
    }
    
    pthread_mutex_unlock(&cache->mutex);
    
    return YQ_CACHE_OK;
}

/* Validate cache configuration */
int yq_cache_validate_config(yq_cache_config *config) {
    if (!config) return YQ_CACHE_ERR_INVAL;
    
    /* Check struct size */
    if (config->struct_size != sizeof(yq_cache_config)) {
        return YQ_CACHE_ERR_CONFIG;
    }
    
    /* Check basic configuration */
    if (config->max_size == 0 || config->max_size > 10000000) {
        return YQ_CACHE_ERR_CONFIG;
    }
    
    if (config->shard_count == 0 || config->shard_count > YQ_CACHE_DEFAULT_SHARDS) {
        return YQ_CACHE_ERR_CONFIG;
    }
    
    if (config->default_ttl > 86400 * 30) { /* Max 30 days */
        return YQ_CACHE_ERR_CONFIG;
    }
    
    return YQ_CACHE_OK;
}

/* Get cache hit ratio */
double yq_cache_get_hit_ratio(yq_cache *cache) {
    if (!cache) return 0.0;
    
    yq_cache_stats stats;
    int result = yq_cache_get_stats(cache, &stats);
    if (result != YQ_CACHE_OK) {
        return 0.0;
    }
    
    return stats.hit_ratio;
}

/* Cleanup expired items */
int yq_cache_cleanup(yq_cache *cache) {
    if (!cache) return YQ_CACHE_ERR_INVAL;
    
    time_t now = time(NULL);
    if (now - cache->last_cleanup < cache->config.cleanup_interval) {
        return YQ_CACHE_OK; /* Not time yet */
    }
    
    pthread_mutex_lock(&cache->mutex);
    
    int cleaned_count = 0;
    
    for (uint32_t i = 0; i < cache->shard_count; i++) {
        yq_cache_shard *shard = cache->shards[i];
        pthread_mutex_lock(&shard->mutex);
        
        yq_cache_item **current = &shard->items[0];
        for (uint32_t j = 0; j < shard->capacity; j++) {
            yq_cache_item *item = shard->items[j];
            while (item) {
                yq_cache_item *next = item->next;
                
                if (yq_cache_item_is_expired(item)) {
                    /* Remove expired item */
                    *current = next;
                    if (item->prev) {
                        item->prev->next = item->next;
                    } else {
                        shard->lru_head = item->next;
                    }
                    
                    if (item->next) {
                        item->next->prev = item->prev;
                    } else {
                        shard->lru_tail = item->prev;
                    }
                    
                    shard->size--;
                    yq_cache_item_free(item);
                    cleaned_count++;
                    
                    item = next;
                    continue;
                }
                
                current = &item->next;
                item = next;
            }
            current = &shard->items[j + 1];
        }
        
        pthread_mutex_unlock(&shard->mutex);
    }
    
    cache->last_cleanup = now;
    pthread_mutex_unlock(&cache->mutex);
    
    if (cleaned_count > 0) {
        yq_cache_log(cache, "Cleaned up %d expired items", cleaned_count);
    }
    
    return YQ_CACHE_OK;
}

/* Batch operations - get multiple values */
int yq_cache_get_multi(yq_cache *cache, const char **keys, size_t *key_lens, void **values, size_t *value_lens, uint32_t count) {
    if (!cache || !keys || !key_lens || !values || !value_lens || count == 0) {
        return YQ_CACHE_ERR_INVAL;
    }
    
    int result = YQ_CACHE_OK;
    int found_count = 0;
    
    for (uint32_t i = 0; i < count; i++) {
        values[i] = NULL;
        value_lens[i] = 0;
        
        int r = yq_cache_get(cache, keys[i], key_lens[i], &values[i], &value_lens[i]);
        if (r == YQ_CACHE_OK) {
            found_count++;
        } else if (r == YQ_CACHE_ERR_EXPIRED) {
            /* Remove expired items */
            yq_cache_delete(cache, keys[i], key_lens[i]);
        }
    }
    
    if ((uint32_t)found_count < count) {
        result = YQ_CACHE_ERR_NOT_FOUND;
    }
    
    return result;
}

/* Batch operations - put multiple values */
int yq_cache_put_multi(yq_cache *cache, const char **keys, size_t *key_lens, const void **values, size_t *value_lens, uint32_t *ttls, uint32_t count) {
    if (!cache || !keys || !key_lens || !values || !value_lens || !ttls || count == 0) {
        return YQ_CACHE_ERR_INVAL;
    }
    
    int result = YQ_CACHE_OK;
    
    for (uint32_t i = 0; i < count; i++) {
        int r = yq_cache_put(cache, keys[i], key_lens[i], values[i], value_lens[i], ttls[i]);
        if (r != YQ_CACHE_OK) {
            result = r;
        }
    }
    
    return result;
}

/* Batch operations - delete multiple keys */
int yq_cache_delete_multi(yq_cache *cache, const char **keys, size_t *key_lens, uint32_t count) {
    if (!cache || !keys || !key_lens || count == 0) {
        return YQ_CACHE_ERR_INVAL;
    }
    
    int result = YQ_CACHE_OK;
    
    for (uint32_t i = 0; i < count; i++) {
        int r = yq_cache_delete(cache, keys[i], key_lens[i]);
        if (r != YQ_CACHE_OK) {
            result = r;
        }
    }
    
    return result;
}

/* Resize cache */
int yq_cache_resize(yq_cache *cache, uint32_t new_size) {
    if (!cache) return YQ_CACHE_ERR_INVAL;
    
    pthread_mutex_lock(&cache->mutex);
    
    uint32_t old_size = cache->config.max_size;
    cache->config.max_size = new_size;
    
    /* Update shard capacities */
    for (uint32_t i = 0; i < cache->shard_count; i++) {
        yq_cache_shard *shard = cache->shards[i];
        shard->capacity = new_size / cache->shard_count;
    }
    
    pthread_mutex_unlock(&cache->mutex);
    
    yq_cache_log(cache, "Cache resized from %u to %u items", old_size, new_size);
    
    return YQ_CACHE_OK;
}

/* Set TTL for key */
int yq_cache_set_ttl(yq_cache *cache, const char *key, size_t key_len, uint32_t ttl) {
    if (!cache || !key) return YQ_CACHE_ERR_INVAL;
    
    uint32_t shard_id = yq_cache_get_shard(cache, key, key_len);
    yq_cache_shard *shard = cache->shards[shard_id];
    
    pthread_mutex_lock(&shard->mutex);
    
    yq_cache_item *item = yq_cache_shard_find(shard, key, key_len);
    if (!item) {
        pthread_mutex_unlock(&shard->mutex);
        return YQ_CACHE_ERR_NOT_FOUND;
    }
    
    /* Set TTL */
    if (ttl > 0) {
        item->expires_at = time(NULL) + ttl;
    } else {
        item->expires_at = 0; /* No expiration */
    }
    
    pthread_mutex_unlock(&shard->mutex);
    
    return YQ_CACHE_OK;
}

/* Get TTL for key */
int yq_cache_get_ttl(yq_cache *cache, const char *key, size_t key_len, uint32_t *ttl) {
    if (!cache || !key || !ttl) return YQ_CACHE_ERR_INVAL;
    
    uint32_t shard_id = yq_cache_get_shard(cache, key, key_len);
    yq_cache_shard *shard = cache->shards[shard_id];
    
    pthread_mutex_lock(&shard->mutex);
    
    yq_cache_item *item = yq_cache_shard_find(shard, key, key_len);
    if (!item) {
        pthread_mutex_unlock(&shard->mutex);
        return YQ_CACHE_ERR_NOT_FOUND;
    }
    
    /* Calculate remaining TTL */
    if (item->expires_at == 0) {
        *ttl = 0; /* No expiration */
    } else {
        time_t now = time(NULL);
        *ttl = (now < item->expires_at) ? (uint32_t)(item->expires_at - now) : 0;
    }
    
    pthread_mutex_unlock(&shard->mutex);
    
    return YQ_CACHE_OK;
}

/* Expire key immediately */
int yq_cache_expire(yq_cache *cache, const char *key, size_t key_len) {
    if (!cache || !key) return YQ_CACHE_ERR_INVAL;
    
    return yq_cache_set_ttl(cache, key, key_len, 0);
}

/* Touch key (update access time) */
int yq_cache_touch(yq_cache *cache, const char *key, size_t key_len) {
    if (!cache || !key) return YQ_CACHE_ERR_INVAL;
    
    uint32_t shard_id = yq_cache_get_shard(cache, key, key_len);
    yq_cache_shard *shard = cache->shards[shard_id];
    
    pthread_mutex_lock(&shard->mutex);
    
    yq_cache_item *item = yq_cache_shard_find(shard, key, key_len);
    if (!item) {
        pthread_mutex_unlock(&shard->mutex);
        return YQ_CACHE_ERR_NOT_FOUND;
    }
    
    yq_cache_item_update_access(item);
    yq_cache_item_to_head(shard, item);
    
    pthread_mutex_unlock(&shard->mutex);
    
    return YQ_CACHE_OK;
}

/* Get item information */
int yq_cache_get_item_info(yq_cache *cache, const char *key, size_t key_len, yq_cache_item *item) {
    if (!cache || !key || !item) return YQ_CACHE_ERR_INVAL;
    
    uint32_t shard_id = yq_cache_get_shard(cache, key, key_len);
    yq_cache_shard *shard = cache->shards[shard_id];
    
    pthread_mutex_lock(&shard->mutex);
    
    yq_cache_item *found = yq_cache_shard_find(shard, key, key_len);
    if (!found) {
        pthread_mutex_unlock(&shard->mutex);
        return YQ_CACHE_ERR_NOT_FOUND;
    }
    
    /* Copy item information */
    memset(item, 0, sizeof(yq_cache_item));
    item->struct_size = sizeof(yq_cache_item);
    item->key_len = found->key_len;
    item->value_len = found->value_len;
    item->flags = found->flags;
    item->access_count = found->access_count;
    item->hit_count = found->hit_count;
    item->created_at = found->created_at;
    item->last_accessed = found->last_accessed;
    item->expires_at = found->expires_at;
    item->hash = found->hash;
    
    if (item->key_len > 0) {
        memcpy(item->key, found->key, item->key_len);
    }
    
    pthread_mutex_unlock(&shard->mutex);
    
    return YQ_CACHE_OK;
}

/* Get all keys */
int yq_cache_get_keys(yq_cache *cache, char **keys, size_t *key_lens, uint32_t max_keys) {
    if (!cache || !keys || !key_lens || max_keys == 0) {
        return YQ_CACHE_ERR_INVAL;
    }
    
    uint32_t count = 0;
    
    pthread_mutex_lock(&cache->mutex);
    
    for (uint32_t i = 0; i < cache->shard_count && count < max_keys; i++) {
        yq_cache_shard *shard = cache->shards[i];
        pthread_mutex_lock(&shard->mutex);
        
        for (uint32_t j = 0; j < shard->capacity && count < max_keys; j++) {
            yq_cache_item *item = shard->items[j];
            while (item && count < max_keys) {
                keys[count] = malloc(item->key_len + 1);
                if (!keys[count]) {
                    pthread_mutex_unlock(&shard->mutex);
                    pthread_mutex_unlock(&cache->mutex);
                    return YQ_CACHE_ERR_NOMEM;
                }
                
                memcpy(keys[count], item->key, item->key_len);
                keys[count][item->key_len] = '\0';
                key_lens[count] = item->key_len;
                
                count++;
                item = item->next;
            }
        }
        
        pthread_mutex_unlock(&shard->mutex);
    }
    
    pthread_mutex_unlock(&cache->mutex);
    
    return count;
}

/* Update cache configuration */
int yq_cache_update_config(yq_cache *cache, yq_cache_config *config) {
    if (!cache || !config) return YQ_CACHE_ERR_INVAL;
    
    int result = yq_cache_validate_config(config);
    if (result != YQ_CACHE_OK) {
        return result;
    }
    
    pthread_mutex_lock(&cache->mutex);
    
    /* Update configuration */
    memcpy(&cache->config, config, sizeof(yq_cache_config));
    
    /* Update shard counts if changed */
    if (config->shard_count != cache->shard_count) {
        /* Note: This would require complex rehashing logic */
        /* For now, just update the count */
        cache->shard_count = config->shard_count;
    }
    
    pthread_mutex_unlock(&cache->mutex);
    
    yq_cache_log(cache, "Configuration updated");
    
    return YQ_CACHE_OK;
}

/* Save cache configuration */
int yq_cache_save_config(yq_cache *cache, const char *filename) {
    if (!cache || !filename) return YQ_CACHE_ERR_INVAL;
    
    FILE *file = fopen(filename, "w");
    if (!file) return YQ_CACHE_ERR_INVAL;
    
    pthread_mutex_lock(&cache->mutex);
    
    /* Write configuration */
    fprintf(file, "# yq-DB Cache Configuration\n");
    fprintf(file, "# Cache: %s\n", cache->name);
    fprintf(file, "enabled=%u\n", cache->config.enabled);
    fprintf(file, "algorithm=%u\n", cache->config.algorithm);
    fprintf(file, "eviction_policy=%u\n", cache->config.eviction_policy);
    fprintf(file, "max_size=%u\n", cache->config.max_size);
    fprintf(file, "max_memory=%zu\n", cache->config.max_memory);
    fprintf(file, "default_ttl=%u\n", cache->config.default_ttl);
    fprintf(file, "cleanup_interval=%u\n", cache->config.cleanup_interval);
    fprintf(file, "shard_count=%u\n", cache->config.shard_count);
    fprintf(file, "compression=%u\n", cache->config.compression);
    fprintf(file, "encryption=%u\n", cache->config.encryption);
    fprintf(file, "persistence=%u\n", cache->config.persistence);
    fprintf(file, "monitoring=%u\n", cache->config.monitoring);
    fprintf(file, "warming=%u\n", cache->config.warming);
    
    pthread_mutex_unlock(&cache->mutex);
    
    fclose(file);
    
    return YQ_CACHE_OK;
}

/* Load cache configuration */
int yq_cache_load_config(yq_cache *cache, const char *filename) {
    if (!cache || !filename) return YQ_CACHE_ERR_INVAL;
    
    FILE *file = fopen(filename, "r");
    if (!file) return YQ_CACHE_ERR_INVAL;
    
    yq_cache_config config = cache->config;
    
    char line[256];
    while (fgets(line, sizeof(line), file)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        
        char key[64];
        char value[64];
        if (sscanf(line, "%63[^=]=%63[^\n]", key, value) == 2) {
            if (strcmp(key, "enabled") == 0) config.enabled = atoi(value);
            else if (strcmp(key, "algorithm") == 0) config.algorithm = atoi(value);
            else if (strcmp(key, "eviction_policy") == 0) config.eviction_policy = atoi(value);
            else if (strcmp(key, "max_size") == 0) config.max_size = atoi(value);
            else if (strcmp(key, "max_memory") == 0) config.max_memory = atoi(value);
            else if (strcmp(key, "default_ttl") == 0) config.default_ttl = atoi(value);
            else if (strcmp(key, "cleanup_interval") == 0) config.cleanup_interval = atoi(value);
            else if (strcmp(key, "shard_count") == 0) config.shard_count = atoi(value);
            else if (strcmp(key, "compression") == 0) config.compression = atoi(value);
            else if (strcmp(key, "encryption") == 0) config.encryption = atoi(value);
            else if (strcmp(key, "persistence") == 0) config.persistence = atoi(value);
            else if (strcmp(key, "monitoring") == 0) config.monitoring = atoi(value);
            else if (strcmp(key, "warming") == 0) config.warming = atoi(value);
        }
    }
    
    fclose(file);
    
    /* Validate and update configuration */
    int result = yq_cache_validate_config(&config);
    if (result != YQ_CACHE_OK) {
        return result;
    }
    
    pthread_mutex_lock(&cache->mutex);
    memcpy(&cache->config, &config, sizeof(yq_cache_config));
    pthread_mutex_unlock(&cache->mutex);
    
    return YQ_CACHE_OK;
}

/* Save cache to file */
int yq_cache_save(yq_cache *cache, const char *filename) {
    if (!cache || !filename) return YQ_CACHE_ERR_INVAL;
    
    FILE *file = fopen(filename, "wb");
    if (!file) return YQ_CACHE_ERR_INVAL;
    
    pthread_mutex_lock(&cache->mutex);
    
    /* Write header */
    fprintf(file, "YQ_CACHE\n");
    fprintf(file, "version=1\n");
    fprintf(file, "name=%s\n", cache->name);
    fprintf(file, "shards=%u\n", cache->shard_count);
    fprintf(file, "timestamp=%ld\n", time(NULL));
    
    /* Write shard data */
    for (uint32_t i = 0; i < cache->shard_count; i++) {
        yq_cache_shard *shard = cache->shards[i];
        pthread_mutex_lock(&shard->mutex);
        
        fprintf(file, "\n[shard_%u]\n", i);
        fprintf(file, "size=%u\n", shard->size);
        
        for (uint32_t j = 0; j < shard->capacity; j++) {
            yq_cache_item *item = shard->items[j];
            while (item) {
                fprintf(file, "key=%.*s\n", (int)item->key_len, item->key);
                fprintf(file, "value_len=%u\n", item->value_len);
                fprintf(file, "created_at=%ld\n", item->created_at);
                fprintf(file, "expires_at=%ld\n", item->expires_at);
                fprintf(file, "access_count=%u\n", item->access_count);
                
                /* Write value data */
                fwrite(item->value, 1, item->value_len, file);
                fprintf(file, "\n");
                
                item = item->next;
            }
        }
        
        pthread_mutex_unlock(&shard->mutex);
    }
    
    pthread_mutex_unlock(&cache->mutex);
    
    fclose(file);
    
    return YQ_CACHE_OK;
}

/* Load cache from file */
int yq_cache_load(yq_cache *cache, const char *filename) {
    if (!cache || !filename) return YQ_CACHE_ERR_INVAL;
    
    FILE *file = fopen(filename, "rb");
    if (!file) return YQ_CACHE_ERR_INVAL;
    
    /* Clear existing cache */
    yq_cache_clear(cache);
    
    char line[1024];
    char current_shard[64] = "";
    
    pthread_mutex_lock(&cache->mutex);
    
    while (fgets(line, sizeof(line), file)) {
        if (strncmp(line, "YQ_CACHE", 8) == 0) {
            /* Skip header */
            continue;
        }
        
        if (line[0] == '[') {
            /* New section */
            sscanf(line, "[%63[^]]]", current_shard);
            continue;
        }
        
        if (strncmp(line, "key=", 4) == 0) {
            /* Parse key */
            char key[YQ_CACHE_MAX_KEY_LEN];
            size_t key_len = strlen(line + 4) - 1; /* Remove newline */
            if (key_len > 0) {
                key[key_len] = '\0';
                strncpy(key, line + 4, key_len);
                
                /* Read value length */
                uint32_t value_len = 0;
                fgets(line, sizeof(line), file);
                if (strncmp(line, "value_len=", 10) == 0) {
                    value_len = atoi(line + 10);
                }
                
                /* Read created_at */
                time_t created_at = 0;
                fgets(line, sizeof(line), file);
                if (strncmp(line, "created_at=", 11) == 0) {
                    created_at = atol(line + 11);
                }
                
                /* Read expires_at */
                time_t expires_at = 0;
                fgets(line, sizeof(line), file);
                if (strncmp(line, "expires_at=", 11) == 0) {
                    expires_at = atol(line + 11);
                }
                
                /* Read access_count */
                uint32_t access_count = 0;
                fgets(line, sizeof(line), file);
                if (strncmp(line, "access_count=", 13) == 0) {
                    access_count = atoi(line + 13);
                }
                
                /* Read value data */
                if (value_len > 0) {
                    void *value = malloc(value_len);
                    if (value) {
                        fread(value, 1, value_len, file);
                        
                        /* Skip newline */
                        fgetc(file);
                        
                        /* Create item */
                        yq_cache_item *item = yq_cache_item_create(key, key_len, value, value_len);
                        if (item) {
                            item->created_at = created_at;
                            item->expires_at = expires_at;
                            item->access_count = access_count;
                            
                            /* Add to appropriate shard */
                            uint32_t shard_id = yq_cache_get_shard(cache, key, key_len);
                            yq_cache_shard *shard = cache->shards[shard_id];
                            
                            pthread_mutex_lock(&shard->mutex);
                            yq_cache_shard_add(shard, item);
                            pthread_mutex_unlock(&shard->mutex);
                            
                            free(value);
                        }
                    }
                }
            }
        }
    }
    
    pthread_mutex_unlock(&cache->mutex);
    
    fclose(file);
    
    return YQ_CACHE_OK;
}

/* Flush cache to persistent storage */
int yq_cache_flush(yq_cache *cache) {
    if (!cache) return YQ_CACHE_ERR_INVAL;
    
    /* This would typically flush to disk */
    /* For now, just clear the cache */
    return yq_cache_clear(cache);
}

/* Warm cache with specific keys */
int yq_cache_warm(yq_cache *cache, const char **keys, size_t *key_lens, uint32_t count) {
    if (!cache || !keys || !key_lens || count == 0) {
        return YQ_CACHE_ERR_INVAL;
    }
    
    yq_cache_log(cache, "Warming cache with %u keys", count);
    
    /* Note: This would typically load values from database */
    /* For now, just create empty cache entries */
    for (uint32_t i = 0; i < count; i++) {
        yq_cache_put(cache, keys[i], key_lens[i], "", 0, cache->config.default_ttl);
    }
    
    return YQ_CACHE_OK;
}

/* Warm cache from database */
int yq_cache_warm_from_db(yq_cache *cache, yq_db *db, const char *prefix) {
    if (!cache || !db) return YQ_CACHE_ERR_INVAL;
    
    yq_cache_log(cache, "Warming cache from database with prefix: %s", prefix ? prefix : "none");
    
    /* Note: This would scan database and load matching keys */
    /* For now, just log the operation */
    if (prefix) {
        yq_cache_log(cache, "Would load keys with prefix: %s", prefix);
    } else {
        yq_cache_log(cache, "Would load all keys from database");
    }
    
    return YQ_CACHE_OK;
}

/* Calculate cache size */
uint32_t yq_cache_calculate_size(yq_cache *cache) {
    if (!cache) return 0;
    
    yq_cache_stats stats;
    int result = yq_cache_get_stats(cache, &stats);
    if (result != YQ_CACHE_OK) {
        return 0;
    }
    
    return stats.total_items;
}