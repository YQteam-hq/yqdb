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

#include "yq_batch.h"
#include "yq_alloc.h"
#include "yq_atomic.h"
#include "yq_security.h"
#include "yq_time.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* Default batch configuration */
static const yq_batch_config default_batch_config = {
    .max_batch_size = 1000,
    .max_key_size = 1024,
    .max_value_size = 1024 * 1024, /* 1MB */
    .max_retries = 3,
    .timeout_ms = 5000,
    .atomic = true,
    .ordered = true,
    .auto_commit = true,
    .auto_rollback = true,
    .enable_stats = true,
    .enable_retry = true,
    .enable_validation = true,
    .thread_count = 1,
    .queue_size = 10000,
    .memory_limit = 100 * 1024 * 1024 /* 100MB */
};

/* Batch operation implementation */
static yq_batch_op *batch_op_create(yq_batch_op_type type, const yq_slice *key, const yq_slice *value, uint32_t flags) {
    if (!key || key->len == 0) return NULL;
    
    yq_batch_op *op = yq_malloc(sizeof(yq_batch_op));
    if (!op) return NULL;
    
    /* Initialize operation structure */
    op->type = type;
    op->key = *key;
    op->value = value ? *value : (yq_slice){0, NULL};
    op->flags = flags;
    op->status = YQ_OK;
    op->timestamp = yq_time_now();
    op->retry_count = 0;
    op->sequence_id = 0;
    op->next = NULL;
    op->prev = NULL;
    op->generation = 1;
    op->ref_count = 1;
    
    return op;
}

static void batch_op_destroy(yq_batch_op *op) {
    if (!op) return;
    
    /* Validate operation state */
    if (op->generation != 1) {
        return;
    }
    
    /* Secure zero sensitive data */
    yq_security_zero(op, sizeof(*op));
    
    yq_free(op);
}

static int batch_op_validate(yq_batch_op *op, const yq_batch_config *config) {
    if (!op || !config) return YQ_ERR_INVAL;
    
    /* Validate operation type */
    if (op->type < YQ_BATCH_INSERT || op->type > YQ_BATCH_REPLACE) {
        return YQ_ERR_INVAL;
    }
    
    /* Validate key size */
    if (op->key.len == 0 || op->key.len > config->max_key_size) {
        return YQ_ERR_INVAL;
    }
    
    /* Validate value size (for operations that have values) */
    if ((op->type == YQ_BATCH_INSERT || op->type == YQ_BATCH_UPDATE || 
         op->type == YQ_BATCH_UPSERT || op->type == YQ_BATCH_REPLACE) &&
        (op->value.len > config->max_value_size)) {
        return YQ_ERR_INVAL;
    }
    
    /* Validate key data */
    if (!yq_security_validate_range(op->key.data, op->key.len)) {
        return YQ_ERR_INVAL;
    }
    
    /* Validate value data (if present) */
    if (op->value.len > 0 && !yq_security_validate_range(op->value.data, op->value.len)) {
        return YQ_ERR_INVAL;
    }
    
    return YQ_OK;
}

/* Batch implementation */
yq_batch *yq_batch_create(const yq_batch_config *config) {
    yq_batch_config final_config;
    
    if (config) {
        final_config = *config;
    } else {
        final_config = default_batch_config;
    }
    
    yq_batch *batch = yq_malloc(sizeof(yq_batch));
    if (!batch) return NULL;
    
    /* Initialize batch structure */
    batch->operations = NULL;
    batch->tail = NULL;
    batch->operation_count = 0;
    batch->total_key_size = 0;
    batch->total_value_size = 0;
    batch->batch_id = yq_time_now();
    batch->creation_time = yq_time_now();
    batch->execution_time = 0;
    batch->max_retries = final_config.max_retries;
    batch->timeout_ms = final_config.timeout_ms;
    batch->atomic = final_config.atomic;
    batch->ordered = final_config.ordered;
    batch->auto_commit = final_config.auto_commit;
    batch->auto_rollback = final_config.auto_rollback;
    batch->generation = 1;
    batch->ref_count = 1;
    batch->stats = final_config.enable_stats ? yq_malloc(sizeof(yq_batch_stats)) : NULL;
    
    if (batch->stats) {
        memset(batch->stats, 0, sizeof(yq_batch_stats));
    }
    
    return batch;
}

