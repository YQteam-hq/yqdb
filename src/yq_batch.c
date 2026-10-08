#include "yq_batch.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* 批量操作错误消息 */
static const char *g_batch_err_msgs[] = {
    "success",
    "batch operation error",
    "out of memory",
    "batch is empty",
    "batch operation conflict",
    "batch operation limit exceeded"
};

const char *yq_batch_strerror(int rc) {
    if (rc < 0 || rc > 5) return "unknown batch error";
    return g_batch_err_msgs[rc];
}

/* 创建批量操作结果 */
yq_batch_result *yq_batch_result_create(int capacity) {
    yq_batch_result *result = malloc(sizeof(yq_batch_result));
    if (!result) return NULL;
    
    result->struct_size = sizeof(yq_batch_result);
    result->success_count = 0;
    result->error_count = 0;
    result->error_indices = NULL;
    result->error_codes = NULL;
    
    if (capacity > 0) {
        result->error_indices = malloc(capacity * sizeof(int));
        result->error_codes = malloc(capacity * sizeof(int));
        if (!result->error_indices || !result->error_codes) {
            free(result->error_indices);
            free(result->error_codes);
            free(result);
            return NULL;
        }
    }
    
    return result;
}

/* 销毁批量操作结果 */
void yq_batch_result_destroy(yq_batch_result *result) {
    if (!result) return;
    
    free(result->error_indices);
    free(result->error_codes);
    free(result);
}

/* 执行批量操作 */
int yq_batch_execute(yq_txn *txn, const yq_batch_op *ops, size_t count, yq_batch_result **result) {
    if (!txn || !ops || count == 0 || !result) {
        return YQ_ERR_INVAL;
    }
    
    /* 创建结果结构体 */
    *result = yq_batch_result_create(count);
    if (!*result) {
        return YQ_ERR_NOMEM;
    }
    
    (*result)->success_count = 0;
    (*result)->error_count = 0;
    
    /* 执行每个操作 */
    for (size_t i = 0; i < count; i++) {
        int rc;
        
        switch (ops[i].type) {
            case YQ_BATCH_OP_PUT:
                rc = yq_put(txn, ops[i].key, ops[i].value, YQ_PUT_UPSERT);
                break;
                
            case YQ_BATCH_OP_DEL:
                rc = yq_del(txn, ops[i].key);
                break;
                
            case YQ_BATCH_OP_UPSERT:
                rc = yq_put(txn, ops[i].key, ops[i].value, YQ_PUT_NOOVERWRITE);
                break;
                
            default:
                rc = YQ_ERR_INVAL;
                break;
        }
        
        if (rc == YQ_OK) {
            (*result)->success_count++;
        } else {
            if ((*result)->error_count < count) {
                (*result)->error_indices[(*result)->error_count] = (int)i;
                (*result)->error_codes[(*result)->error_count] = rc;
            }
            (*result)->error_count++;
        }
    }
    
    return YQ_OK;
}

/* 执行批量插入 */
int yq_batch_insert(yq_txn *txn, const yq_batch_op *ops, size_t count, yq_batch_result **result) {
    if (!txn || !ops || count == 0 || !result) {
        return YQ_ERR_INVAL;
    }
    
    /* 创建结果结构体 */
    *result = yq_batch_result_create(count);
    if (!*result) {
        return YQ_ERR_NOMEM;
    }
    
    (*result)->success_count = 0;
    (*result)->error_count = 0;
    
    /* 执行插入操作 */
    for (size_t i = 0; i < count; i++) {
        int rc = yq_put(txn, ops[i].key, ops[i].value, YQ_PUT_NOOVERWRITE);
        
        if (rc == YQ_OK) {
            (*result)->success_count++;
        } else {
            if ((*result)->error_count < count) {
                (*result)->error_indices[(*result)->error_count] = (int)i;
                (*result)->error_codes[(*result)->error_count] = rc;
            }
            (*result)->error_count++;
        }
    }
    
    return YQ_OK;
}

/* 执行批量删除 */
int yq_batch_delete(yq_txn *txn, const yq_slice *keys, size_t count, yq_batch_result **result) {
    if (!txn || !keys || count == 0 || !result) {
        return YQ_ERR_INVAL;
    }
    
    /* 创建结果结构体 */
    *result = yq_batch_result_create(count);
    if (!*result) {
        return YQ_ERR_NOMEM;
    }
    
    (*result)->success_count = 0;
    (*result)->error_count = 0;
    
    /* 执行删除操作 */
    for (size_t i = 0; i < count; i++) {
        int rc = yq_del(txn, keys[i]);
        
        if (rc == YQ_OK) {
            (*result)->success_count++;
        } else {
            if ((*result)->error_count < count) {
                (*result)->error_indices[(*result)->error_count] = (int)i;
                (*result)->error_codes[(*result)->error_count] = rc;
            }
            (*result)->error_count++;
        }
    }
    
    return YQ_OK;
}

/* 批量获取值 */
int yq_batch_get(yq_txn *txn, const yq_slice *keys, size_t count, yq_slice *values) {
    if (!txn || !keys || !values || count == 0) {
        return YQ_ERR_INVAL;
    }
    
    for (size_t i = 0; i < count; i++) {
        int rc = yq_get(txn, keys[i], &values[i]);
        if (rc != YQ_OK) {
            /* 初始化未找到的值 */
            values[i].data = NULL;
            values[i].size = 0;
        }
    }
    
    return YQ_OK;
}

/* 批量检查键是否存在 */
int yq_batch_exists(yq_txn *txn, const yq_slice *keys, size_t count, int *exists) {
    if (!txn || !keys || !exists || count == 0) {
        return YQ_ERR_INVAL;
    }
    
    yq_slice value = { 0 };
    
    for (size_t i = 0; i < count; i++) {
        int rc = yq_get(txn, keys[i], &value);
        exists[i] = (rc == YQ_OK);
        
        /* 重置value以避免内存泄漏 */
        value.data = NULL;
        value.size = 0;
    }
    
    return YQ_OK;
}

/* 创建批量操作项 */
yq_batch_op yq_batch_op_create(yq_batch_op_type type, yq_slice key, yq_slice value) {
    yq_batch_op op;
    op.type = type;
    op.key = key;
    op.value = value;
    return op;
}

/* 创建批量操作项（删除操作） */
yq_batch_op yq_batch_op_create_delete(yq_slice key) {
    yq_batch_op op;
    op.type = YQ_BATCH_OP_DEL;
    op.key = key;
    op.value.data = NULL;
    op.value.size = 0;
    return op;
}

/* 创建批量操作项（插入操作） */
yq_batch_op yq_batch_op_create_insert(yq_slice key, yq_slice value) {
    yq_batch_op op;
    op.type = YQ_BATCH_OP_PUT;
    op.key = key;
    op.value = value;
    return op;
}

/* 创建批量操作项（更新操作） */
yq_batch_op yq_batch_op_create_upsert(yq_slice key, yq_slice value) {
    yq_batch_op op;
    op.type = YQ_BATCH_OP_UPSERT;
    op.key = key;
    op.value = value;
    return op;
}