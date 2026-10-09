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

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

struct yq_memblk {
    char *base;
    size_t size;
    size_t used;
    struct yq_memblk *next;
};

static size_t round_up(size_t n, size_t align) {
    return (n + align - 1) & ~(align - 1);
}

yq_memblk *yq_memblk_create(size_t size) {
    if (size == 0) size = 4096;
    if (size > SIZE_MAX - 4096) return NULL;
    size = round_up(size, 4096);
    if (size > SIZE_MAX - 4096) return NULL;
    
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
    blk->base = (char *)p;
    blk->size = size;
    blk->used = 0;
    blk->next = NULL;
    return blk;
}

void yq_memblk_destroy(yq_memblk *b) {
    while (b) {
        yq_memblk *next = b->next;
        if (b->base) {
#ifdef _WIN32
            VirtualFree(b->base, 0, MEM_RELEASE);
#else
            munmap(b->base, b->size);
#endif
            b->base = NULL;
        }
        free(b);
        b = next;
    }
}

void *yq_memblk_alloc(yq_memblk *b, size_t size) {
    if (!b || size == 0) return NULL;
    if (size > SIZE_MAX - 8) return NULL;
    if (b->used > SIZE_MAX - 8) return NULL;
    
    size_t aligned = round_up(size, 8);
    if (b->used + aligned > b->size) return NULL;
    if (aligned > SIZE_MAX - 8) return NULL;
    
    void *ret = b->base + b->used;
    b->used += aligned;
    return ret;
}

void yq_memblk_reset(yq_memblk *b) {
    if (b) {
        b->used = 0;
    }
}

void *yq_memblk_base(yq_memblk *b) {
    if (!b) return NULL;
    if (!b->base) return NULL;
    return b->base;
}

size_t yq_memblk_used(yq_memblk *b) {
    if (!b) return 0;
    if (b->used > SIZE_MAX - 1) return 0;
    return b->used;
}
