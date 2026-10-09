/*
 * Copyright (c) 2024 YQteam-hq
 *
 * This file is part of the YQDB project.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "yq_frag.h"
#include "yq_alloc.h"
#include "yq_atomic.h"
#include "yq_security.h"
#include <string.h>
#include <stdlib.h>

/* Memory fragmentation analyzer implementation */

/* Block allocation strategy implementation */
static int frag_strategy_best_fit(yq_frag_analyzer *analyzer, size_t size) {
    if (!analyzer || size == 0) return YQ_ERR_INVAL;
    
    yq_frag_block *best_block = NULL;
    size_t best_size = SIZE_MAX;
    
    yq_frag_block *current = analyzer->free_blocks;
    while (current) {
        if (current->size >= size && current->size < best_size) {
            best_block = current;
            best_size = current->size;
        }
        current = current->next;
    }
    
    if (!best_block) return YQ_ERR_NOTFOUND;
    
    /* Remove from free list */
    frag_remove_block(analyzer, best_block, analyzer->free_blocks);
    
    /* Create allocated block */
    yq_frag_block *allocated = frag_create_block(best_block->address, size, 1);
    if (!allocated) return YQ_ERR_NOMEM;
    
    frag_insert_block(analyzer, allocated, analyzer->allocated_blocks);
    
    /* Handle remaining space */
    if (best_size > size) {
        yq_frag_block *remaining = frag_create_block(
            best_block->address + size, 
            best_size - size, 
            0
        );
        if (remaining) {
            frag_insert_block(analyzer, remaining, analyzer->free_blocks);
        }
    }
    
    frag_destroy_block(best_block);
    return YQ_OK;
}

static int frag_strategy_first_fit(yq_frag_analyzer *analyzer, size_t size) {
    if (!analyzer || size == 0) return YQ_ERR_INVAL;
    
    yq_frag_block *current = analyzer->free_blocks;
    while (current) {
        if (current->size >= size) {
            /* Remove from free list */
            frag_remove_block(analyzer, current, analyzer->free_blocks);
            
            /* Create allocated block */
            yq_frag_block *allocated = frag_create_block(current->address, size, 1);
            if (!allocated) {
                frag_insert_block(analyzer, current, analyzer->free_blocks);
                return YQ_ERR_NOMEM;
            }
            
            frag_insert_block(analyzer, allocated, analyzer->allocated_blocks);
            
            /* Handle remaining space */
            if (current->size > size) {
                yq_frag_block *remaining = frag_create_block(
                    current->address + size, 
                    current->size - size, 
                    0
                );
                if (remaining) {
                    frag_insert_block(analyzer, remaining, analyzer->free_blocks);
                }
            }
            
            frag_destroy_block(current);
            return YQ_OK;
        }
        current = current->next;
    }
    
    return YQ_ERR_NOTFOUND;
}

static int frag_strategy_worst_fit(yq_frag_analyzer *analyzer, size_t size) {
    if (!analyzer || size == 0) return YQ_ERR_INVAL;
    
    yq_frag_block *worst_block = NULL;
    size_t worst_size = 0;
    
    yq_frag_block *current = analyzer->free_blocks;
    while (current) {
        if (current->size >= size && current->size > worst_size) {
            worst_block = current;
            worst_size = current->size;
        }
        current = current->next;
    }
    
    if (!worst_block) return YQ_ERR_NOTFOUND;
    
    /* Remove from free list */
    frag_remove_block(analyzer, worst_block, analyzer->free_blocks);
    
    /* Create allocated block */
    yq_frag_block *allocated = frag_create_block(worst_block->address, size, 1);
    if (!allocated) {
        frag_insert_block(analyzer, worst_block, analyzer->free_blocks);
        return YQ_ERR_NOMEM;
    }
    
    frag_insert_block(analyzer, allocated, analyzer->allocated_blocks);
    
    /* Handle remaining space */
    if (worst_size > size) {
        yq_frag_block *remaining = frag_create_block(
            worst_block->address + size, 
            worst_size - size, 
            0
        );
        if (remaining) {
            frag_insert_block(analyzer, remaining, analyzer->free_blocks);
        }
    }
    
    frag_destroy_block(worst_block);
    return YQ_OK;
}

