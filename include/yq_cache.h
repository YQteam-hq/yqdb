/*
 *  yq_cache.h - Caching layer API for yq-DB
 *  
 *  This header defines the caching functionality for yq-DB,
 *  providing in-memory caching to improve read performance and reduce
 *  disk I/O for frequently accessed data.
 *  
 *  Features:
 *  - Multiple cache algorithms (LRU, LFU, FIFO, ARC)
 *  - Configurable cache size and TTL
 *  - Thread-safe operations
 *  - Cache statistics and monitoring
 *  - Eviction policies
 *  - Cache warming strategies
 *  - Multi-level caching support
 *  
 *  Enable with: -DYQ_ENABLE_CACHE=1
 */

#ifndef YQ_CACHE_H
#define YQ_CACHE_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Cache configuration macros */
#define YQ_CACHE_MAX_NAME_LEN      64
#define YQ_CACHE_MAX_KEY_LEN      256
#define YQ_CACHE_MAX_VALUE_LEN    1024 * 1024  /* 1MB max value size */
#define YQ_CACHE_DEFAULT_SIZE      1000000     /* 1M entries */
#define YQ_CACHE_DEFAULT_TTL       3600        /* 1 hour default TTL */
#define YQ_CACHE_DEFAULT_SHARDS    16          /* Number of cache shards */

/* Cache algorithms */
typedef enum yq_cache_algorithm {
    YQ_CACHE_ALGORITHM_LRU = 0,    /* Least Recently Used */
    YQ_CACHE_ALGORITHM_LFU = 1,    /* Least Frequently Used */
    YQ_CACHE_ALGORITHM_FIFO = 2,   /* First In First Out */
    YQ_CACHE_ALGORITHM_ARC = 3,    /* Adaptive Replacement Cache */
    YQ_CACHE_ALGORITHM_RANDOM = 4, /* Random eviction */
    YQ_CACHE_ALGORITHM_CLOCK = 5   /* Clock algorithm */
} yq_cache_algorithm;

/* Cache eviction policies */
typedef enum yq_cache_eviction_policy {
    YQ_CACHE_EVICTION_STRICT = 0,   /* Strict size limits */
    YQ_CACHE_EVICTION_SOFT = 1,     /* Soft eviction with warnings */
    YQ_CACHE_EVICTION_LRU = 2,     /* LRU-based eviction */
    YQ_CACHE_EVICTION_LFU = 2,     /* LFU-based eviction */
    YQ_CACHE_EVICTION_ALL = 3      /* Evict all when full */
} yq_cache_eviction_policy;

/* Cache item states */
typedef enum yq_cache_item_state {
    YQ_CACHE_ITEM_STATE_ACTIVE = 0,
    YQ_CACHE_ITEM_STATE_EXPIRED = 1,
    YQ_CACHE_ITEM_STATE_EVICTED = 2,
    YQ_CACHE_ITEM_STATE_INVALID = 3
} yq_cache_item_state;

/* Cache statistics */
typedef struct yq_cache_stats {
    uint32_t struct_size;          /* Must be sizeof(yq_cache_stats) */
    uint64_t total_items;          /* Total items in cache */
    uint64_t hits;                  /* Cache hit count */
    uint64_t misses;                /* Cache miss count */
    uint64_t evictions;            /* Eviction count */
    uint64_t expirations;          /* Expiration count */
    uint64_t updates;              /* Update count */
    uint64_t deletions;            /* Deletion count */
    size_t memory_usage;           /* Current memory usage */
    size_t peak_memory;            /* Peak memory usage */
    double hit_ratio;              /* Cache hit ratio */
    double eviction_rate;          /* Eviction rate */
    time_t last_hit;               /* Last hit timestamp */
    time_t last_miss;              /* Last miss timestamp */
    time_t last_eviction;          /* Last eviction timestamp */
    uint32_t reserved[8];          /* Must be 0 */
} yq_cache_stats;

/* Cache configuration */
typedef struct yq_cache_config {
    uint32_t struct_size;          /* Must be sizeof(yq_cache_config) */
    uint32_t enabled;              /* Whether caching enabled */
    uint32_t algorithm;            /* Cache algorithm */
    uint32_t eviction_policy;      /* Eviction policy */
    uint32_t max_size;             /* Maximum number of items */
    size_t max_memory;            /* Maximum memory usage */
    uint32_t default_ttl;          /* Default TTL in seconds */
    uint32_t cleanup_interval;     /* Cleanup interval in seconds */
    uint32_t shard_count;          /* Number of cache shards */
    uint32_t compression;          /* Enable value compression */
    uint32_t encryption;           /* Enable value encryption */
    uint32_t persistence;          /* Enable cache persistence */
    uint32_t monitoring;           /* Enable monitoring */
    uint32_t warming;              /* Enable cache warming */
    uint32_t reserved[8];          /* Must be 0 */
} yq_cache_config;