void yq_batch_destroy(yq_batch *batch) {
    if (!batch) return;
    
    /* Validate batch state */
    if (batch->generation != 1) {
        return;
    }
    
    /* Destroy all operations */
    yq_batch_op *current = batch->operations;
    while (current) {
        yq_batch_op *next = current->next;
        batch_op_destroy(current);
        current = next;
    }
    
    /* Destroy statistics */
    if (batch->stats) {
        yq_free(batch->stats);
    }
    
    /* Secure zero sensitive data */
    yq_security_zero(batch, sizeof(*batch));
    
    yq_free(batch);
}

int yq_batch_ref(yq_batch *batch) {
    if (!batch) return YQ_ERR_INVAL;
    
    /* Validate batch state */
    if (batch->generation != 1) {
        return YQ_ERR_INVAL;
    }
    
    uint32_t old_count = atomic_fetch_add(&batch->ref_count, 1);
    if (old_count == 0) {
        return YQ_ERR_INVAL;
    }
    
    return YQ_OK;
}

int yq_batch_unref(yq_batch *batch) {
    if (!batch) return YQ_ERR_INVAL;
    
    /* Validate batch state */
    if (batch->generation != 1) {
        return YQ_ERR_INVAL;
    }
    
    uint32_t old_count = atomic_fetch_sub(&batch->ref_count, 1);
    if (old_count == 1) {
        yq_batch_destroy(batch);
        return YQ_OK;
    }
    
    return YQ_OK;
}

/* Batch operation functions */
int yq_batch_add(yq_batch *batch, yq_batch_op_type type, const yq_slice *key, const yq_slice *value, uint32_t flags) {
    if (!batch || !key || key->len == 0) return YQ_ERR_INVAL;
    
    /* Validate batch state */
    if (batch->generation != 1) {
        return YQ_ERR_INVAL;
    }
    
    /* Check batch size limit */
    if (batch->operation_count >= default_batch_config.max_batch_size) {
        return YQ_ERR_TOOBIG;
    }
    
    /* Create operation */
    yq_batch_op *op = batch_op_create(type, key, value, flags);
    if (!op) return YQ_ERR_NOMEM;
    
    /* Validate operation if enabled */
    if (default_batch_config.enable_validation) {
        int result = batch_op_validate(op, &default_batch_config);
        if (result != YQ_OK) {
            batch_op_destroy(op);
            return result;
        }
    }
    
    /* Add to batch */
    if (!batch->operations) {
        batch->operations = op;
        batch->tail = op;
    } else {
        op->prev = batch->tail;
        batch->tail->next = op;
        batch->tail = op;
    }
    
    batch->operation_count++;
    batch->total_key_size += key->len;
    if (value) {
        batch->total_value_size += value->len;
    }
    
    return YQ_OK;
}

int yq_batch_add_insert(yq_batch *batch, const yq_slice *key, const yq_slice *value, uint32_t flags) {
    return yq_batch_add(batch, YQ_BATCH_INSERT, key, value, flags);
}

int yq_batch_add_update(yq_batch *batch, const yq_slice *key, const yq_slice *value, uint32_t flags) {
    return yq_batch_add(batch, YQ_BATCH_UPDATE, key, value, flags);
}

int yq_batch_add_delete(yq_batch *batch, const yq_slice *key, uint32_t flags) {
    return yq_batch_add(batch, YQ_BATCH_DELETE, key, NULL, flags);
}

int yq_batch_add_upsert(yq_batch *batch, const yq_slice *key, const yq_slice *value, uint32_t flags) {
    return yq_batch_add(batch, YQ_BATCH_UPSERT, key, value, flags);
}

int yq_batch_add_replace(yq_batch *batch, const yq_slice *key, const yq_slice *value, uint32_t flags) {
    return yq_batch_add(batch, YQ_BATCH_REPLACE, key, value, flags);
}