/* Block management utilities */
static yq_frag_block *frag_create_block(uint64_t address, size_t size, int allocated) {
    if (address == 0 || size == 0) return NULL;
    
    yq_frag_block *block = yq_malloc(sizeof(yq_frag_block));
    if (!block) return NULL;
    
    block->address = address;
    block->size = size;
    block->allocated = allocated;
    block->fragmentation_score = 0;
    block->next = NULL;
    block->prev = NULL;
    block->generation = 1;
    block->ref_count = 1;
    
    return block;
}

static void frag_destroy_block(yq_frag_block *block) {
    if (!block) return;
    
    /* Validate block state */
    if (block->generation != 1) {
        return;
    }
    
    /* Secure zero sensitive data */
    yq_security_zero(block, sizeof(*block));
    
    yq_free(block);
}

static void frag_insert_block(yq_frag_analyzer *analyzer, yq_frag_block *block, yq_frag_block **list) {
    if (!analyzer || !block || !list) return;
    
    /* Validate analyzer state */
    if (analyzer->generation != 1) {
        return;
    }
    
    block->next = *list;
    block->prev = NULL;
    
    if (*list) {
        (*list)->prev = block;
    }
    
    *list = block;
    analyzer->block_count++;
}

static void frag_remove_block(yq_frag_analyzer *analyzer, yq_frag_block *block, yq_frag_block **list) {
    if (!analyzer || !block || !list) return;
    
    /* Validate analyzer state */
    if (analyzer->generation != 1) {
        return;
    }
    
    if (block->prev) {
        block->prev->next = block->next;
    } else {
        *list = block->next;
    }
    
    if (block->next) {
        block->next->prev = block->prev;
    }
    
    block->next = NULL;
    block->prev = NULL;
    analyzer->block_count--;
}

/* Fragmentation calculation utilities */
static double frag_calculate_external_fragmentation(yq_frag_analyzer *analyzer) {
    if (!analyzer) return 0.0;
    
    size_t total_free = 0;
    size_t largest_free = 0;
    size_t free_block_count = 0;
    
    yq_frag_block *current = analyzer->free_blocks;
    while (current) {
        total_free += current->size;
        if (current->size > largest_free) {
            largest_free = current->size;
        }
        free_block_count++;
        current = current->next;
    }
    
    if (total_free == 0) return 0.0;
    
    /* External fragmentation = (Total free - Largest free) / Total free */
    return ((double)(total_free - largest_free)) / total_free;
}

static double frag_calculate_internal_fragmentation(yq_frag_analyzer *analyzer) {
    if (!analyzer) return 0.0;
    
    size_t total_allocated = 0;
    size_t total_requested = 0;
    
    yq_frag_block *current = analyzer->allocated_blocks;
    while (current) {
        total_allocated += current->size;
        /* Estimate requested size (assuming 16-byte alignment) */
        size_t requested = (current->size + 15) & ~15;
        total_requested += requested;
        current = current->next;
    }
    
    if (total_requested == 0) return 0.0;
    
    /* Internal fragmentation = (Total allocated - Total requested) / Total requested */
    return ((double)(total_allocated - total_requested)) / total_requested;
}

