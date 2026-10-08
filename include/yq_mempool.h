/*
 * yq_mempool.h — Memory pool allocator for small objects
 *
 * This header provides a high-performance memory pool allocator optimized for
 * small, frequently allocated objects. It reduces malloc/free overhead by
 * pre-allocating memory in chunks and managing it internally.
 */

#ifndef YQ_MEMPOOL_H
#define YQ_MEMPOOL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Memory pool configuration */
#define YQ_MEMPOOL_SMALL_OBJ_SIZE  256    /* Maximum size for small objects */
#define YQ_MEMPOOL_CHUNK_SIZE     4096    /* Size of each memory chunk */
#define YQ_MEMPOOL_CHUNK_OBJS     16      /* Objects per chunk (4096/256) */

/*
 * Memory pool handle
 */
typedef struct yq_mempool yq_mempool;

/*
 * Create a new memory pool
 * Returns NULL on failure, valid handle on success
 */
yq_mempool *yq_mempool_create(void);

/*
 * Destroy a memory pool and free all allocated objects
 */
void yq_mempool_destroy(yq_mempool *pool);

/*
 * Allocate an object from the pool
 * Size must be <= YQ_MEMPOOL_SMALL_OBJ_SIZE
 * Returns NULL on failure
 */
void *yq_mempool_alloc(yq_mempool *pool, size_t size);

/*
 * Free an object back to the pool
 * Objects must be allocated with yq_mempool_alloc()
 */
void yq_mempool_free(yq_mempool *pool, void *ptr);

/*
 * Get statistics about the pool
 */
typedef struct yq_mempool_stats {
    uint32_t struct_size;        /* Must be sizeof(yq_mempool_stats) */
    uint32_t chunks_allocated;  /* Number of memory chunks allocated */
    uint32_t objects_allocated;  /* Number of objects currently allocated */
    uint32_t objects_freed;      /* Lifetime count of freed objects */
    uint32_t free_objects;       /* Objects currently in free list */
    uint64_t memory_used;        /* Total memory used by chunks */
    uint64_t memory_allocated;   /* Total memory allocated from system */
    uint32_t reserved[4];        /* Must be zero */
} yq_mempool_stats;

int yq_mempool_stats_get(yq_mempool *pool, yq_mempool_stats *stats);

/*
 * Reset the pool (destroy all objects but keep chunks for reuse)
 * This is faster than destroy+create for repeated workloads
 */
void yq_mempool_reset(yq_mempool *pool);

#ifdef __cplusplus
}
#endif

#endif /* YQ_MEMPOOL_H */