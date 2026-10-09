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
#endif

/*
 * Memory chunk - contains multiple objects
 */
struct yq_memchunk {
    char *memory;               /* Base memory address */
    size_t used;                /* Bytes used in this chunk */
    struct yq_memchunk *next;   /* Next chunk in list */
};

/*
 * Free object in the free list
 */
struct yq_freeobj {
    struct yq_freeobj *next;    /* Next free object */
};

/*
 * Header stored in front of every pool object.
 *
 * The pool used to keep one mixed-size free list: free() pushed any object
 * onto it and alloc() popped any object off it, regardless of the slot size
 * the object originally occupied. A 256-byte request could therefore be
 * served a 64-byte slot and the write silently ran past it into the
 * neighbouring object. free() now reads the header back to learn the slot
 * size and returns the object to the matching size class, and alloc() only
 * ever reuses a slot of its own class.
 */
#define YQ_MEMPOOL_OBJ_MAGIC 0x59514F42u  /* "YQOB" */

typedef struct {
    uint32_t slot_size;         /* Usable bytes behind this header */
    uint32_t magic;             /* YQ_MEMPOOL_OBJ_MAGIC */
} yq_obj_header;

/* Number of size classes: 8, 16, ... YQ_MEMPOOL_SMALL_OBJ_SIZE bytes. */
#define YQ_MEMPOOL_CLASS_COUNT (YQ_MEMPOOL_SMALL_OBJ_SIZE / 8)

#define YQ_MEMPOOL_HDR_SIZE (sizeof(yq_obj_header))

/*
 * Memory pool implementation
 */
struct yq_mempool {
    struct yq_freeobj *free_classes[YQ_MEMPOOL_CLASS_COUNT]; /* per-size free lists */
    struct yq_memchunk *chunks;      /* List of memory chunks */
    uint32_t chunks_count;          /* Number of chunks */
    uint32_t objects_allocated;      /* Current allocated objects */
    uint32_t objects_freed;         /* Lifetime freed objects */
    uint32_t free_objects;          /* Objects in all free lists */
};

/* Round up to alignment boundary */
static size_t round_up(size_t n, size_t align) {
    return (n + align - 1) & ~(align - 1);
}

/* Size class index for an already rounded-up slot size (8..256). */
static size_t size_class(size_t rounded) {
    return rounded / 8 - 1;
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
    chunk->used = 0;
    chunk->next = NULL;
    return chunk;
}

/* Destroy a memory chunk */
static void memchunk_destroy(struct yq_memchunk *chunk) {
    if (chunk) {
        if (chunk->memory) {
#ifdef _WIN32
            VirtualFree(chunk->memory, 0, MEM_RELEASE);
#else
            munmap(chunk->memory, YQ_MEMPOOL_CHUNK_SIZE);
#endif
        }
        free(chunk);
    }
}

yq_mempool *yq_mempool_create(void) {
    yq_mempool *pool = malloc(sizeof(yq_mempool));
    if (!pool) return NULL;
    
    for (size_t i = 0; i < YQ_MEMPOOL_CLASS_COUNT; i++) {
        pool->free_classes[i] = NULL;
    }
    pool->chunks = NULL;
    pool->chunks_count = 0;
    pool->objects_allocated = 0;
    pool->objects_freed = 0;
    pool->free_objects = 0;
    
    return pool;
}

void yq_mempool_destroy(yq_mempool *pool) {
    if (!pool) return;
    
    /* Free all chunks */
    struct yq_memchunk *chunk = pool->chunks;
    while (chunk) {
        struct yq_memchunk *next = chunk->next;
        memchunk_destroy(chunk);
        chunk = next;
    }
    
    /* Free the pool itself */
    free(pool);
}