static void frag_update_fragmentation_scores(yq_frag_analyzer *analyzer) {
    if (!analyzer) return;
    
    /* Calculate external fragmentation score */
    analyzer->stats.external_frag = frag_calculate_external_fragmentation(analyzer);
    
    /* Calculate internal fragmentation score */
    analyzer->stats.internal_frag = frag_calculate_internal_fragmentation(analyzer);
    
    /* Calculate total fragmentation score */
    analyzer->stats.total_frag = (analyzer->stats.external_frag + analyzer->stats.internal_frag) / 2.0;
    
    /* Update individual block scores */
    yq_frag_block *current = analyzer->free_blocks;
    while (current) {
        /* Fragmentation score based on block size relative to average */
        size_t avg_free_size = analyzer->stats.total_free / 
                              (analyzer->stats.free_blocks > 0 ? analyzer->stats.free_blocks : 1);
        
        if (avg_free_size > 0) {
            current->fragmentation_score = 
                ((double)abs((int)(current->size - avg_free_size))) / avg_free_size;
        } else {
            current->fragmentation_score = 0.0;
        }
        
        current = current->next;
    }
}

/* Public API implementation */
yq_frag_analyzer *yq_frag_analyzer_create(size_t total_memory) {
    if (total_memory == 0) return NULL;
    
    yq_frag_analyzer *analyzer = yq_malloc(sizeof(yq_frag_analyzer));
    if (!analyzer) return NULL;
    
    /* Initialize analyzer structure */
    analyzer->blocks = NULL;
    analyzer->free_blocks = NULL;
    analyzer->allocated_blocks = NULL;
    analyzer->block_count = 0;
    analyzer->last_analysis = 0;
    analyzer->auto_defrag = true;
    analyzer->defrag_threshold = 0.3; /* 30% fragmentation threshold */
    analyzer->max_defrag_size = total_memory / 10; /* 10% of total memory */
    
    /* Initialize statistics */
    analyzer->stats.total_memory = total_memory;
    analyzer->stats.total_allocated = 0;
    analyzer->stats.total_free = total_memory;
    analyzer->stats.free_blocks = 0;
    analyzer->stats.allocated_blocks = 0;
    analyzer->stats.external_frag = 0.0;
    analyzer->stats.internal_frag = 0.0;
    analyzer->stats.total_frag = 0.0;
    analyzer->stats.defrag_operations = 0;
    analyzer->stats.defrag_freed = 0;
    
    /* Create initial free block covering entire memory space */
    yq_frag_block *initial_block = frag_create_block(0, total_memory, 0);
    if (!initial_block) {
        yq_free(analyzer);
        return NULL;
    }
    
    frag_insert_block(analyzer, initial_block, &analyzer->free_blocks);
    
    return analyzer;
}

void yq_frag_analyzer_destroy(yq_frag_analyzer *analyzer) {
    if (!analyzer) return;
    
    /* Validate analyzer state */
    if (analyzer->generation != 1) {
        return;
    }
    
    /* Destroy all blocks */
    yq_frag_block *current = analyzer->blocks;
    while (current) {
        yq_frag_block *next = current->next;
        frag_destroy_block(current);
        current = next;
    }
    
    /* Secure zero sensitive data */
    yq_security_zero(analyzer, sizeof(*analyzer));
    
    yq_free(analyzer);
}

int yq_frag_analyze(yq_frag_analyzer *analyzer, yq_frag_stats *stats) {
    if (!analyzer || !stats) return YQ_ERR_INVAL;
    
    /* Validate analyzer state */
    if (analyzer->generation != 1) {
        return YQ_ERR_INVAL;
    }
    
    /* Count free and allocated blocks */
    analyzer->stats.free_blocks = 0;
    analyzer->stats.allocated_blocks = 0;
    analyzer->stats.total_allocated = 0;
    analyzer->stats.total_free = 0;
    
    yq_frag_block *current = analyzer->free_blocks;
    while (current) {
        analyzer->stats.free_blocks++;
        analyzer->stats.total_free += current->size;
        current = current->next;
    }
    
    current = analyzer->allocated_blocks;
    while (current) {
        analyzer->stats.allocated_blocks++;
        analyzer->stats.total_allocated += current->size;
        current = current->next;
    }
    
    /* Update fragmentation scores */
    frag_update_fragmentation_scores(analyzer);
    
    /* Copy statistics to output */
    *stats = analyzer->stats;
    
    /* Update last analysis time */
    analyzer->last_analysis = yq_time_now();
    
    return YQ_OK;
}

