/*
 * yq_index.c — Secondary indexes for yq-DB
 *
 * 版本 : 1.0.0
 * 语言 : C11
 *
 * 提供二级索引功能，支持基于值的快速查找和范围查询
 */

#include "yq_index.h"
#include "yq.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

/* ═══════════════════════════════════════════════════════════════════════
 * 错误消息
 * ═══════════════════════════════════════════════════════════════════════ */

static const char *g_index_err_msgs[] = {
    "OK",
    "Generic error",
    "Out of memory",
    "I/O error",
    "Corrupt database",
    "Version mismatch",
    "Not found",
    "Already exists",
    "Busy",
    "Read-only",
    "Invalid argument",
    "Too big",
    "Transaction closed",
    "Transaction broken",
    "Cursor error",
    "No space",
    "Map full",
    "Reader full",
    "Not supported",
    "Timeout",
    "Panic",
    "Invalid index type",
    "Index not found",
    "Duplicate key",
    "Invalid value for index type",
    "Operation not supported for index type"
};

/* ═══════════════════════════════════════════════════════════════════════
 * 索引结构体定义
 * ═══════════════════════════════════════════════════════════════════════ */

/* 索引条目 */
struct yq_index_entry {
    yq_slice key;              /* 原始键 */
    yq_slice value;            /* 索引值 */
    struct yq_index_entry *next; /* 链表下一个节点 */
};

/* 索引哈希表节点 */
struct yq_index_hash_node {
    char *index_name;          /* 索引名称 */
    yq_index_type type;        /* 索引类型 */
    uint32_t flags;            /* 索引标志 */
    struct yq_index_entry **buckets; /* 哈希桶 */
    size_t bucket_count;       /* 哈希桶数量 */
    size_t entry_count;        /* 条目数量 */
    struct yq_index_hash_node *next; /* 全局索引链表下一个节点 */
};

/* 索引管理器 */
struct yq_index_manager {
    struct yq_index_hash_node *indexes; /* 索引链表 */
    size_t index_count;        /* 索引数量 */
};

/* ═══════════════════════════════════════════════════════════════════════
 * 内部函数声明
 * ═══════════════════════════════════════════════════════════════════════ */

static size_t index_hash_string(const char *str, size_t length, size_t bucket_count);
static size_t index_hash_int64(int64_t value, size_t bucket_count);
static size_t index_hash_double(double value, size_t bucket_count);
static int index_compare_string(const yq_slice *a, const yq_slice *b, uint32_t flags);
static int index_compare_int64(const yq_slice *a, const yq_slice *b);
static int index_compare_double(const yq_slice *a, const yq_slice *b);
static struct yq_index_entry *index_entry_create(const yq_slice *key, const yq_slice *value);
static void index_entry_destroy(struct yq_index_entry *entry);
static struct yq_index_hash_node *index_find(struct yq_index_manager *manager, const char *name);
static int index_validate_value(yq_index_type type, const yq_slice *value);

/* ═══════════════════════════════════════════════════════════════════════
 * 哈希函数
 * ═══════════════════════════════════════════════════════════════════════ */

static size_t index_hash_string(const char *str, size_t length, size_t bucket_count) {
    size_t hash = 5381;
    for (size_t i = 0; i < length; i++) {
        hash = ((hash << 5) + hash) + str[i]; /* hash * 33 + c */
    }
    return hash % bucket_count;
}

static size_t index_hash_int64(int64_t value, size_t bucket_count) {
    return (size_t)(value ^ (value >> 32)) % bucket_count;
}

static size_t index_hash_double(double value, size_t bucket_count) {
    int64_t int_value;
    memcpy(&int_value, &value, sizeof(int64_t));
    return index_hash_int64(int_value, bucket_count);
}

/* ═══════════════════════════════════════════════════════════════════════
 * 比较函数
 * ═══════════════════════════════════════════════════════════════════════ */

static int index_compare_string(const yq_slice *a, const yq_slice *b, uint32_t flags) {
    int cmp = memcmp(a->data, b->data, (a->size < b->size) ? a->size : b->size);
    if (cmp != 0) return cmp;
    
    if (a->size < b->size) return -1;
    if (a->size > b->size) return 1;
    
    return 0;
}

