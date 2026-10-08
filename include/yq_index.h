/*
 * yq_index.h — Secondary indexes for yq-DB
 *
 * 版本 : 1.0.0
 * 语言 : C11
 *
 * 提供二级索引功能，支持基于值的快速查找和范围查询
 */

#ifndef YQ_INDEX_H
#define YQ_INDEX_H

#include "yq.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════
 * 索引类型
 * ═══════════════════════════════════════════════════════════════════════ */

typedef enum yq_index_type {
    YQ_INDEX_STRING = 1,    /* 字符串索引 */
    YQ_INDEX_INT64 = 2,      /* 64位整数索引 */
    YQ_INDEX_DOUBLE = 3,     /* 双精度浮点数索引 */
    YQ_INDEX_BINARY = 4      /* 二进制数据索引 */
} yq_index_type;

/* ═══════════════════════════════════════════════════════════════════════
 * 索引配置
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct yq_index_opts {
    uint32_t struct_size;      /* 必须 = sizeof(yq_index_opts) */
    yq_index_type type;        /* 索引类型 */
    uint32_t flags;            /* 索引标志 */
    uint32_t reserved[8];      /* 必须为 0 */
} yq_index_opts;

/* ═══════════════════════════════════════════════════════════════════════
 * 索引操作结果
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct yq_index_result {
    uint32_t struct_size;      /* 必须 = sizeof(yq_index_result) */
    int count;                 /* 匹配的键数量 */
    yq_slice *keys;            /* 匹配的键列表 */
    int *error_code;           /* 错误代码列表（可选） */
    uint32_t reserved[8];      /* 必须为 0 */
} yq_index_result;

/* ═══════════════════════════════════════════════════════════════════════
 * 索引标志
 * ═══════════════════════════════════════════════════════════════════════ */

#define YQ_INDEX_UNIQUE       0x0001u  /* 唯一索引 */
#define YQ_INDEX_CASE_SENSITIVE 0x0002u  /* 字符串索引区分大小写 */
#define YQ_INDEX_DESCENDING    0x0004u  /* 降序排列 */
#define YQ_INDEX_NULLS_FIRST  0x0008u  /* NULL值优先 */

/* ═══════════════════════════════════════════════════════════════════════
 * 索引管理
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * 创建索引
 * 输入：db - 数据库句柄，name - 索引名称，opts - 索引配置
 * 返回：YQ_OK 成功，其他错误码见 ERRORS.md
 */
int yq_index_create(yq_db *db, const char *name, const yq_index_opts *opts);

/*
 * 删除索引
 * 输入：db - 数据库句柄，name - 索引名称
 * 返回：YQ_OK 成功，YQ_ERR_NOTFOUND 索引不存在
 */
int yq_index_drop(yq_db *db, const char *name);

/*
 * 列出所有索引
 * 输入：db - 数据库句柄，count - 返回的索引数量
 * 返回：索引名称数组，调用者负责释放
 */
char **yq_index_list(yq_db *db, int *count);

/*
 * 检查索引是否存在
 * 输入：db - 数据库句柄，name - 索引名称
 * 返回：1 存在，0 不存在
 */
int yq_index_exists(yq_db *db, const char *name);

/* ═══════════════════════════════════════════════════════════════════════
 * 索引查询
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * 精确查找
 * 输入：txn - 事务句柄，index_name - 索引名称，value - 查找值
 * 返回：匹配的键列表，调用者负责释放结果
 */
int yq_index_find_exact(yq_txn *txn, const char *index_name, const yq_slice *value, yq_index_result **result);

/*
 * 范围查找
 * 输入：txn - 事务句柄，index_name - 索引名称，start - 起始值，end - 结束值
 * 返回：范围内的键列表，调用者负责释放结果
 */
int yq_index_find_range(yq_txn *txn, const char *index_name, const yq_slice *start, const yq_slice *end, yq_index_result **result);

/*
 * 前缀查找（仅字符串索引）
 * 输入：txn - 事务句柄，index_name - 索引名称，prefix - 前缀
 * 返回：匹配前缀的键列表，调用者负责释放结果
 */
int yq_index_find_prefix(yq_txn *txn, const char *index_name, const yq_slice *prefix, yq_index_result **result);

/*
 * 大于查找
 * 输入：txn - 事务句柄，index_name - 索引名称，value - 比较值
 * 返回：大于value的键列表，调用者负责释放结果
 */
int yq_index_find_gt(yq_txn *txn, const char *index_name, const yq_slice *value, yq_index_result **result);

/*
 * 大于等于查找
 * 输入：txn - 事务句柄，index_name - 索引名称，value - 比较值
 * 返回：大于等于value的键列表，调用者负责释放结果
 */
int yq_index_find_gte(yq_txn *txn, const char *index_name, const yq_slice *value, yq_index_result **result);

/*
 * 小于查找
 * 输入：txn - 事务句柄，index_name - 索引名称，value - 比较值
 * 返回：小于value的键列表，调用者负责释放结果
 */
int yq_index_find_lt(yq_txn *txn, const char *index_name, const yq_slice *value, yq_index_result **result);

/*
 * 小于等于查找
 * 输入：txn - 事务句柄，index_name - 索引名称，value - 比较值
 * 返回：小于等于value的键列表，调用者负责释放结果
 */
int yq_index_find_lte(yq_txn *txn, const char *index_name, const yq_slice *value, yq_index_result **result);

/* ═══════════════════════════════════════════════════════════════════════
 * 索引统计信息
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * 获取索引统计信息
 * 输入：db - 数据库句柄，name - 索引名称
 * 返回：索引条目数量，-1 表示错误
 */
int64_t yq_index_size(yq_db *db, const char *name);

/*
 * 获取索引内存使用量
 * 输入：db - 数据库句柄，name - 索引名称
 * 返回：索引内存使用量（字节），-1 表示错误
 */
int64_t yq_index_memory_usage(yq_db *db, const char *name);

/* ═══════════════════════════════════════════════════════════════════════
 * 索引结果清理
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * 释放索引查询结果
 * 输入：result - 索引查询结果
 */
void yq_index_result_free(yq_index_result *result);

#ifdef __cplusplus
}
#endif

#endif /* YQ_INDEX_H */