int yq_frag_allocate(yq_frag_analyzer *analyzer, size_t size, uint64_t *address) {
    if (!analyzer || size == 0 || !address) return YQ_ERR_INVAL;
    
    /* Validate analyzer state */
    if (analyzer->generation != 1) {
        return YQ_ERR_INVAL;
    }
    
    /* Validate size bounds */
    if (size > analyzer->stats.total_memory) {
        return YQ_ERR_TOOBIG;
    }
    
    /* Check if we need to defragment */
    if (analyzer->auto_defrag && analyzer->stats.total_frag >= analyzer->defrag_threshold) {
        int result = yq_frag_defrag(analyzer, YQ_FRAG_STRATEGY_COMPACT);
        if (result == YQ_OK) {
            /* Retry allocation after defragmentation */
            return yq_frag_allocate(analyzer, size, address);
        }
    }
    
    /* Try allocation strategies in order of preference */
    int result = frag_strategy_best_fit(analyzer, size);
    if (result != YQ_OK) {
        result = frag_strategy_first_fit(analyzer, size);
        if (result != YQ_OK) {
            result = frag_strategy_worst_fit(analyzer, size);
        }
    }
    
    if (result == YQ_OK) {
        /* Find the allocated block and return its address */
        yq_frag_block *current = analyzer->allocated_blocks;
        while (current) {
            if (current->size >= size) {
                *address = current->address;
                return YQ_OK;
            }
            current = current->next;
        }
    }
    
    return YQ_ERR_NOMEM;
}

int yq_frag_free(yq_frag_analyzer *analyzer, uint64_t address, size_t size) {
    if (!analyzer || address == 0 || size == 0) return YQ_ERR_INVAL;
    
    /* Validate analyzer state */
    if (analyzer->generation != 1) {
        return YQ_ERR_INVAL;
    }
    
    /* Validate address and size bounds */
    if (address + size > analyzer->stats.total_memory) {
        return YQ_ERR_INVAL;
    }
    
    /* Find and remove the allocated block */
    yq_frag_block *current = analyzer->allocated_blocks;
    while (current) {
        if (current->address == address && current->size == size) {
            frag_remove_block(analyzer, current, &analyzer->allocated_blocks);
            
            /* Create free block */
            yq_frag_block *free_block = frag_create_block(address, size, 0);
            if (!free_block) {
                /* Put the block back if we can't create free block */
                frag_insert_block(analyzer, current, &analyzer->allocated_blocks);
                return YQ_ERR_NOMEM;
            }
            
            /* Try to merge with adjacent free blocks */
            frag_merge_adjacent_blocks(analyzer, free_block);
            
            return YQ_OK;
        }
        current = current->next;
    }
    
    return YQ_ERR_NOTFOUND;
}

int yq_frag_defrag(yq_frag_analyzer *analyzer, yq_frag_strategy strategy) {
    if (!analyzer) return YQ_ERR_INVAL;
    
    /* Validate analyzer state */
    if (analyzer->generation != 1) {
        return YQ_ERR_INVAL;
    }
    
    /* Check if defragmentation is needed */
    if (analyzer->stats.total_frag < analyzer->defrag_threshold) {
        return YQ_OK; /* No defragmentation needed */
    }
    
    /* Validate strategy */
    if (strategy < YQ_FRAG_STRATEGY_FIRST_FIT || strategy > YQ_FRAG_STRATEGY_COMPACT) {
        return YQ_ERR_INVAL;
    }
    
    size_t total_freed = 0;
    
    switch (strategy) {
        case YQ_FRAG_STRATEGY_COMPACT:
            /* Compact all allocated blocks to the beginning */
            total_freed = frag_compact_blocks(analyzer);
            break;
            
        case YQ_FRAG_STRATEGY_COALESCE:
            /* Coalesce adjacent free blocks */
            total_freed = frag_coalesce_blocks(analyzer);
            break;
            
        case YQ_FRAG_STRATEGY_RECLAIM:
            /* Reclaim small free blocks */
            total_freed = frag_reclaim_small_blocks(analyzer);
            break;
            
        default:
            return YQ_ERR_INVAL;
    }
    
    /* Update statistics */
    analyzer->stats.defrag_operations++;
    analyzer->stats.defrag_freed += total_freed;
    
    /* Recalculate fragmentation */
    yq_frag_analyze(analyzer, &analyzer->stats);
    
    return YQ_OK;
}