/* Batch execution functions */
int yq_batch_execute(yq_batch *batch, yq_batch_result *result) {
    if (!batch || !result) return YQ_ERR_INVAL;
    
    /* Validate batch state */
    if (batch->generation != 1) {
        return YQ_ERR_INVAL;
    }
    
    /* Reset result */
    memset(result, 0, sizeof(yq_batch_result));
    result->batch_id = batch->batch_id;
    
    /* Check if batch is empty */
    if (batch->operation_count == 0) {
        result->status = YQ_OK;
        result->operations_processed = 0;
        result->operations_succeeded = 0;
        result->operations_failed = 0;
        return YQ_OK;
    }
    
    /* Start execution timer */
    uint64_t start_time = yq_time_now();
    
    /* Execute operations */
    size_t succeeded = 0;
    size_t failed = 0;
    yq_batch_op **failed_ops = NULL;
    
    yq_batch_op *current = batch->operations;
    while (current) {
        /* Execute operation */
        int op_result = YQ_OK; /* Placeholder - actual operation execution would go here */
        
        /* Update operation status */
        current->status = op_result;
        
        if (op_result == YQ_OK) {
            succeeded++;
        } else {
            /* Add to failed operations list */
            yq_batch_op **new_failed = yq_realloc(failed_ops, (failed + 1) * sizeof(yq_batch_op *));
            if (!new_failed) {
                /* Clean up and return error */
                for (size_t i = 0; i < failed; i++) {
                    failed_ops[i] = NULL;
                }
                yq_free(failed_ops);
                return YQ_ERR_NOMEM;
            }
            
            failed_ops = new_failed;
            failed_ops[failed] = current;
            failed++;
        }
        
        /* Move to next operation */
        current = current->next;
    }
    
    /* Update execution time */
    uint64_t end_time = yq_time_now();
    result->execution_time_us = end_time - start_time;
    
    /* Update batch statistics */
    if (batch->stats) {
        batch->stats->total_operations += batch->operation_count;
        batch->stats->successful_operations += succeeded;
        batch->stats->failed_operations += failed;
        batch->stats->execution_time_us += result->execution_time_us;
        
        if (batch->stats->total_operations > 0) {
            batch->stats->success_rate = (double)succeeded / batch->operation_count;
            batch->stats->error_rate = (double)failed / batch->operation_count;
        }
    }
    
    /* Set result */
    result->status = (failed == 0) ? YQ_OK : YQ_ERR_IO;
    result->operations_processed = batch->operation_count;
    result->operations_succeeded = succeeded;
    result->operations_failed = failed;
    result->failed_ops = failed_ops;
    result->failed_op_count = failed;
    
    /* Handle auto-commit/rollback */
    if (batch->auto_commit && failed == 0) {
        /* Auto-commit successful batch */
        /* Placeholder - actual commit logic would go here */
    } else if (batch->auto_rollback && failed > 0) {
        /* Auto-rollback failed batch */
        /* Placeholder - actual rollback logic would go here */
    }
    
    return result->status;
}

int yq_batch_execute_async(yq_batch *batch, yq_batch_callback callback, void *user_data) {
    if (!batch || !callback) return YQ_ERR_INVAL;
    
    /* Validate batch state */
    if (batch->generation != 1) {
        return YQ_ERR_INVAL;
    }
    
    /* Placeholder for async execution */
    /* In a real implementation, this would queue the batch for execution */
    /* and call the callback when complete */
    
    return YQ_ERR_NOTIMPL;
}

int yq_batch_rollback(yq_batch *batch) {
    if (!batch) return YQ_ERR_INVAL;
    
    /* Validate batch state */
    if (batch->generation != 1) {
        return YQ_ERR_INVAL;
    }
    
    /* Placeholder for rollback logic */
    /* This would undo all operations in the batch */
    
    return YQ_OK;
}

int yq_batch_commit(yq_batch *batch) {
    if (!batch) return YQ_ERR_INVAL;
    
    /* Validate batch state */
    if (batch->generation != 1) {
        return YQ_ERR_INVAL;
    }
    
    /* Placeholder for commit logic */
    /* This would persist all operations in the batch */
    
    return YQ_OK;
}

