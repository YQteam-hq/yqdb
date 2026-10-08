/*
 * yq_ttl.h — Time-To-Live functionality for yq-DB
 *
 * 版本 : 1.0.0
 * 语言 : C11
 *
 * 提供TTL功能，支持键的自动过期和清理
 */

#ifndef YQ_TTL_H
#define YQ_TTL_H

#include "yq.h"
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════
 * TTL配置
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct yq_ttl_opts {
    uint32_t struct_size;      /* 必须 = sizeof(yq_ttl_opts) */
    uint32_t cleanup_interval; /* 清理间隔（秒），0表示手动清理 */
    uint32_t max_expired;      /* 单次清理的最大过期键数量，0表示无限制 */
    uint32_t reserved[8];      /* 必须为 0 */
} yq_ttl_opts;

/* ═══════════════════════════════════════════════════════════════════════
 * TTL结果
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct yq_ttl_result {
    uint32_t struct_size;      /* 必须 = sizeof(yq_ttl_result) */
    int cleaned_count;         /* 清理的键数量 */
    int error_count;           /* 错误数量 */
    yq_slice *cleaned_keys;    /* 清理的键列表 */
    int *error_codes;         /* 错误代码列表 */
    uint32_t reserved[8];      /* 必须为 0 */
} yq_ttl_result;

/* ═══════════════════════════════════════════════════════════════════════
 * TTL操作
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * 设置TTL配置
 * 输入：db - 数据库句柄，opts - TTL配置
 * 返回：YQ_OK 成功，其他错误码见 ERRORS.md
 */
int yq_ttl_configure(yq_db *db, const yq_ttl_opts *opts);

/*
 * 获取TTL配置
 * 输入：db - 数据库句柄，opts - 输出配置
 * 返回：YQ_OK 成功，其他错误码见 ERRORS.md
 */
int yq_ttl_get_config(yq_db *db, yq_ttl_opts *opts);

/*
 * 设置键的TTL
 * 输入：txn - 事务句柄，key - 键，ttl - 过期时间（秒）
 * 返回：YQ_OK 成功，其他错误码见 ERRORS.md
 */
int yq_ttl_set(yq_txn *txn, const yq_slice *key, uint32_t ttl);

/*
 * 获取键的剩余TTL
 * 输入：txn - 事务句柄，key - 键
 * 返回：剩余TTL秒数，-1表示永不过期，-2表示键不存在
 */
int32_t yq_ttl_get(yq_txn *txn, const yq_slice *key);

/*
 * 取消键的TTL
 * 输入：txn - 事务句柄，key - 键
 * 返回：YQ_OK 成功，其他错误码见 ERRORS.md
 */
int yq_ttl_unset(yq_txn *txn, const yq_slice *key);

/*
 * 手动清理过期键
 * 输入：db - 数据库句柄，max_count - 最大清理数量，0表示无限制
 * 返回：清理结果，调用者负责释放
 */
int yq_ttl_cleanup(yq_db *db, uint32_t max_count, yq_ttl_result **result);

/*
 * 获取过期键统计信息
 * 输入：db - 数据库句柄
 * 返回：过期键数量，-1表示错误
 */
int64_t yq_ttl_expired_count(yq_db *db);

/*
 * 获取TTL内存使用量
 * 输入：db - 数据库句柄
 * 返回：内存使用量（字节），-1表示错误
 */
int64_t yq_ttl_memory_usage(yq_db *db);

/* ═══════════════════════════════════════════════════════════════════════
 * TTL查询
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * 查询即将过期的键
 * 输入：txn - 事务句柄，max_ttl - 最大剩余时间（秒），count - 返回的键数量
 * 返回：即将过期的键列表，调用者负责释放
 */
yq_slice **yq_ttl_find_expiring(yq_txn *txn, uint32_t max_ttl, int *count);

/*
 * 查询已过期的键
 * 输入：txn - 事务句柄，count - 返回的键数量
 * 返回：已过期的键列表，调用者负责释放
 */
yq_slice **yq_ttl_find_expired(yq_txn *txn, int *count);

/* ═══════════════════════════════════════════════════════════════════════
 * TTL结果清理
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * 释放TTL清理结果
 * 输入：result - TTL清理结果
 */
void yq_ttl_result_free(yq_ttl_result *result);

/*
 * 释放TTL查询结果
 * 输入：keys - 键列表，count - 键数量
 */
void yq_ttl_query_result_free(yq_slice **keys, int count);

/* ═══════════════════════════════════════════════════════════════════════
 * TTL工具函数
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * 获取当前时间戳（秒）
 * 返回：当前时间戳
 */
time_t yq_ttl_now(void);

/*
 * 时间戳转换
 * 输入：timestamp - 时间戳
 * 返回：可读的时间字符串，调用者负责释放
 */
char *yq_ttl_format_time(time_t timestamp);

/*
 * 解析时间字符串
 * 输入：time_str - 时间字符串
 * 返回：时间戳，-1表示错误
 */
time_t yq_ttl_parse_time(const char *time_str);

#ifdef __cplusplus
}
#endif

#endif /* YQ_TTL_H */