int yq_frag_set_auto_defrag(yq_frag_analyzer *analyzer, bool enabled) {
    if (!analyzer) return YQ_ERR_INVAL;
    
    /* Validate analyzer state */
    if (analyzer->generation != 1) {
        return YQ_ERR_INVAL;
    }
    
    analyzer->auto_defrag = enabled;
    return YQ_OK;
}

int yq_frag_set_defrag_threshold(yq_frag_analyzer *analyzer, double threshold) {
    if (!analyzer) return YQ_ERR_INVAL;
    
    /* Validate analyzer state */
    if (analyzer->generation != 1) {
        return YQ_ERR_INVAL;
    }
    
    /* Validate threshold (0.0 to 1.0) */
    if (threshold < 0.0 || threshold > 1.0) {
        return YQ_ERR_INVAL;
    }
    
    analyzer->defrag_threshold = threshold;
    return YQ_OK;
}

int yq_frag_set_max_defrag_size(yq_frag_analyzer *analyzer, size_t max_size) {
    if (!analyzer) return YQ_ERR_INVAL;
    
    /* Validate analyzer state */
    if (analyzer->generation != 1) {
        return YQ_ERR_INVAL;
    }
    
    /* Validate size bounds */
    if (max_size > analyzer->stats.total_memory) {
        return YQ_ERR_TOOBIG;
    }
    
    analyzer->max_defrag_size = max_size;
    return YQ_OK;
}

/* Advanced fragmentation analysis utilities */
int yq_frag_get_fragmentation_report(yq_frag_analyzer *analyzer, yq_frag_report *report) {
    if (!analyzer || !report) return YQ_ERR_INVAL;
    
    /* Validate analyzer state */
    if (analyzer->generation != 1) {
        return YQ_ERR_INVAL;
    }
    
    /* Analyze current state */
    int result = yq_frag_analyze(analyzer, &analyzer->stats);
    if (result != YQ_OK) {
        return result;
    }
    
    /* Initialize report */
    memset(report, 0, sizeof(yq_frag_report));
    
    /* Copy basic statistics */
    report->total_memory = analyzer->stats.total_memory;
    report->total_allocated = analyzer->stats.total_allocated;
    report->total_free = analyzer->stats.total_free;
    report->free_blocks = analyzer->stats.free_blocks;
    report->allocated_blocks = analyzer->stats.allocated_blocks;
    report->external_frag = analyzer->stats.external_frag;
    report->internal_frag = analyzer->stats.internal_frag;
    report->total_frag = analyzer->stats.total_frag;
    report->defrag_operations = analyzer->stats.defrag_operations;
    report->defrag_freed = analyzer->stats.defrag_freed;
    
    /* Calculate additional metrics */
    if (analyzer->stats.total_memory > 0) {
        report->utilization_ratio = (double)analyzer->stats.total_allocated / analyzer->stats.total_memory;
        report->fragmentation_penalty = analyzer->stats.total_frag * 100.0; /* Convert to percentage */
    } else {
        report->utilization_ratio = 0.0;
        report->fragmentation_penalty = 0.0;
    }
    
    /* Analyze free block distribution */
    if (analyzer->free_blocks > 0) {
        size_t *block_sizes = yq_malloc(analyzer->free_blocks * sizeof(size_t));
        if (!block_sizes) return YQ_ERR_NOMEM;
        
        yq_frag_block *current = analyzer->free_blocks;
        size_t index = 0;
        while (current && index < analyzer->free_blocks) {
            block_sizes[index++] = current->size;
            current = current->next;
        }
        
        /* Calculate free block statistics */
        report->avg_free_block_size = 0.0;
        report->max_free_block_size = 0;
        report->min_free_block_size = SIZE_MAX;
        
        for (size_t i = 0; i < index; i++) {
            report->avg_free_block_size += block_sizes[i];
            if (block_sizes[i] > report->max_free_block_size) {
                report->max_free_block_size = block_sizes[i];
            }
            if (block_sizes[i] < report->min_free_block_size) {
                report->min_free_block_size = block_sizes[i];
            }
        }
        
        if (index > 0) {
            report->avg_free_block_size /= index;
        }
        
        yq_free(block_sizes);
    }
    
    return YQ_OK;
}