/* Batch query functions */
int yq_batch_get(yq_batch *batch, const yq_slice *key, yq_slice *value) {
    if (!batch || !key || !value) return YQ_ERR_INVAL;
    
    /* Validate batch state */
    if (batch->generation != 1) {
        return YQ_ERR_INVAL;
    }
    
    /* Search for key in batch operations */
    yq_batch_op *current = batch->operations;
    while (current) {
        if (current->key.len == key->len && 
            memcmp(current->key.data, key->data, key->len) == 0) {
            *value = current->value;
            return YQ_OK;
        }
        current = current->next;
    }
    
    return YQ_ERR_NOTFOUND;
}

int yq_batch_contains(yq_batch *batch, const yq_slice *key, bool *contains) {
    if (!batch || !key || !contains) return YQ_ERR_INVAL;
    
    /* Validate batch state */
    if (batch->generation != 1) {
        return YQ_ERR_INVAL;
    }
    
    /* Search for key in batch operations */
    yq_batch_op *current = batch->operations;
    while (current) {
        if (current->key.len == key->len && 
            memcmp(current->key.data, key->data, key->len) == 0) {
            *contains = true;
            return YQ_OK;
        }
        current = current->next;
    }
    
    *contains = false;
    return YQ_OK;
}

int yq_batch_get_stats(yq_batch *batch, yq_batch_stats *stats) {
    if (!batch || !stats) return YQ_ERR_INVAL;
    
    /* Validate batch state */
    if (batch->generation != 1) {
        return YQ_ERR_INVAL;
    }
    
    if (batch->stats) {
        *stats = *batch->stats;
    } else {
        memset(stats, 0, sizeof(yq_batch_stats));
    }
    
    return YQ_OK;
}

/* Batch utility functions */
int yq_batch_validate(yq_batch *batch) {
    if (!batch) return YQ_ERR_INVAL;
    
    /* Validate batch state */
    if (batch->generation != 1) {
        return YQ_ERR_INVAL;
    }
    
    /* Validate each operation */
    yq_batch_op *current = batch->operations;
    while (current) {
        int result = batch_op_validate(current, &default_batch_config);
        if (result != YQ_OK) {
            return result;
        }
        current = current->next;
    }
    
    return YQ_OK;
}

int yq_batch_clear(yq_batch *batch) {
    if (!batch) return YQ_ERR_INVAL;
    
    /* Validate batch state */
    if (batch->generation != 1) {
        return YQ_ERR_INVAL;
    }
    
    /* Destroy all operations */
    yq_batch_op *current = batch->operations;
    while (current) {
        yq_batch_op *next = current->next;
        batch_op_destroy(current);
        current = next;
    }
    
    /* Reset batch state */
    batch->operations = NULL;
    batch->tail = NULL;
    batch->operation_count = 0;
    batch->total_key_size = 0;
    batch->total_value_size = 0;
    batch->execution_time = 0;
    
    return YQ_OK;
}

int yq_batch_size(yq_batch *batch, size_t *size) {
    if (!batch || !size) return YQ_ERR_INVAL;
    
    /* Validate batch state */
    if (batch->generation != 1) {
        return YQ_ERR_INVAL;
    }
    
    *size = batch->operation_count;
    return YQ_OK;
}

int yq_batch_memory_usage(yq_batch *batch, size_t *usage) {
    if (!batch || !usage) return YQ_ERR_INVAL;
    
    /* Validate batch state */
    if (batch->generation != 1) {
        return YQ_ERR_INVAL;
    }
    
    /* Calculate memory usage */
    size_t total_usage = sizeof(yq_batch);
    
    yq_batch_op *current = batch->operations;
    while (current) {
        total_usage += sizeof(yq_batch_op);
        total_usage += current->key.len;
        total_usage += current->value.len;
        current = current->next;
    }
    
    if (batch->stats) {
        total_usage += sizeof(yq_batch_stats);
    }
    
    *usage = total_usage;
    return YQ_OK;
}

