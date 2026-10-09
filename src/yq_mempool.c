#if !defined(_WIN32)
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "yq_mempool.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#include <stdatomic.h>
#endif

/*
 * Memory chunk - contains multiple objects
 */
struct yq_memchunk {
    char *memory;               /* Base memory address */
    atomic_size_t used;         /* Bytes used in this chunk */
    atomic_size_t corruption_check; /* Corruption detection */
    struct yq_memchunk *next;   /* Next chunk in list */
};

/*
 * Free object in the free list
 */
struct yq_freeobj {
    struct yq_freeobj *next;    /* Next free object */
};

/*
 * Memory pool implementation
 */
struct yq_mempool {
    atomic_uintptr_t free_list;       /* List of free small objects */
    atomic_uintptr_t chunks;          /* List of memory chunks */
    atomic_size_t chunks_count;       /* Number of chunks */
    atomic_size_t objects_allocated;  /* Current allocated objects */
    atomic_size_t objects_freed;      /* Lifetime freed objects */
    atomic_size_t free_objects;       /* Objects in free list */
    atomic_size_t active_writers;     /* Writer lock for thread safety */
    atomic_size_t generation;         /* Generation counter for reset detection */
    atomic_size_t corruption_check;   /* Corruption detection */
};

/* Round up to alignment boundary */
static size_t round_up(size_t n, size_t align) {
    return (n + align - 1) & ~(align - 1);
}

