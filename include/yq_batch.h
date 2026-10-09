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

#ifndef YQ_BATCH_H
#define YQ_BATCH_H

#include "yq.h"
#include "yq_slice.h"
#include "yq_atomic.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Batch operation types */
typedef enum {
    YQ_BATCH_INSERT,
    YQ_BATCH_UPDATE,
    YQ_BATCH_DELETE,
    YQ_BATCH_UPSERT,
    YQ_BATCH_REPLACE
} yq_batch_op_type;

/* Batch operation structure */
typedef struct yq_batch_op {
    yq_batch_op_type type;          /* Operation type */
    yq_slice key;                   /* Key for the operation */
    yq_slice value;                 /* Value for the operation */
    uint32_t flags;                 /* Operation flags */
    int status;                     /* Operation status */
    uint64_t timestamp;            /* Operation timestamp */
    uint32_t retry_count;           /* Number of retries */
    uint64_t sequence_id;           /* Sequence number */
    struct yq_batch_op *next;       /* Next operation in batch */
    struct yq_batch_op *prev;       /* Previous operation in batch */
    volatile uint32_t generation;    /* Generation counter */
    volatile uint32_t ref_count;    /* Reference counter */
} yq_batch_op;

/* Batch structure */
typedef struct yq_batch {
    yq_batch_op *operations;       /* Head of operation list */
    yq_batch_op *tail;             /* Tail of operation list */
    size_t operation_count;         /* Number of operations */
    size_t total_key_size;          /* Total size of all keys */
    size_t total_value_size;        /* Total size of all values */
    uint64_t batch_id;              /* Unique batch identifier */
    uint64_t creation_time;        /* Batch creation timestamp */
    uint64_t execution_time;       /* Batch execution timestamp */
    uint32_t max_retries;          /* Maximum retry attempts */
    uint32_t timeout_ms;           /* Batch timeout in milliseconds */
    bool atomic;                    /* Whether batch should be atomic */
    bool ordered;                   /* Whether operations should be ordered */
    bool auto_commit;               /* Whether to auto-commit on success */
    bool auto_rollback;             /* Whether to auto-rollback on failure */
    volatile uint32_t generation;    /* Generation counter */
    volatile uint32_t ref_count;     /* Reference counter */
    yq_batch_stats *stats;          /* Batch execution statistics */
} yq_batch;

/* Batch statistics */
typedef struct yq_batch_stats {
    uint64_t total_operations;      /* Total operations processed */
    uint64_t successful_operations; /* Successful operations */
    uint64_t failed_operations;    /* Failed operations */
    uint64_t retry_operations;     /* Operations that were retried */
    uint64_t execution_time_us;     /* Total execution time in microseconds */
    uint64_t average_latency_us;    /* Average operation latency */
    uint64_t throughput_ops_sec;   /* Operations per second */
    size_t total_key_bytes;         /* Total key bytes processed */
    size_t total_value_bytes;       /* Total value bytes processed */
    double error_rate;              /* Error rate (0.0 to 1.0) */
    double success_rate;           /* Success rate (0.0 to 1.0) */
    uint32_t peak_memory_usage;    /* Peak memory usage in bytes */
    uint32_t average_memory_usage; /* Average memory usage in bytes */
} yq_batch_stats;

/* Batch result structure */
typedef struct yq_batch_result {
    int status;                     /* Overall batch status */
    uint64_t batch_id;             /* Batch identifier */
    uint64_t execution_time_us;    /* Execution time in microseconds */
    size_t operations_processed;   /* Number of operations processed */
    size_t operations_succeeded;   /* Number of successful operations */
    size_t operations_failed;       /* Number of failed operations */
    yq_batch_op **failed_ops;      /* Array of failed operations */
    size_t failed_op_count;         /* Number of failed operations */
    yq_batch_stats stats;          /* Batch execution statistics */
    char *error_message;           /* Error message if batch failed */
    size_t error_message_len;      /* Length of error message */
} yq_batch_result;