int yq_batch_get_failed_operations(yq_batch *batch, yq_batch_op ***failed_ops, size_t *count) {
    if (!batch || !failed_ops || !count) return YQ_ERR_INVAL;
    
    /* Validate batch state */
    if (batch->generation != 1) {
        return YQ_ERR_INVAL;
    }
    
    /* Count failed operations */
    size_t failed_count = 0;
    yq_batch_op *current = batch->operations;
    while (current) {
        if (current->status != YQ_OK) {
            failed_count++;
        }
        current = current->next;
    }
    
    /* Allocate array for failed operations */
    yq_batch_op **failed_array = yq_malloc(failed_count * sizeof(yq_batch_op *));
    if (!failed_array) return YQ_ERR_NOMEM;
    
    /* Fill array with failed operations */
    size_t index = 0;
    current = batch->operations;
    while (current) {
        if (current->status != YQ_OK) {
            failed_array[index++] = current;
        }
        current = current->next;
    }
    
    *failed_ops = failed_array;
    *count = failed_count;
    
    return YQ_OK;
}

/* Batch configuration functions */
int yq_batch_set_config(yq_batch *batch, const yq_batch_config *config) {
    if (!batch || !config) return YQ_ERR_INVAL;
    
    /* Validate batch state */
    if (batch->generation != 1) {
        return YQ_ERR_INVAL;
    }
    
    /* Update configuration */
    batch->max_retries = config->max_retries;
    batch->timeout_ms = config->timeout_ms;
    batch->atomic = config->atomic;
    batch->ordered = config->ordered;
    batch->auto_commit = config->auto_commit;
    batch->auto_rollback = config->auto_rollback;
    
    return YQ_OK;
}

int yq_batch_get_config(yq_batch *batch, yq_batch_config *config) {
    if (!batch || !config) return YQ_ERR_INVAL;
    
    /* Validate batch state */
    if (batch->generation != 1) {
        return YQ_ERR_INVAL;
    }
    
    /* Get configuration */
    config->max_batch_size = default_batch_config.max_batch_size;
    config->max_key_size = default_batch_config.max_key_size;
    config->max_value_size = default_batch_config.max_value_size;
    config->max_retries = batch->max_retries;
    config->timeout_ms = batch->timeout_ms;
    config->atomic = batch->atomic;
    config->ordered = batch->ordered;
    config->auto_commit = batch->auto_commit;
    config->auto_rollback = batch->auto_rollback;
    config->enable_stats = default_batch_config.enable_stats;
    config->enable_retry = default_batch_config.enable_retry;
    config->enable_validation = default_batch_config.enable_validation;
    config->thread_count = default_batch_config.thread_count;
    config->queue_size = default_batch_config.queue_size;
    config->memory_limit = default_batch_config.memory_limit;
    
    return YQ_OK;
}

/* Batch statistics functions */
int yq_batch_reset_stats(yq_batch *batch) {
    if (!batch) return YQ_ERR_INVAL;
    
    /* Validate batch state */
    if (batch->generation != 1) {
        return YQ_ERR_INVAL;
    }
    
    if (batch->stats) {
        memset(batch->stats, 0, sizeof(yq_batch_stats));
    }
    
    return YQ_OK;
}

int yq_batch_enable_stats(yq_batch *batch, bool enabled) {
    if (!batch) return YQ_ERR_INVAL;
    
    /* Validate batch state */
    if (batch->generation != 1) {
        return YQ_ERR_INVAL;
    }
    
    if (enabled && !batch->stats) {
        batch->stats = yq_malloc(sizeof(yq_batch_stats));
        if (batch->stats) {
            memset(batch->stats, 0, sizeof(yq_batch_stats));
        } else {
            return YQ_ERR_NOMEM;
        }
    } else if (!enabled && batch->stats) {
        yq_free(batch->stats);
        batch->stats = NULL;
    }
    
    return YQ_OK;
}