void *yq_mempool_alloc(yq_mempool *pool, size_t size) {
    if (!pool || size == 0 || size > YQ_MEMPOOL_SMALL_OBJ_SIZE) {
        return NULL;
    }
    
    /* Round up size to alignment boundary */
    size_t rounded = round_up(size, 8);
    size_t cls = size_class(rounded);

    /* First, try to get an object of exactly this size class. A slot freed
     * from another class is never handed out here: mixing them is what let
     * a small slot be reused for a large request. */
    if (pool->free_classes[cls]) {
        struct yq_freeobj *obj = pool->free_classes[cls];
        pool->free_classes[cls] = obj->next;
        pool->free_objects--;
        pool->objects_allocated++;
        memset(obj, 0, size);  /* Zero-fill for security */
        return obj;
    }
    
    /* No free objects of this class, allocate from a chunk */
    if (pool->chunks == NULL ||
        pool->chunks->used + YQ_MEMPOOL_HDR_SIZE + rounded > YQ_MEMPOOL_CHUNK_SIZE) {
        /* Need a new chunk */
        struct yq_memchunk *new_chunk = memchunk_create();
        if (!new_chunk) return NULL;
        
        new_chunk->next = pool->chunks;
        pool->chunks = new_chunk;
        pool->chunks_count++;
    }
    
    struct yq_memchunk *chunk = pool->chunks;
    yq_obj_header *hdr = (yq_obj_header *)(chunk->memory + chunk->used);
    hdr->slot_size = (uint32_t)rounded;
    hdr->magic = YQ_MEMPOOL_OBJ_MAGIC;
    chunk->used += YQ_MEMPOOL_HDR_SIZE + rounded;

    void *ptr = (char *)hdr + YQ_MEMPOOL_HDR_SIZE;
    pool->objects_allocated++;
    memset(ptr, 0, size);  /* Zero-fill for security */
    return ptr;
}

void yq_mempool_free(yq_mempool *pool, void *ptr) {
    if (!pool || !ptr) return;
    
    /*
     * Recover the slot size from the header in front of the object so it can
     * go back to its own size class. A pointer that does not carry a pool
     * header is not ours: silently queueing it used to corrupt the free list
     * (and, with one mixed list, served wrong-sized slots), so ignore it.
     */
    yq_obj_header *hdr = (yq_obj_header *)((char *)ptr - YQ_MEMPOOL_HDR_SIZE);
    if (hdr->magic != YQ_MEMPOOL_OBJ_MAGIC) return;
    if (hdr->slot_size == 0 || hdr->slot_size > YQ_MEMPOOL_SMALL_OBJ_SIZE ||
        (hdr->slot_size & 7u) != 0) {
        return;
    }

    size_t cls = size_class(hdr->slot_size);
    
    /* Add to the free list of its size class */
    struct yq_freeobj *obj = (struct yq_freeobj *)ptr;
    obj->next = pool->free_classes[cls];
    pool->free_classes[cls] = obj;
    
    pool->objects_allocated--;
    pool->objects_freed++;
    pool->free_objects++;
}

int yq_mempool_stats_get(yq_mempool *pool, yq_mempool_stats *stats) {
    /* 与全项目统一：错误码用 YQ_ERR_*，不返回裸 -1 */
    if (!pool || !stats) return YQ_ERR_INVAL;
    if (stats->struct_size != sizeof(yq_mempool_stats)) return YQ_ERR_INVAL;
    
    stats->struct_size = sizeof(yq_mempool_stats);
    stats->chunks_allocated = pool->chunks_count;
    stats->objects_allocated = pool->objects_allocated;
    stats->objects_freed = pool->objects_freed;
    stats->free_objects = pool->free_objects;
    stats->memory_used = (uint64_t)pool->chunks_count * YQ_MEMPOOL_CHUNK_SIZE;
    stats->memory_allocated = (uint64_t)pool->chunks_count * YQ_MEMPOOL_CHUNK_SIZE;
    
    return YQ_OK;
}

void yq_mempool_reset(yq_mempool *pool) {
    if (!pool) return;
    
    /* Reset all chunks */
    struct yq_memchunk *chunk = pool->chunks;
    while (chunk) {
        chunk->used = 0;
        chunk = chunk->next;
    }
    
    /* Reset every free list and the counters */
    for (size_t i = 0; i < YQ_MEMPOOL_CLASS_COUNT; i++) {
        pool->free_classes[i] = NULL;
    }
    pool->objects_allocated = 0;
    pool->objects_freed = 0;
    pool->free_objects = 0;
}
