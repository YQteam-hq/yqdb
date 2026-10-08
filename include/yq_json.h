/*
 * yq_json.h — JSON support for yq-DB
 *
 * 版本 : 1.0.0
 * 语言 : C11
 *
 * 提供JSON编码和解码功能，支持将JSON数据存储为yq-DB值
 */

#ifndef YQ_JSON_H
#define YQ_JSON_H

#include "yq.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════
 * JSON错误码
 * ═══════════════════════════════════════════════════════════════════════ */

typedef enum yq_json_rc {
    YQ_JSON_OK         = 0,  /* 成功 */
    YQ_JSON_ERR        = 1,  /* JSON解析错误 */
    YQ_JSON_NOMEM      = 2,  /* 内存分配失败 */
    YQ_JSON_DEPTH      = 3,  /* JSON嵌套深度超限 */
    YQ_JSON_INVALID    = 4,  /* 无效的JSON格式 */
    YQ_JSON_TRUNCATED  = 5,  /* JSON数据被截断 */
    YQ_JSON_UNSUPPORTED = 6  /* 不支持的JSON特性 */
} yq_json_rc;

/*
 * 返回JSON错误码的静态描述字符串。返回值生命周期为整个进程，调用方不得释放。
 */
const char *yq_json_strerror(int rc);

/* ═══════════════════════════════════════════════════════════════════════
 * JSON值类型
 * ═══════════════════════════════════════════════════════════════════════ */

typedef enum yq_json_type {
    YQ_JSON_NULL       = 0,  /* null */
    YQ_JSON_BOOL       = 1,  /* boolean */
    YQ_JSON_NUMBER     = 2,  /* number */
    YQ_JSON_STRING     = 3,  /* string */
    YQ_JSON_ARRAY      = 4,  /* array */
    YQ_JSON_OBJECT     = 5   /* object */
} yq_json_type;

/*
 * JSON值。用于表示解析后的JSON数据。
 * 注意：此结构体中的指针仅在yq_json_value_destroy前有效。
 */
typedef struct yq_json_value {
    yq_json_type type;
    union {
        int boolean;                                  /* 布尔值 */
        double number;                                /* 数值 */
        struct {
            char *data;                               /* 字符串数据 */
            size_t length;                             /* 字符串长度 */
        } string;
        struct {
            struct yq_json_value *elements;          /* 数组元素 */
            size_t count;                              /* 数组元素数量 */
        } array;
        struct {
            struct {
                char *key;                             /* 对象键 */
                struct yq_json_value *value;           /* 对象值 */
            } *members;
            size_t count;                              /* 对象成员数量 */
        } object;
    } data;
} yq_json_value;

/* ═══════════════════════════════════════════════════════════════════════
 * JSON解析和生成
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * 从JSON字符串解析JSON值。
 * 输入：json_str - JSON字符串，length - 字符串长度（0表示自动计算）
 * 输出：out - 解析后的JSON值
 * 返回：yq_json_rc错误码
 */
int yq_json_parse(const char *json_str, size_t length, yq_json_value **out);

/*
 * 生成JSON字符串。
 * 输入：value - JSON值，pretty - 是否美化输出
 * 输出：out - 生成的JSON字符串（调用者负责释放）
 * 返回：yq_json_rc错误码
 */
int yq_json_generate(const yq_json_value *value, int pretty, char **out);

/*
 * 释放JSON值及其所有子值。
 */
void yq_json_value_destroy(yq_json_value *value);

/* ═══════════════════════════════════════════════════════════════════════
 * yq-DB集成函数
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * 将JSON值存储为yq-DB值。
 * 输入：txn - 事务，key - 键，json_value - JSON值
 * 返回：yq_rc错误码
 */
int yq_json_put(yq_txn *txn, yq_slice key, const yq_json_value *json_value);

/*
 * 从yq-DB读取并解析JSON值。
 * 输入：txn - 事务，key - 键，out - 解析后的JSON值
 * 返回：yq_rc错误码
 */
int yq_json_get(yq_txn *txn, yq_slice key, yq_json_value **out);

/*
 * 将JSON字符串直接存储到yq-DB。
 * 输入：txn - 事务，key - 键，json_str - JSON字符串
 * 返回：yq_rc错误码
 */
int yq_json_put_raw(yq_txn *txn, yq_slice key, const char *json_str);

/*
 * 从yq-DB读取原始JSON字符串。
 * 输入：txn - 事务，key - 键，out - JSON字符串（借用指针）
 * 返回：yq_rc错误码
 */
int yq_json_get_raw(yq_txn *txn, yq_slice key, yq_slice *out);

/* ═══════════════════════════════════════════════════════════════════════
 * JSON辅助函数
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * 创建JSON null值。
 */
yq_json_value *yq_json_create_null(void);

/*
 * 创建JSON布尔值。
 */
yq_json_value *yq_json_create_bool(int boolean);

/*
 * 创建JSON数值。
 */
yq_json_value *yq_json_create_number(double number);

/*
 * 创建JSON字符串。
 * 输入：str - 字符串，length - 长度（0表示自动计算）
 * 返回：新创建的JSON字符串值
 */
yq_json_value *yq_json_create_string(const char *str, size_t length);

/*
 * 创建JSON数组。
 * 返回：新创建的JSON数组值
 */
yq_json_value *yq_json_create_array(void);

/*
 * 创建JSON对象。
 * 返回：新创建的JSON对象值
 */
yq_json_value *yq_json_create_object(void);

/*
 * 向JSON数组添加元素。
 * 返回：yq_json_rc错误码
 */
int yq_json_array_add(yq_json_value *array, yq_json_value *value);

/*
 * 向JSON对象添加成员。
 * 输入：key - 键，value - 值
 * 返回：yq_json_rc错误码
 */
int yq_json_object_add(yq_json_value *object, const char *key, yq_json_value *value);

/*
 * 获取JSON值的字符串表示。
 * 返回：类型字符串（"null", "bool", "number", "string", "array", "object"）
 */
const char *yq_json_type_to_string(yq_json_type type);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* YQ_JSON_H */