int yq_batch_get_stats_summary(yq_batch *batch, yq_batch_stats *stats) {
    if (!batch || !stats) return YQ_ERR_INVAL;
    
    /* Validate batch state */
    if (batch->generation != 1) {
        return YQ_ERR_INVAL;
    }
    
    if (batch->stats) {
        *stats = *batch->stats;
    } else {
        memset(stats, 0, sizeof(yq_batch_stats));
    }
    
    return YQ_OK;
}

/* Batch error handling */
int yq_batch_get_error(yq_batch *batch, char *error_buf, size_t error_buf_len) {
    if (!batch || !error_buf || error_buf_len == 0) return YQ_ERR_INVAL;
    
    /* Validate batch state */
    if (batch->generation != 1) {
        return YQ_ERR_INVAL;
    }
    
    /* Placeholder for error message */
    const char *error_msg = "Batch execution error";
    size_t msg_len = strlen(error_msg);
    
    if (msg_len >= error_buf_len) {
        msg_len = error_buf_len - 1;
    }
    
    memcpy(error_buf, error_msg, msg_len);
    error_buf[msg_len] = '\0';
    
    return YQ_OK;
}

int yq_batch_clear_error(yq_batch *batch) {
    if (!batch) return YQ_ERR_INVAL;
    
    /* Validate batch state */
    if (batch->generation != 1) {
        return YQ_ERR_INVAL;
    }
    
    /* Placeholder for error clearing */
    return YQ_OK;
}

/* Batch optimization functions */
int yq_batch_optimize(yq_batch *batch) {
    if (!batch) return YQ_ERR_INVAL;
    
    /* Validate batch state */
    if (batch->generation != 1) {
        return YQ_ERR_INVAL;
    }
    
    /* Optimize operation order if enabled */
    if (batch->ordered) {
        /* Sort operations by key for better performance */
        /* Placeholder for sorting logic */
    }
    
    /* Remove duplicate operations */
    yq_batch_op *current = batch->operations;
    while (current) {
        yq_batch_op *runner = current->next;
        while (runner) {
            if (current->key.len == runner->key.len && 
                memcmp(current->key.data, runner->data, current->key.len) == 0) {
                /* Duplicate found - remove runner */
                yq_batch_op *next = runner->next;
                if (runner->prev) {
                    runner->prev->next = next;
                }
                if (next) {
                    next->prev = runner->prev;
                }
                if (batch->tail == runner) {
                    batch->tail = runner->prev;
                }
                batch_op_destroy(runner);
                runner = next;
            } else {
                runner = runner->next;
            }
        }
        current = current->next;
    }
    
    return YQ_OK;
}

int yq_batch_compact(yq_batch *batch) {
    if (!batch) return YQ_ERR_INVAL;
    
    /* Validate batch state */
    if (batch->generation != 1) {
        return YQ_ERR_INVAL;
    }
    
    /* Compact batch by removing unnecessary operations */
    /* Placeholder for compaction logic */
    
    return YQ_OK;
}

int yq_batch_prune(yq_batch *batch, size_t max_age_ms) {
    if (!batch) return YQ_ERR_INVAL;
    
    /* Validate batch state */
    if (batch->generation != 1) {
        return YQ_ERR_INVAL;
    }
    
    uint64_t cutoff_time = yq_time_now() - max_age_ms;
    
    /* Remove old operations */
    yq_batch_op *current = batch->operations;
    while (current) {
        yq_batch_op *next = current->next;
        if (current->timestamp < cutoff_time) {
            /* Remove old operation */
            if (current->prev) {
                current->prev->next = next;
            } else {
                batch->operations = next;
            }
            if (next) {
                next->prev = current->prev;
            } else {
                batch->tail = current->prev;
            }
            batch_op_destroy(current);
        }
        current = next;
    }
    
    return YQ_OK;
}