/* Batch configuration */
typedef struct yq_batch_config {
    size_t max_batch_size;         /* Maximum number of operations per batch */
    size_t max_key_size;           /* Maximum key size per operation */
    size_t max_value_size;         /* Maximum value size per operation */
    uint32_t max_retries;          /* Maximum retry attempts */
    uint32_t timeout_ms;           /* Batch timeout in milliseconds */
    bool atomic;                    /* Whether batches should be atomic */
    bool ordered;                   /* Whether operations should be ordered */
    bool auto_commit;               /* Whether to auto-commit on success */
    bool auto_rollback;             /* Whether to auto-rollback on failure */
    bool enable_stats;             /* Whether to collect statistics */
    bool enable_retry;             /* Whether to enable retry logic */
    bool enable_validation;        /* Whether to enable input validation */
    uint32_t thread_count;         /* Number of worker threads */
    uint32_t queue_size;           /* Operation queue size */
    uint64_t memory_limit;         /* Memory limit for batch operations */
} yq_batch_config;

/* Batch operation result callback */
typedef int (*yq_batch_callback)(yq_batch_op *op, int status, void *user_data);

/* Batch management functions */
yq_batch *yq_batch_create(const yq_batch_config *config);
void yq_batch_destroy(yq_batch *batch);
int yq_batch_ref(yq_batch *batch);
int yq_batch_unref(yq_batch *batch);

/* Batch operation functions */
int yq_batch_add(yq_batch *batch, yq_batch_op_type type, const yq_slice *key, const yq_slice *value, uint32_t flags);
int yq_batch_add_insert(yq_batch *batch, const yq_slice *key, const yq_slice *value, uint32_t flags);
int yq_batch_add_update(yq_batch *batch, const yq_slice *key, const yq_slice *value, uint32_t flags);
int yq_batch_add_delete(yq_batch *batch, const yq_slice *key, uint32_t flags);
int yq_batch_add_upsert(yq_batch *batch, const yq_slice *key, const yq_slice *value, uint32_t flags);
int yq_batch_add_replace(yq_batch *batch, const yq_slice *key, const yq_slice *value, uint32_t flags);

/* Batch execution functions */
int yq_batch_execute(yq_batch *batch, yq_batch_result *result);
int yq_batch_execute_async(yq_batch *batch, yq_batch_callback callback, void *user_data);
int yq_batch_rollback(yq_batch *batch);
int yq_batch_commit(yq_batch *batch);

/* Batch query functions */
int yq_batch_get(yq_batch *batch, const yq_slice *key, yq_slice *value);
int yq_batch_contains(yq_batch *batch, const yq_slice *key, bool *contains);
int yq_batch_get_stats(yq_batch *batch, yq_batch_stats *stats);

/* Batch utility functions */
int yq_batch_validate(yq_batch *batch);
int yq_batch_clear(yq_batch *batch);
int yq_batch_size(yq_batch *batch, size_t *size);
int yq_batch_memory_usage(yq_batch *batch, size_t *usage);
int yq_batch_get_failed_operations(yq_batch *batch, yq_batch_op ***failed_ops, size_t *count);

/* Batch configuration functions */
int yq_batch_set_config(yq_batch *batch, const yq_batch_config *config);
int yq_batch_get_config(yq_batch *batch, yq_batch_config *config);

/* Batch statistics functions */
int yq_batch_reset_stats(yq_batch *batch);
int yq_batch_enable_stats(yq_batch *batch, bool enabled);
int yq_batch_get_stats_summary(yq_batch *batch, yq_batch_stats *stats);

/* Batch error handling */
int yq_batch_get_error(yq_batch *batch, char *error_buf, size_t error_buf_len);
int yq_batch_clear_error(yq_batch *batch);

/* Batch optimization functions */
int yq_batch_optimize(yq_batch *batch);
int yq_batch_compact(yq_batch *batch);
int yq_batch_prune(yq_batch *batch, size_t max_age_ms);

/* Batch serialization functions */
int yq_batch_serialize(yq_batch *batch, char **buffer, size_t *buffer_len);
int yq_batch_deserialize(const char *buffer, size_t buffer_len, yq_batch **batch);

#ifdef __cplusplus
}
#endif

#endif /* YQ_BATCH_H */