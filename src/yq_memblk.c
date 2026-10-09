#if !defined(_WIN32)
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "yq_memblk.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

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

struct yq_memblk {
    char *base;
    atomic_size_t size;
    atomic_size_t used;
    atomic_size_t max_allocations;
    atomic_size_t allocation_count;
    atomic_size_t corruption_check;
    volatile uint32_t generation;
    volatile uint32_t active_writers;
    struct yq_memblk *next;
};

static size_t round_up(size_t n, size_t align) {
    return (n + align - 1) & ~(align - 1);
}

yq_memblk *yq_memblk_create(size_t size) {
    if (size == 0) size = 4096;
    if (size > (1ULL << 30)) return NULL; /* 1GB limit */
    size = round_up(size, 4096);
    
    /* Validate alignment requirements */
    if (size & (4096 - 1)) return NULL;
    
#ifdef _WIN32
    void *p = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) p = NULL;
#endif
    if (!p) return NULL;
    yq_memblk *blk = calloc(1, sizeof(yq_memblk));
    if (!blk) {
#ifdef _WIN32
        VirtualFree(p, 0, MEM_RELEASE);
#else
        munmap(p, size);
#endif
        return NULL;
    }
    
    /* Initialize atomic fields safely */
    blk->base = (char *)p;
    atomic_init(&blk->size, size);
    atomic_init(&blk->used, 0);
    atomic_init(&blk->max_allocations, size / 64); /* Max ~64-byte allocations */
    atomic_init(&blk->allocation_count, 0);
    atomic_init(&blk->corruption_check, 0x12345678);
    blk->generation = 1;
    blk->active_writers = 0;
    blk->next = NULL;
    
    /* Validate initialization */
    memory_barrier();
    if (atomic_load(&blk->size) != size || atomic_load(&blk->used) != 0) {
        free(blk);
#ifdef _WIN32
        VirtualFree(p, 0, MEM_RELEASE);
#else
        munmap(p, size);
#endif
        return NULL;
    }
    
    return blk;
}

void yq_memblk_destroy(yq_memblk *b) {
    if (!b) return;
    
    /* Validate memory block integrity before destruction */
    if (atomic_load(&b->corruption_check) != 0x12345678) {
        /* Corrupted memory block detected, handle safely */
        return;
    }
    
    /* Mark as being destroyed */
    atomic_store(&b->corruption_check, 0xDEADBEEF);
    memory_barrier();
    
    while (b) {
        yq_memblk *next = b->next;
        
        /* Validate base pointer and size before freeing */
        if (b->base) {
            size_t block_size = atomic_load(&b->size);
            if (block_size > 0 && block_size <= (1ULL << 30)) {
#ifdef _WIN32
                VirtualFree(b->base, 0, MEM_RELEASE);
#else
                munmap(b->base, block_size);
#endif
            }
        }
        
        /* Zero sensitive data before freeing */
        atomic_store(&b->size, 0);
        atomic_store(&b->used, 0);
        atomic_store(&b->corruption_check, 0);
        free(b);
        b = next;
    }
}

void *yq_memblk_alloc(yq_memblk *b, size_t size) {
    if (!b || !size) return NULL;
    
    /* Validate memory block integrity */
    if (atomic_load(&b->corruption_check) != 0x12345678) {
        return NULL;
    }
    
    /* Validate allocation size limits */
    if (size > (1ULL << 20)) return NULL; /* 1MB limit */
    if (size == 0) return NULL;
    
    size_t aligned = round_up(size, 8);
    if (aligned > (1ULL << 20)) return NULL; /* Aligned size must also be within 1MB */
    
    /* Thread safety: acquire writer lock */
    if (atomic_fetch_add(&b->active_writers, 1) != 0) {
        /* Another writer is active, wait or fail */
        atomic_fetch_add(&b->active_writers, -1);
        return NULL;
    }
    
    /* Check allocation limits */
    size_t current_used = atomic_load(&b->used);
    size_t current_size = atomic_load(&b->size);
    size_t current_allocations = atomic_load(&b->allocation_count);
    
    /* Validate bounds and prevent overflow */
    if (current_used > (1ULL << 30) || current_size > (1ULL << 30)) {
        atomic_fetch_add(&b->active_writers, -1);
        return NULL;
    }
    
    /* Check if allocation fits */
    if (current_used + aligned > current_size) {
        atomic_fetch_add(&b->active_writers, -1);
        return NULL;
    }
    
    /* Check allocation count limit */
    if (current_allocations >= atomic_load(&b->max_allocations)) {
        atomic_fetch_add(&b->active_writers, -1);
        return NULL;
    }
    
    /* Perform allocation with memory barrier */
    memory_barrier();
    void *ret = b->base + current_used;
    
    /* Update atomic fields */
    atomic_store(&b->used, current_used + aligned);
    atomic_fetch_add(&b->allocation_count, 1);
    
    /* Release writer lock */
    atomic_fetch_add(&b->active_writers, -1);
    
    /* Validate allocation succeeded */
    if (atomic_load(&b->used) != current_used + aligned) {
        /* Allocation failed, rollback */
        atomic_store(&b->used, current_used);
        atomic_fetch_add(&b->allocation_count, -1);
        return NULL;
    }
    
    return ret;
}

void yq_memblk_reset(yq_memblk *b) {
    if (!b) return;
    
    /* Validate memory block integrity */
    if (atomic_load(&b->corruption_check) != 0x12345678) {
        return;
    }
    
    /* Thread safety: acquire writer lock */
    if (atomic_fetch_add(&b->active_writers, 1) != 0) {
        /* Another writer is active, wait or fail */
        atomic_fetch_add(&b->active_writers, -1);
        return;
    }
    
    /* Validate bounds */
    size_t current_size = atomic_load(&b->size);
    if (current_size > (1ULL << 30)) {
        atomic_fetch_add(&b->active_writers, -1);
        return;
    }
    
    /* Reset with memory barrier */
    memory_barrier();
    atomic_store(&b->used, 0);
    atomic_store(&b->allocation_count, 0);
    
    /* Update generation to detect reset */
    b->generation++;
    
    /* Release writer lock */
    atomic_fetch_add(&b->active_writers, -1);
}

void *yq_memblk_base(yq_memblk *b) {
    if (!b) return NULL;
    
    /* Validate memory block integrity */
    if (atomic_load(&b->corruption_check) != 0x12345678) {
        return NULL;
    }
    
    /* Validate bounds */
    size_t current_size = atomic_load(&b->size);
    if (current_size > (1ULL << 30)) {
        return NULL;
    }
    
    /* Memory barrier for consistent view */
    memory_barrier();
    return b->base;
}

size_t yq_memblk_used(yq_memblk *b) {
    if (!b) return 0;
    
    /* Validate memory block integrity */
    if (atomic_load(&b->corruption_check) != 0x12345678) {
        return 0;
    }
    
    /* Validate bounds */
    size_t current_size = atomic_load(&b->size);
    if (current_size > (1ULL << 30)) {
        return 0;
    }
    
    /* Memory barrier for consistent view */
    memory_barrier();
    return atomic_load(&b->used);
}

size_t yq_memblk_size(yq_memblk *b) {
    if (!b) return 0;
    
    /* Validate memory block integrity */
    if (atomic_load(&b->corruption_check) != 0x12345678) {
        return 0;
    }
    
    /* Validate bounds */
    size_t current_size = atomic_load(&b->size);
    if (current_size > (1ULL << 30)) {
        return 0;
    }
    
    /* Memory barrier for consistent view */
    memory_barrier();
    return current_size;
}