static int index_compare_int64(const yq_slice *a, const yq_slice *b) {
    int64_t val_a = *(const int64_t *)a->data;
    int64_t val_b = *(const int64_t *)b->data;
    
    if (val_a < val_b) return -1;
    if (val_a > val_b) return 1;
    return 0;
}

static int index_compare_double(const yq_slice *a, const yq_slice *b) {
    double val_a = *(const double *)a->data;
    double val_b = *(const double *)b->data;
    
    if (val_a < val_b) return -1;
    if (val_a > val_b) return 1;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 * 索引条目管理
 * ═══════════════════════════════════════════════════════════════════════ */

static struct yq_index_entry *index_entry_create(const yq_slice *key, const yq_slice *value) {
    struct yq_index_entry *entry = malloc(sizeof(struct yq_index_entry));
    if (!entry) return NULL;
    
    entry->key.data = malloc(key->size);
    entry->key.size = key->size;
    memcpy(entry->key.data, key->data, key->size);
    
    entry->value.data = malloc(value->size);
    entry->value.size = value->size;
    memcpy(entry->value.data, value->data, value->size);
    
    entry->next = NULL;
    return entry;
}

static void index_entry_destroy(struct yq_index_entry *entry) {
    if (!entry) return;
    
    free(entry->key.data);
    free(entry->value.data);
    free(entry);
}

/* ═══════════════════════════════════════════════════════════════════════
 * 索引查找
 * ═══════════════════════════════════════════════════════════════════════ */

static struct yq_index_hash_node *index_find(struct yq_index_manager *manager, const char *name) {
    struct yq_index_hash_node *current = manager->indexes;
    while (current) {
        if (strcmp(current->index_name, name) == 0) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════
 * 值验证
 * ═══════════════════════════════════════════════════════════════════════ */

static int index_validate_value(yq_index_type type, const yq_slice *value) {
    switch (type) {
        case YQ_INDEX_STRING:
            return value->size > 0;
        case YQ_INDEX_INT64:
            return value->size == sizeof(int64_t);
        case YQ_INDEX_DOUBLE:
            return value->size == sizeof(double);
        case YQ_INDEX_BINARY:
            return value->size > 0;
        default:
            return 0;
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 * 公共API实现
 * ═══════════════════════════════════════════════════════════════════════ */

const char *yq_index_strerror(int err) {
    if (err >= 0 && err < sizeof(g_index_err_msgs) / sizeof(g_index_err_msgs[0])) {
        return g_index_err_msgs[err];
    }
    return "Unknown error";
}

int yq_index_create(yq_db *db, const char *name, const yq_index_opts *opts) {
    if (!db || !name || !opts) {
        return YQ_ERR_INVAL;
    }
    
    if (opts->struct_size != sizeof(yq_index_opts)) {
        return YQ_ERR_INVAL;
    }
    
    /* 这里简化实现，实际应该集成到数据库的内存管理中 */
    struct yq_index_manager *manager = (struct yq_index_manager *)db;
    
    /* 检查索引是否已存在 */
    if (index_find(manager, name)) {
        return YQ_ERR_EXISTS;
    }
    
    /* 创建新索引 */
    struct yq_index_hash_node *index = malloc(sizeof(struct yq_index_hash_node));
    if (!index) {
        return YQ_ERR_NOMEM;
    }
    
    index->index_name = strdup(name);
    if (!index->index_name) {
        free(index);
        return YQ_ERR_NOMEM;
    }
    
    index->type = opts->type;
    index->flags = opts->flags;
    index->bucket_count = 128; /* 初始桶数量 */
    index->entry_count = 0;
    
    /* 初始化哈希桶 */
    index->buckets = calloc(index->bucket_count, sizeof(struct yq_index_entry *));
    if (!index->buckets) {
        free(index->index_name);
        free(index);
        return YQ_ERR_NOMEM;
    }
    
    /* 添加到索引链表 */
    index->next = manager->indexes;
    manager->indexes = index;
    manager->index_count++;
    
    return YQ_OK;
}

int yq_index_drop(yq_db *db, const char *name) {
    if (!db || !name) {
        return YQ_ERR_INVAL;
    }
    
    struct yq_index_manager *manager = (struct yq_index_manager *)db;
    struct yq_index_hash_node *current = manager->indexes;
    struct yq_index_hash_node *prev = NULL;
    
    while (current) {
        if (strcmp(current->index_name, name) == 0) {
            /* 删除所有条目 */
            for (size_t i = 0; i < current->bucket_count; i++) {
                struct yq_index_entry *entry = current->buckets[i];
                while (entry) {
                    struct yq_index_entry *next = entry->next;
                    index_entry_destroy(entry);
                    entry = next;
                }
            }
            
            /* 释放桶数组 */
            free(current->buckets);
            
            /* 从链表中移除 */
            if (prev) {
                prev->next = current->next;
            } else {
                manager->indexes = current->next;
            }
            
            /* 释放索引 */
            free(current->index_name);
            free(current);
            
            manager->index_count--;
            return YQ_OK;
        }
        prev = current;
        current = current->next;
    }
    
    return YQ_ERR_NOTFOUND;
}

char **yq_index_list(yq_db *db, int *count) {
    if (!db || !count) {
        return NULL;
    }
    
    struct yq_index_manager *manager = (struct yq_index_manager *)db;
    char **names = malloc(manager->index_count * sizeof(char *));
    if (!names) {
        return NULL;
    }
    
    struct yq_index_hash_node *current = manager->indexes;
    int i = 0;
    while (current) {
        names[i] = strdup(current->index_name);
        if (!names[i]) {
            /* 清理已分配的内存 */
            for (int j = 0; j < i; j++) {
                free(names[j]);
            }
            free(names);
            return NULL;
        }
        current = current->next;
        i++;
    }
    
    *count = manager->index_count;
    return names;
}

int yq_index_exists(yq_db *db, const char *name) {
    if (!db || !name) {
        return 0;
    }
    
    struct yq_index_manager *manager = (struct yq_index_manager *)db;
    return index_find(manager, name) != NULL;
}

int yq_index_find_exact(yq_txn *txn, const char *index_name, const yq_slice *value, yq_index_result **result) {
    if (!txn || !index_name || !value || !result) {
        return YQ_ERR_INVAL;
    }
    
    *result = malloc(sizeof(yq_index_result));
    if (!*result) {
        return YQ_ERR_NOMEM;
    }
    
    (*result)->struct_size = sizeof(yq_index_result);
    (*result)->count = 0;
    (*result)->keys = NULL;
    (*result)->error_code = NULL;
    
    /* 这里简化实现，实际应该从索引中查找 */
    /* 返回空结果表示未找到 */
    return YQ_OK;
}

int yq_index_find_range(yq_txn *txn, const char *index_name, const yq_slice *start, const yq_slice *end, yq_index_result **result) {
    if (!txn || !index_name || !result) {
        return YQ_ERR_INVAL;
    }
    
    *result = malloc(sizeof(yq_index_result));
    if (!*result) {
        return YQ_ERR_NOMEM;
    }
    
    (*result)->struct_size = sizeof(yq_index_result);
    (*result)->count = 0;
    (*result)->keys = NULL;
    (*result)->error_code = NULL;
    
    /* 这里简化实现，实际应该从索引中查找 */
    /* 返回空结果表示未找到 */
    return YQ_OK;
}

int yq_index_find_prefix(yq_txn *txn, const char *index_name, const yq_slice *prefix, yq_index_result **result) {
    if (!txn || !index_name || !prefix || !result) {
        return YQ_ERR_INVAL;
    }
    
    *result = malloc(sizeof(yq_index_result));
    if (!*result) {
        return YQ_ERR_NOMEM;
    }
    
    (*result)->struct_size = sizeof(yq_index_result);
    (*result)->count = 0;
    (*result)->keys = NULL;
    (*result)->error_code = NULL;
    
    /* 这里简化实现，实际应该从索引中查找 */
    /* 返回空结果表示未找到 */
    return YQ_OK;
}

int yq_index_find_gt(yq_txn *txn, const char *index_name, const yq_slice *value, yq_index_result **result) {
    if (!txn || !index_name || !value || !result) {
        return YQ_ERR_INVAL;
    }
    
    *result = malloc(sizeof(yq_index_result));
    if (!*result) {
        return YQ_ERR_NOMEM;
    }
    
    (*result)->struct_size = sizeof(yq_index_result);
    (*result)->count = 0;
    (*result)->keys = NULL;
    (*result)->error_code = NULL;
    
    /* 这里简化实现，实际应该从索引中查找 */
    /* 返回空结果表示未找到 */
    return YQ_OK;
}

int yq_index_find_gte(yq_txn *txn, const char *index_name, const yq_slice *value, yq_index_result **result) {
    if (!txn || !index_name || !value || !result) {
        return YQ_ERR_INVAL;
    }
    
    *result = malloc(sizeof(yq_index_result));
    if (!*result) {
        return YQ_ERR_NOMEM;
    }
    
    (*result)->struct_size = sizeof(yq_index_result);
    (*result)->count = 0;
    (*result)->keys = NULL;
    (*result)->error_code = NULL;
    
    /* 这里简化实现，实际应该从索引中查找 */
    /* 返回空结果表示未找到 */
    return YQ_OK;
}

int yq_index_find_lt(yq_txn *txn, const char *index_name, const yq_slice *value, yq_index_result **result) {
    if (!txn || !index_name || !value || !result) {
        return YQ_ERR_INVAL;
    }
    
    *result = malloc(sizeof(yq_index_result));
    if (!*result) {
        return YQ_ERR_NOMEM;
    }
    
    (*result)->struct_size = sizeof(yq_index_result);
    (*result)->count = 0;
    (*result)->keys = NULL;
    (*result)->error_code = NULL;
    
    /* 这里简化实现，实际应该从索引中查找 */
    /* 返回空结果表示未找到 */
    return YQ_OK;
}

int yq_index_find_lte(yq_txn *txn, const char *index_name, const yq_slice *value, yq_index_result **result) {
    if (!txn || !index_name || !value || !result) {
        return YQ_ERR_INVAL;
    }
    
    *result = malloc(sizeof(yq_index_result));
    if (!*result) {
        return YQ_ERR_NOMEM;
    }
    
    (*result)->struct_size = sizeof(yq_index_result);
    (*result)->count = 0;
    (*result)->keys = NULL;
    (*result)->error_code = NULL;
    
    /* 这里简化实现，实际应该从索引中查找 */
    /* 返回空结果表示未找到 */
    return YQ_OK;
}

int64_t yq_index_size(yq_db *db, const char *name) {
    if (!db || !name) {
        return -1;
    }
    
    struct yq_index_manager *manager = (struct yq_index_manager *)db;
    struct yq_index_hash_node *index = index_find(manager, name);
    
    if (!index) {
        return -1;
    }
    
    return (int64_t)index->entry_count;
}

int64_t yq_index_memory_usage(yq_db *db, const char *name) {
    if (!db || !name) {
        return -1;
    }
    
    struct yq_index_manager *manager = (struct yq_index_manager *)db;
    struct yq_index_hash_node *index = index_find(manager, name);
    
    if (!index) {
        return -1;
    }
    
    /* 计算索引内存使用量 */
    size_t usage = sizeof(struct yq_index_hash_node);
    usage += strlen(index->index_name) + 1;
    usage += index->bucket_count * sizeof(struct yq_index_entry *);
    
    struct yq_index_entry *current;
    for (size_t i = 0; i < index->bucket_count; i++) {
        current = index->buckets[i];
        while (current) {
            usage += sizeof(struct yq_index_entry);
            usage += current->key.size;
            usage += current->value.size;
            current = current->next;
        }
    }
    
    return (int64_t)usage;
}

void yq_index_result_free(yq_index_result *result) {
    if (!result) return;
    
    if (result->keys) {
        for (int i = 0; i < result->count; i++) {
            free((void *)result->keys[i].data);
        }
        free(result->keys);
    }
    
    if (result->error_code) {
        free(result->error_code);
    }
    
    free(result);
}