/* Create a new memory chunk */
static struct yq_memchunk *memchunk_create(void) {
#ifdef _WIN32
    void *p = VirtualAlloc(NULL, YQ_MEMPOOL_CHUNK_SIZE, 
                          MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    void *p = mmap(NULL, YQ_MEMPOOL_CHUNK_SIZE, 
                   PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) p = NULL;
#endif
    if (!p) return NULL;
    
    struct yq_memchunk *chunk = malloc(sizeof(struct yq_memchunk));
    if (!chunk) {
#ifdef _WIN32
        VirtualFree(p, 0, MEM_RELEASE);
#else
        munmap(p, YQ_MEMPOOL_CHUNK_SIZE);
#endif
        return NULL;
    }
    
    chunk->memory = (char *)p;
    atomic_init(&chunk->used, 0);
    atomic_init(&chunk->corruption_check, 0x12345678);
    chunk->next = NULL;
    return chunk;
}

/* Destroy a memory chunk */
static void memchunk_destroy(struct yq_memchunk *chunk) {
    if (!chunk) return;
    
    /* Validate chunk integrity */
    if (atomic_load(&chunk->corruption_check) != 0x12345678) {
        /* Corrupted chunk detected, handle safely */
        return;
    }
    
    /* Mark as being destroyed */
    atomic_store(&chunk->corruption_check, 0xDEADBEEF);
    
    if (chunk->memory) {
#ifdef _WIN32
        VirtualFree(chunk->memory, 0, MEM_RELEASE);
#else
        munmap(chunk->memory, YQ_MEMPOOL_CHUNK_SIZE);
#endif
    }
    
    /* Zero sensitive data before freeing */
    atomic_store(&chunk->used, 0);
    atomic_store(&chunk->corruption_check, 0);
    free(chunk);
}

yq_mempool *yq_mempool_create(void) {
    if (!sizeof(yq_mempool)) return NULL;
    yq_mempool *pool = malloc(sizeof(yq_mempool));
    if (!pool) return NULL;
    
    /* Initialize atomic fields safely */
    atomic_init(&pool->free_list, (uintptr_t)NULL);
    atomic_init(&pool->chunks, (uintptr_t)NULL);
    atomic_init(&pool->chunks_count, 0);
    atomic_init(&pool->objects_allocated, 0);
    atomic_init(&pool->objects_freed, 0);
    atomic_init(&pool->free_objects, 0);
    atomic_init(&pool->active_writers, 0);
    atomic_init(&pool->generation, 1);
    atomic_init(&pool->corruption_check, 0x12345678);
    
    /* Validate initialization */
    if (atomic_load(&pool->corruption_check) != 0x12345678) {
        free(pool);
        return NULL;
    }
    
    return pool;
}

void yq_mempool_destroy(yq_mempool *pool) {
    if (!pool) return;
    
    /* Validate pool integrity */
    if (atomic_load(&pool->corruption_check) != 0x12345678) {
        /* Corrupted pool detected, handle safely */
        return;
    }
    
    /* Validate bounds */
    uint32_t chunks_count = atomic_load(&pool->chunks_count);
    if (chunks_count > (1ULL << 20)) return; /* Prevent overflow */
    
    /* Mark as being destroyed */
    atomic_store(&pool->corruption_check, 0xDEADBEEF);
    
    /* Free all chunks */
    struct yq_memchunk *chunk = (struct yq_memchunk *)atomic_load(&pool->chunks);
    while (chunk) {
        struct yq_memchunk *next = chunk->next;
        memchunk_destroy(chunk);
        chunk = next;
    }
    
    /* Zero sensitive data before freeing */
    atomic_store(&pool->free_list, (uintptr_t)NULL);
    atomic_store(&pool->chunks, (uintptr_t)NULL);
    atomic_store(&pool->chunks_count, 0);
    atomic_store(&pool->objects_allocated, 0);
    atomic_store(&pool->objects_freed, 0);
    atomic_store(&pool->free_objects, 0);
    atomic_store(&pool->corruption_check, 0);
    
    /* Free the pool itself */
    free(pool);
}

void *yq_mempool_alloc(yq_mempool *pool, size_t size) {
    if (!pool || !size || size > YQ_MEMPOOL_SMALL_OBJ_SIZE) {
        return NULL;
    }
    
    /* Validate pool integrity */
    if (atomic_load(&pool->corruption_check) != 0x12345678) {
        return NULL;
    }
    
    /* Validate allocation size limits */
    if (size > (1ULL << 20)) return NULL; /* 1MB limit */
    if (size == 0) return NULL;
    
    /* Round up size to alignment boundary */
    size = round_up(size, 8);
    if (size > (1ULL << 20)) return NULL; /* Aligned size must also be within 1MB */
    
    /* Thread safety: acquire writer lock */
    if (atomic_fetch_add(&pool->active_writers, 1) != 0) {
        /* Another writer is active, wait or fail */
        atomic_fetch_add(&pool->active_writers, -1);
        return NULL;
    }
    
    /* First, try to get from free list */
    struct yq_freeobj *free_list = (struct yq_freeobj *)atomic_load(&pool->free_list);
    if (free_list) {
        struct yq_freeobj *obj = free_list;
        atomic_store(&pool->free_list, (uintptr_t)obj->next);
        atomic_fetch_add(&pool->free_objects, -1);
        atomic_fetch_add(&pool->objects_allocated, 1);
        memset(obj, 0, size);  /* Zero-fill for security */
        atomic_fetch_add(&pool->active_writers, -1);
        return obj;
    }
    
    /* No free objects available, allocate from a new chunk */
    struct yq_memchunk *chunks = (struct yq_memchunk *)atomic_load(&pool->chunks);
    if (chunks == NULL || atomic_load(&chunks->used) + size > YQ_MEMPOOL_CHUNK_SIZE) {
        /* Need a new chunk */
        uint32_t chunks_count = atomic_load(&pool->chunks_count);
        if (chunks_count > (1ULL << 20)) {
            atomic_fetch_add(&pool->active_writers, -1);
            return NULL; /* Prevent overflow */
        }
        
        struct yq_memchunk *new_chunk = memchunk_create();
        if (!new_chunk) {
            atomic_fetch_add(&pool->active_writers, -1);
            return NULL;
        }
        
        new_chunk->next = chunks;
        atomic_store(&pool->chunks, (uintptr_t)new_chunk);
        atomic_fetch_add(&pool->chunks_count, 1);
        chunks = new_chunk;
    }
    
    size_t chunk_used = atomic_load(&chunks->used);
    void *ptr = chunks->memory + chunk_used;
    
    /* Validate bounds and prevent overflow */
    if (chunk_used > (1ULL << 30) || YQ_MEMPOOL_CHUNK_SIZE > (1ULL << 30)) {
        atomic_fetch_add(&pool->active_writers, -1);
        return NULL;
    }
    
    if (chunk_used + size > YQ_MEMPOOL_CHUNK_SIZE) {
        atomic_fetch_add(&pool->active_writers, -1);
        return NULL;
    }
    
    /* Perform allocation with memory barrier */
    #ifdef _WIN32
    MemoryBarrier();
    #else
    __sync_synchronize();
    #endif
    
    /* Update atomic fields */
    atomic_store(&chunks->used, chunk_used + size);
    atomic_fetch_add(&pool->objects_allocated, 1);
    
    /* Release writer lock */
    atomic_fetch_add(&pool->active_writers, -1);
    
    /* Validate allocation succeeded */
    if (atomic_load(&chunks->used) != chunk_used + size) {
        /* Allocation failed, rollback */
        atomic_store(&chunks->used, chunk_used);
        atomic_fetch_add(&pool->objects_allocated, -1);
        return NULL;
    }
    
    memset(ptr, 0, size);  /* Zero-fill for security */
    return ptr;
}

void yq_mempool_free(yq_mempool *pool, void *ptr) {
    if (!pool || !ptr) return;
    
    /* Validate pool integrity */
    if (atomic_load(&pool->corruption_check) != 0x12345678) {
        return;
    }
    
    /* Thread safety: acquire writer lock */
    if (atomic_fetch_add(&pool->active_writers, 1) != 0) {
        /* Another writer is active, wait or fail */
        atomic_fetch_add(&pool->active_writers, -1);
        return;
    }
    
    /* Validate bounds and prevent underflow */
    uint32_t objects_allocated = atomic_load(&pool->objects_allocated);
    if (objects_allocated == 0) {
        atomic_fetch_add(&pool->active_writers, -1);
        return; /* Prevent underflow */
    }
    
    /* Add to free list */
    struct yq_freeobj *obj = (struct yq_freeobj *)ptr;
    obj->next = (struct yq_freeobj *)atomic_load(&pool->free_list);
    atomic_store(&pool->free_list, (uintptr_t)obj);
    
    /* Update atomic fields */
    atomic_fetch_add(&pool->objects_allocated, -1);
    atomic_fetch_add(&pool->objects_freed, 1);
    atomic_fetch_add(&pool->free_objects, 1);
    
    /* Release writer lock */
    atomic_fetch_add(&pool->active_writers, -1);
}

int yq_mempool_stats_get(yq_mempool *pool, yq_mempool_stats *stats) {
    /* 与全项目统一：错误码用 YQ_ERR_*，不返回裸 -1 */
    if (!pool || !stats) return YQ_ERR_INVAL;
    if (stats->struct_size != sizeof(yq_mempool_stats)) return YQ_ERR_INVAL;
    
    /* Validate pool integrity */
    if (atomic_load(&pool->corruption_check) != 0x12345678) {
        return YQ_ERR_INVAL;
    }
    
    /* Validate bounds */
    uint32_t chunks_count = atomic_load(&pool->chunks_count);
    if (chunks_count > (1ULL << 20)) return YQ_ERR_INVAL; /* Validate bounds */
    
    /* Memory barrier for consistent view */
    #ifdef _WIN32
    MemoryBarrier();
    #else
    __sync_synchronize();
    #endif
    
    stats->struct_size = sizeof(yq_mempool_stats);
    stats->chunks_allocated = chunks_count;
    stats->objects_allocated = atomic_load(&pool->objects_allocated);
    stats->objects_freed = atomic_load(&pool->objects_freed);
    stats->free_objects = atomic_load(&pool->free_objects);
    stats->memory_used = (uint64_t)chunks_count * YQ_MEMPOOL_CHUNK_SIZE;
    stats->memory_allocated = (uint64_t)chunks_count * YQ_MEMPOOL_CHUNK_SIZE;
    
    return YQ_OK;
}

void yq_mempool_reset(yq_mempool *pool) {
    if (!pool) return;
    
    /* Validate pool integrity */
    if (atomic_load(&pool->corruption_check) != 0x12345678) {
        return;
    }
    
    /* Thread safety: acquire writer lock */
    if (atomic_fetch_add(&pool->active_writers, 1) != 0) {
        /* Another writer is active, wait or fail */
        atomic_fetch_add(&pool->active_writers, -1);
        return;
    }
    
    /* Validate bounds */
    uint32_t chunks_count = atomic_load(&pool->chunks_count);
    if (chunks_count > (1ULL << 20)) {
        atomic_fetch_add(&pool->active_writers, -1);
        return; /* Prevent overflow */
    }
    
    /* Reset all chunks */
    struct yq_memchunk *chunk = (struct yq_memchunk *)atomic_load(&pool->chunks);
    while (chunk) {
        atomic_store(&chunk->used, 0);
        chunk = chunk->next;
    }
    
    /* Reset free list and counters with memory barrier */
    #ifdef _WIN32
    MemoryBarrier();
    #else
    __sync_synchronize();
    #endif
    
    atomic_store(&pool->free_list, (uintptr_t)NULL);
    atomic_store(&pool->objects_allocated, 0);
    atomic_store(&pool->objects_freed, 0);
    atomic_store(&pool->free_objects, 0);
    
    /* Update generation to detect reset */
    atomic_fetch_add(&pool->generation, 1);
    
    /* Release writer lock */
    atomic_fetch_add(&pool->active_writers, -1);
}