/* Internal defragmentation implementation */
static size_t frag_compact_blocks(yq_frag_analyzer *analyzer) {
    if (!analyzer) return 0;
    
    size_t total_freed = 0;
    uint64_t current_address = 0;
    
    /* Sort allocated blocks by address */
    yq_frag_block *sorted_blocks = NULL;
    yq_frag_block *current = analyzer->allocated_blocks;
    
    while (current) {
        yq_frag_block *next = current->next;
        
        /* Insert sorted */
        if (!sorted_blocks || current->address < sorted_blocks->address) {
            current->next = sorted_blocks;
            current->prev = NULL;
            if (sorted_blocks) {
                sorted_blocks->prev = current;
            }
            sorted_blocks = current;
        } else {
            yq_frag_block *insert_pos = sorted_blocks;
            while (insert_pos->next && insert_pos->next->address < current->address) {
                insert_pos = insert_pos->next;
            }
            
            current->next = insert_pos->next;
            current->prev = insert_pos;
            if (insert_pos->next) {
                insert_pos->next->prev = current;
            }
            insert_pos->next = current;
        }
        
        current = next;
    }
    
    /* Compact blocks */
    yq_frag_block *current_block = sorted_blocks;
    while (current_block) {
        if (current_block->address != current_address) {
            /* Move block to current_address */
            size_t move_size = current_block->size;
            
            /* Remove from current position */
            frag_remove_block(analyzer, current_block, &analyzer->allocated_blocks);
            
            /* Update address */
            current_block->address = current_address;
            
            /* Insert back */
            frag_insert_block(analyzer, current_block, &analyzer->allocated_blocks);
            
            total_freed += move_size;
        }
        
        current_address += current_block->size;
        current_block = current_block->next;
    }
    
    /* Create single free block at the end */
    yq_frag_block *current_free = analyzer->free_blocks;
    while (current_free) {
        frag_remove_block(analyzer, current_free, &analyzer->free_blocks);
        frag_destroy_block(current_free);
        current_free = analyzer->free_blocks;
    }
    
    if (current_address < analyzer->stats.total_memory) {
        yq_frag_block *end_free = frag_create_block(current_address, 
                                                   analyzer->stats.total_memory - current_address, 
                                                   0);
        if (end_free) {
            frag_insert_block(analyzer, end_free, &analyzer->free_blocks);
        }
    }
    
    return total_freed;
}