/* Cache item metadata */
typedef struct yq_cache_item {
    uint32_t struct_size;          /* Must be sizeof(yq_cache_item) */
    uint32_t key_len;              /* Key length */
    uint32_t value_len;            /* Value length */
    uint32_t flags;                /* Item flags */
    uint32_t access_count;         /* Access count */
    uint32_t hit_count;            /* Hit count */
    time_t created_at;             /* Creation timestamp */
    time_t last_accessed;          /* Last access timestamp */
    time_t expires_at;             /* Expiration timestamp */
    uint64_t hash;                 /* Key hash */
    char key[YQ_CACHE_MAX_KEY_LEN]; /* Key data */
    void *value;                   /* Value data */
    size_t value_size;             /* Value size */
    struct yq_cache_item *prev;    /* Previous item in LRU list */
    struct yq_cache_item *next;    /* Next item in LRU list */
    struct yq_cache_shard *shard;  /* Reference to parent shard */
} yq_cache_item;

/* Cache shard */
typedef struct yq_cache_shard {
    yq_cache_config config;
    yq_cache_stats stats;
    yq_cache_item **items;         /* Hash table items */
    uint32_t capacity;             /* Hash table capacity */
    uint32_t size;                 /* Current size */
    pthread_mutex_t mutex;         /* Shard mutex */
    struct yq_cache_item *lru_head; /* LRU list head */
    struct yq_cache_item *lru_tail; /* LRU list tail */
    struct yq_cache *cache;       /* Reference to parent cache */
} yq_cache_shard;

/* Cache structure */
typedef struct yq_cache {
    yq_cache_config config;
    yq_cache_stats stats;
    yq_cache_shard *shards[YQ_CACHE_DEFAULT_SHARDS];
    uint32_t shard_count;
    pthread_mutex_t mutex;         /* Global mutex */
    int running;                  /* Whether cache is running */
    char name[YQ_CACHE_MAX_NAME_LEN]; /* Cache name */
    time_t last_cleanup;           /* Last cleanup timestamp */
} yq_cache;

/* Cache error codes */
#define YQ_CACHE_OK                 0
#define YQ_CACHE_ERR_INVAL         1
#define YQ_CACHE_ERR_NOMEM         2
#define YQ_CACHE_ERR_EXISTS        3
#define YQ_CACHE_ERR_NOT_FOUND      4
#define YQ_CACHE_ERR_FULL           5
#define YQ_CACHE_ERR_EXPIRED        6
#define YQ_CACHE_ERR_BUSY           7
#define YQ_CACHE_ERR_SHUTDOWN       8
#define YQ_CACHE_ERR_CONFIG         9
#define YQ_CACHE_ERR_STATE         10

/* Cache API functions */
int yq_cache_init(yq_cache_config *config, const char *name, yq_cache **out);
int yq_cache_shutdown(yq_cache *cache);
int yq_cache_clear(yq_cache *cache);

/* Cache operations */
int yq_cache_get(yq_cache *cache, const char *key, size_t key_len, void **value, size_t *value_len);
int yq_cache_put(yq_cache *cache, const char *key, size_t key_len, const void *value, size_t value_len, uint32_t ttl);
int yq_cache_update(yq_cache *cache, const char *key, size_t key_len, const void *value, size_t value_len);
int yq_cache_delete(yq_cache *cache, const char *key, size_t key_len);
int yq_cache_exists(yq_cache *cache, const char *key, size_t key_len);

/* Batch operations */
int yq_cache_get_multi(yq_cache *cache, const char **keys, size_t *key_lens, void **values, size_t *value_lens, uint32_t count);
int yq_cache_put_multi(yq_cache *cache, const char **keys, size_t *key_lens, const void **values, size_t *value_lens, uint32_t *ttls, uint32_t count);
int yq_cache_delete_multi(yq_cache *cache, const char **keys, size_t *key_lens, uint32_t count);

/* Cache management */
int yq_cache_resize(yq_cache *cache, uint32_t new_size);
int yq_cache_set_ttl(yq_cache *cache, const char *key, size_t key_len, uint32_t ttl);
int yq_cache_get_ttl(yq_cache *cache, const char *key, size_t key_len, uint32_t *ttl);
int yq_cache_expire(yq_cache *cache, const char *key, size_t key_len);
int yq_cache_touch(yq_cache *cache, const char *key, size_t key_len);

/* Statistics and monitoring */
int yq_cache_get_stats(yq_cache *cache, yq_cache_stats *stats);
int yq_cache_get_item_info(yq_cache *cache, const char *key, size_t key_len, yq_cache_item *item);
int yq_cache_get_keys(yq_cache *cache, char **keys, size_t *key_lens, uint32_t max_keys);
int yq_cache_cleanup(yq_cache *cache);

/* Cache configuration */
int yq_cache_update_config(yq_cache *cache, yq_cache_config *config);
int yq_cache_save_config(yq_cache *cache, const char *filename);
int yq_cache_load_config(yq_cache *cache, const char *filename);

/* Cache persistence */
int yq_cache_save(yq_cache *cache, const char *filename);
int yq_cache_load(yq_cache *cache, const char *filename);
int yq_cache_flush(yq_cache *cache);

/* Cache warming */
int yq_cache_warm(yq_cache *cache, const char **keys, size_t *key_lens, uint32_t count);
/* Forward declaration for yq_db */
struct yq_db;
int yq_cache_warm_from_db(yq_cache *cache, struct yq_db *db, const char *prefix);

/* Utility functions */
uint32_t yq_cache_hash(const char *key, size_t key_len);
uint32_t yq_cache_calculate_size(yq_cache *cache);
int yq_cache_validate_config(yq_cache_config *config);
double yq_cache_get_hit_ratio(yq_cache *cache);

#ifdef __cplusplus
}
#endif

#endif /* YQ_CACHE_H */