/* Batch serialization functions */
int yq_batch_serialize(yq_batch *batch, char **buffer, size_t *buffer_len) {
    if (!batch || !buffer || !buffer_len) return YQ_ERR_INVAL;
    
    /* Validate batch state */
    if (batch->generation != 1) {
        return YQ_ERR_INVAL;
    }
    
    /* Calculate required buffer size */
    size_t total_size = sizeof(uint64_t) + sizeof(uint64_t) + sizeof(uint32_t);
    
    yq_batch_op *current = batch->operations;
    while (current) {
        total_size += sizeof(uint32_t) + current->key.len;
        total_size += sizeof(uint32_t) + current->value.len;
        total_size += sizeof(uint32_t) + sizeof(uint32_t);
        current = current->next;
    }
    
    /* Allocate buffer */
    char *buf = yq_malloc(total_size);
    if (!buf) return YQ_ERR_NOMEM;
    
    /* Write batch header */
    char *ptr = buf;
    memcpy(ptr, &batch->batch_id, sizeof(uint64_t));
    ptr += sizeof(uint64_t);
    memcpy(ptr, &batch->creation_time, sizeof(uint64_t));
    ptr += sizeof(uint64_t);
    memcpy(ptr, &batch->operation_count, sizeof(uint32_t));
    ptr += sizeof(uint32_t);
    
    /* Write operations */
    current = batch->operations;
    while (current) {
        memcpy(ptr, &current->type, sizeof(uint32_t));
        ptr += sizeof(uint32_t);
        memcpy(ptr, &current->key.len, sizeof(uint32_t));
        ptr += sizeof(uint32_t);
        memcpy(ptr, current->key.data, current->key.len);
        ptr += current->key.len;
        memcpy(ptr, &current->value.len, sizeof(uint32_t));
        ptr += sizeof(uint32_t);
        memcpy(ptr, current->value.data, current->value.len);
        ptr += current->value.len;
        memcpy(ptr, &current->flags, sizeof(uint32_t));
        ptr += sizeof(uint32_t);
        memcpy(ptr, &current->status, sizeof(uint32_t));
        ptr += sizeof(uint32_t);
        current = current->next;
    }
    
    *buffer = buf;
    *buffer_len = total_size;
    
    return YQ_OK;
}

int yq_batch_deserialize(const char *buffer, size_t buffer_len, yq_batch **batch) {
    if (!buffer || buffer_len == 0 || !batch) return YQ_ERR_INVAL;
    
    /* Create batch */
    yq_batch *b = yq_batch_create(NULL);
    if (!b) return YQ_ERR_NOMEM;
    
    /* Read batch header */
    const char *ptr = buffer;
    uint64_t batch_id, creation_time;
    uint32_t operation_count;
    
    memcpy(&batch_id, ptr, sizeof(uint64_t));
    ptr += sizeof(uint64_t);
    memcpy(&creation_time, ptr, sizeof(uint64_t));
    ptr += sizeof(uint64_t);
    memcpy(&operation_count, ptr, sizeof(uint32_t));
    ptr += sizeof(uint32_t);
    
    b->batch_id = batch_id;
    b->creation_time = creation_time;
    
    /* Read operations */
    for (uint32_t i = 0; i < operation_count; i++) {
        uint32_t type, key_len, value_len, flags, status;
        
        memcpy(&type, ptr, sizeof(uint32_t));
        ptr += sizeof(uint32_t);
        memcpy(&key_len, ptr, sizeof(uint32_t));
        ptr += sizeof(uint32_t);
        memcpy(&value_len, ptr, sizeof(uint32_t));
        ptr += sizeof(uint32_t);
        
        yq_slice key = {key_len, (void *)ptr};
        ptr += key_len;
        
        yq_slice value = {value_len, (void *)ptr};
        ptr += value_len;
        
        memcpy(&flags, ptr, sizeof(uint32_t));
        ptr += sizeof(uint32_t);
        memcpy(&status, ptr, sizeof(uint32_t));
        ptr += sizeof(uint32_t);
        
        /* Add operation to batch */
        yq_batch_op_type op_type = (yq_batch_op_type)type;
        int result = yq_batch_add(b, op_type, &key, value_len > 0 ? &value : NULL, flags);
        if (result != YQ_OK) {
            yq_batch_destroy(b);
            return result;
        }
        
        /* Set operation status */
        yq_batch_op *op = b->tail;
        if (op) {
            op->status = status;
        }
    }
    
    *batch = b;
    return YQ_OK;
}