static size_t frag_coalesce_blocks(yq_frag_analyzer *analyzer) {
    if (!analyzer) return 0;
    
    size_t total_freed = 0;
    
    /* Sort free blocks by address */
    yq_frag_block *sorted_blocks = NULL;
    yq_frag_block *current = analyzer->free_blocks;
    
    while (current) {
        yq_frag_block *next = current->next;
        
        /* Insert sorted */
        if (!sorted_blocks || current->address < sorted_blocks->address) {
            current->next = sorted_blocks;
            current->prev = NULL;
            if (sorted_blocks) {
                sorted_blocks->prev = current;
            }
            sorted_blocks = current;
        } else {
            yq_frag_block *insert_pos = sorted_blocks;
            while (insert_pos->next && insert_pos->next->address < current->address) {
                insert_pos = insert_pos->next;
            }
            
            current->next = insert_pos->next;
            current->prev = insert_pos;
            if (insert_pos->next) {
                insert_pos->next->prev = current;
            }
            insert_pos->next = current;
        }
        
        current = next;
    }
    
    /* Coalesce adjacent blocks */
    yq_frag_block *current_block = sorted_blocks;
    while (current_block && current_block->next) {
        yq_frag_block *next_block = current_block->next;
        
        /* Check if blocks are adjacent */
        if (current_block->address + current_block->size == next_block->address) {
            /* Merge blocks */
            size_t combined_size = current_block->size + next_block->size;
            
            /* Remove both blocks */
            frag_remove_block(analyzer, current_block, &analyzer->free_blocks);
            frag_remove_block(analyzer, next_block, &analyzer->free_blocks);
            
            /* Create merged block */
            yq_frag_block *merged = frag_create_block(current_block->address, combined_size, 0);
            if (merged) {
                frag_insert_block(analyzer, merged, &analyzer->free_blocks);
                total_freed += current_block->size; /* Count the space between blocks */
            }
            
            /* Destroy original blocks */
            frag_destroy_block(current_block);
            frag_destroy_block(next_block);
            
            /* Continue with merged block */
            current_block = merged;
        } else {
            current_block = next_block;
        }
    }
    
    return total_freed;
}

static size_t frag_reclaim_small_blocks(yq_frag_analyzer *analyzer) {
    if (!analyzer) return 0;
    
    size_t total_freed = 0;
    size_t small_block_threshold = 64; /* 64 bytes threshold */
    
    yq_frag_block *current = analyzer->free_blocks;
    while (current) {
        yq_frag_block *next = current->next;
        
        if (current->size < small_block_threshold) {
            /* Remove small block */
            frag_remove_block(analyzer, current, &analyzer->free_blocks);
            total_freed += current->size;
            frag_destroy_block(current);
        }
        
        current = next;
    }
    
    return total_freed;
}

static void frag_merge_adjacent_blocks(yq_frag_analyzer *analyzer, yq_frag_block *new_block) {
    if (!analyzer || !new_block) return;
    
    /* Check for merge with previous block */
    if (new_block->address > 0) {
        yq_frag_block *prev = analyzer->free_blocks;
        while (prev) {
            if (prev->address + prev->size == new_block->address) {
                /* Merge with previous block */
                size_t combined_size = prev->size + new_block->size;
                
                /* Remove both blocks */
                frag_remove_block(analyzer, prev, &analyzer->free_blocks);
                frag_remove_block(analyzer, new_block, &analyzer->free_blocks);
                
                /* Create merged block */
                yq_frag_block *merged = frag_create_block(prev->address, combined_size, 0);
                if (merged) {
                    frag_insert_block(analyzer, merged, &analyzer->free_blocks);
                }
                
                /* Destroy original blocks */
                frag_destroy_block(prev);
                frag_destroy_block(new_block);
                
                /* Continue with merged block */
                new_block = merged;
                break;
            }
            prev = prev->next;
        }
    }
    
    /* Check for merge with next block */
    if (new_block && new_block->next) {
        yq_frag_block *next = new_block->next;
        if (new_block->address + new_block->size == next->address) {
            /* Merge with next block */
            size_t combined_size = new_block->size + next->size;
            
            /* Remove both blocks */
            frag_remove_block(analyzer, new_block, &analyzer->free_blocks);
            frag_remove_block(analyzer, next, &analyzer->free_blocks);
            
            /* Create merged block */
            yq_frag_block *merged = frag_create_block(new_block->address, combined_size, 0);
            if (merged) {
                frag_insert_block(analyzer, merged, &analyzer->free_blocks);
            }
            
            /* Destroy original blocks */
            frag_destroy_block(new_block);
            frag_destroy_block(next);
        }
    }
}