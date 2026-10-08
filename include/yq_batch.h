/*
 * yq_batch.h — Batch operations for yq-DB
 *
 * 版本 : 1.0.0
 * 语言 : C11
 *
 * 提供批量操作功能，支持批量插入、更新、删除操作
 */

#ifndef YQ_BATCH_H
#define YQ_BATCH_H

#include "yq.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════
 * 批量操作错误码
 * ═══════════════════════════════════════════════════════════════════════ */

typedef enum yq_batch_rc {
    YQ_BATCH_OK        = 0,  /* 成功 */
    YQ_BATCH_ERR       = 1,  /* 批量操作错误 */
    YQ_BATCH_NOMEM     = 2,  /* 内存分配失败 */
    YQ_BATCH_EMPTY     = 3,  /* 批量为空 */
    YQ_BATCH_CONFLICT  = 4,  /* 批量操作冲突 */
    YQ_BATCH_LIMIT     = 5   /* 超过批量操作限制 */
} yq_batch_rc;

/*
 * 返回批量操作错误码的静态描述字符串。返回值生命周期为整个进程，调用方不得释放。
 */
const char *yq_batch_strerror(int rc);

/* ═══════════════════════════════════════════════════════════════════════
 * 批量操作类型
 * ═══════════════════════════════════════════════════════════════════════ */

typedef enum yq_batch_op_type {
    YQ_BATCH_OP_PUT   = 0,  /* 插入或更新 */
    YQ_BATCH_OP_DEL   = 1,  /* 删除 */
    YQ_BATCH_OP_UPSERT = 2  /* 插入（不存在时） */
} yq_batch_op_type;

/*
 * 批量操作项
 */
typedef struct yq_batch_op {
    yq_batch_op_type type;     /* 操作类型 */
    yq_slice key;             /* 键 */
    yq_slice value;          /* 值（删除操作时忽略） */
} yq_batch_op;

/*
 * 批量操作结果
 */
typedef struct yq_batch_result {
    uint32_t struct_size;     /* 必须 = sizeof(yq_batch_result) */
    int success_count;        /* 成功操作数量 */
    int error_count;          /* 失败操作数量 */
    int *error_indices;       /* 失败操作的索引数组（可为NULL） */
    int *error_codes;         /* 失败操作的错误码数组（可为NULL） */
} yq_batch_result;

/* ═══════════════════════════════════════════════════════════════════════
 * 批量操作API
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * 创建批量操作结果结构体。
 * 输入：capacity - 预期的错误数量
 * 返回：新创建的批量操作结果结构体
 */
yq_batch_result *yq_batch_result_create(int capacity);

/*
 * 销毁批量操作结果结构体。
 */
void yq_batch_result_destroy(yq_batch_result *result);

/*
 * 执行批量操作。
 * 输入：txn - 事务，ops - 操作数组，count - 操作数量，result - 结果输出
 * 返回：yq_rc错误码
 */
int yq_batch_execute(yq_txn *txn, const yq_batch_op *ops, size_t count, yq_batch_result **result);

/*
 * 执行批量插入（存在则跳过）。
 * 输入：txn - 事务，ops - 操作数组，count - 操作数量，result - 结果输出
 * 返回：yq_rc错误码
 */
int yq_batch_insert(yq_txn *txn, const yq_batch_op *ops, size_t count, yq_batch_result **result);

/*
 * 执行批量删除。
 * 输入：txn - 事务，keys - 键数组，count - 键数量，result - 结果输出
 * 返回：yq_rc错误码
 */
int yq_batch_delete(yq_txn *txn, const yq_slice *keys, size_t count, yq_batch_result **result);

/* ═══════════════════════════════════════════════════════════════════════
 * 批量查询操作
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * 批量获取值。
 * 输入：txn - 事务，keys - 键数组，count - 键数量，values - 值数组输出
 * 返回：yq_rc错误码
 */
int yq_batch_get(yq_txn *txn, const yq_slice *keys, size_t count, yq_slice *values);

/*
 * 批量检查键是否存在。
 * 输入：txn - 事务，keys - 键数组，count - 键数量，exists - 存在性数组输出
 * 返回：yq_rc错误码
 */
int yq_batch_exists(yq_txn *txn, const yq_slice *keys, size_t count, int *exists);

/* ═══════════════════════════════════════════════════════════════════════
 * 批量辅助函数
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * 创建批量操作项。
 * 输入：type - 操作类型，key - 键，value - 值
 * 返回：新的批量操作项
 */
yq_batch_op yq_batch_op_create(yq_batch_op_type type, yq_slice key, yq_slice value);

/*
 * 创建批量操作项（删除操作）。
 * 输入：key - 键
 * 返回：新的批量操作项
 */
yq_batch_op yq_batch_op_create_delete(yq_slice key);

/*
 * 创建批量操作项（插入操作）。
 * 输入：key - 键，value - 值
 * 返回：新的批量操作项
 */
yq_batch_op yq_batch_op_create_insert(yq_slice key, yq_slice value);

/*
 * 创建批量操作项（更新操作）。
 * 输入：key - 键，value - 值
 * 返回：新的批量操作项
 */
yq_batch_op yq_batch_op_create_upsert(yq_slice key, yq_slice value);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* YQ_BATCH_H */