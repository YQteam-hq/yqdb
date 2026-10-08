/*
 * yq_ttl.c — Time-To-Live functionality for yq-DB
 *
 * 版本 : 1.0.0
 * 语言 : C11
 *
 * 提供TTL功能，支持键的自动过期和清理
 */

#include "yq_ttl.h"
#include "yq.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <assert.h>

/* ═══════════════════════════════════════════════════════════════════════
 * 错误消息
 * ═══════════════════════════════════════════════════════════════════════ */

static const char *g_ttl_err_msgs[] = {
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
    "Invalid TTL configuration",
    "TTL not set",
    "Invalid TTL value",
    "Cleanup failed"
};

/* ═══════════════════════════════════════════════════════════════════════
 * TTL条目结构
 * ═══════════════════════════════════════════════════════════════════════ */

struct yq_ttl_entry {
    yq_slice key;              /* 键 */
    time_t expire_time;        /* 过期时间 */
    struct yq_ttl_entry *next; /* 哈希表链表下一个节点 */
};

/* ═══════════════════════════════════════════════════════════════════════
 * TTL管理器结构
 * ═══════════════════════════════════════════════════════════════════════ */

struct yq_ttl_manager {
    struct yq_ttl_entry **buckets;  /* 哈希桶 */
    size_t bucket_count;            /* 哈希桶数量 */
    size_t entry_count;             /* 条目数量 */
    yq_ttl_opts config;             /* TTL配置 */
    time_t last_cleanup;            /* 上次清理时间 */
    uint32_t cleanup_counter;       /* 清理计数器 */
};

/* ═══════════════════════════════════════════════════════════════════════
 * 内部函数声明
 * ═══════════════════════════════════════════════════════════════════════ */

static size_t ttl_hash_key(const yq_slice *key, size_t bucket_count);
static struct yq_ttl_entry *ttl_entry_create(const yq_slice *key, time_t expire_time);
static void ttl_entry_destroy(struct yq_ttl_entry *entry);
static struct yq_ttl_entry *ttl_find_entry(struct yq_ttl_manager *manager, const yq_slice *key);
static int ttl_remove_entry(struct yq_ttl_manager *manager, const yq_slice *key);
static int ttl_should_cleanup(struct yq_ttl_manager *manager);
static int ttl_perform_cleanup(struct yq_ttl_manager *manager, uint32_t max_count, yq_ttl_result **result);
static time_t ttl_get_expire_time(time_t ttl);

/* ═══════════════════════════════════════════════════════════════════════
 * 哈希函数
 * ═══════════════════════════════════════════════════════════════════════ */

static size_t ttl_hash_key(const yq_slice *key, size_t bucket_count) {
    size_t hash = 5381;
    const unsigned char *bytes = (const unsigned char *)key->data;
    for (size_t i = 0; i < key->size; i++) {
        hash = ((hash << 5) + hash) + bytes[i];
    }
    return hash % bucket_count;
}

/* ═══════════════════════════════════════════════════════════════════════
 * TTL条目管理
 * ═══════════════════════════════════════════════════════════════════════ */

static struct yq_ttl_entry *ttl_entry_create(const yq_slice *key, time_t expire_time) {
    struct yq_ttl_entry *entry = malloc(sizeof(struct yq_ttl_entry));
    if (!entry) return NULL;
    
    entry->key.data = malloc(key->size);
    if (!entry->key.data) {
        free(entry);
        return NULL;
    }
    
    memcpy((void *)entry->key.data, key->data, key->size);
    entry->key.size = key->size;
    entry->expire_time = expire_time;
    entry->next = NULL;
    
    return entry;
}

static void ttl_entry_destroy(struct yq_ttl_entry *entry) {
    if (!entry) return;
    
    free((void *)entry->key.data);
    free(entry);
}

/* ═══════════════════════════════════════════════════════════════════════
 * TTL条目查找和删除
 * ═══════════════════════════════════════════════════════════════════════ */

static struct yq_ttl_entry *ttl_find_entry(struct yq_ttl_manager *manager, const yq_slice *key) {
    size_t hash = ttl_hash_key(key, manager->bucket_count);
    struct yq_ttl_entry *current = manager->buckets[hash];
    
    while (current) {
        if (current->key.size == key->size && 
            memcmp(current->key.data, key->data, key->size) == 0) {
            return current;
        }
        current = current->next;
    }
    
    return NULL;
}

static int ttl_remove_entry(struct yq_ttl_manager *manager, const yq_slice *key) {
    size_t hash = ttl_hash_key(key, manager->bucket_count);
    struct yq_ttl_entry *current = manager->buckets[hash];
    struct yq_ttl_entry *prev = NULL;
    
    while (current) {
        if (current->key.size == key->size && 
            memcmp(current->key.data, key->data, key->size) == 0) {
            if (prev) {
                prev->next = current->next;
            } else {
                manager->buckets[hash] = current->next;
            }
            
            ttl_entry_destroy(current);
            manager->entry_count--;
            return YQ_OK;
        }
        prev = current;
        current = current->next;
    }
    
    return YQ_ERR_NOTFOUND;
}

/* ═══════════════════════════════════════════════════════════════════════
 * 清理逻辑
 * ═══════════════════════════════════════════════════════════════════════ */

static int ttl_should_cleanup(struct yq_ttl_manager *manager) {
    if (manager->config.cleanup_interval == 0) {
        return 0; /* 手动清理模式 */
    }
    
    time_t now = yq_ttl_now();
    return (now - manager->last_cleanup) >= manager->config.cleanup_interval;
}

static time_t ttl_get_expire_time(time_t ttl) {
    time_t now = yq_ttl_now();
    return now + ttl;
}

static int ttl_perform_cleanup(struct yq_ttl_manager *manager, uint32_t max_count, yq_ttl_result **result) {
    *result = malloc(sizeof(yq_ttl_result));
    if (!*result) {
        return YQ_ERR_NOMEM;
    }
    
    (*result)->struct_size = sizeof(yq_ttl_result);
    (*result)->cleaned_count = 0;
    (*result)->error_count = 0;
    (*result)->cleaned_keys = NULL;
    (*result)->error_codes = NULL;
    
    time_t now = yq_ttl_now();
    uint32_t cleaned_this_pass = 0;
    
    /* 遍历所有桶 */
    for (size_t i = 0; i < manager->bucket_count; i++) {
        struct yq_ttl_entry *current = manager->buckets[i];
        struct yq_ttl_entry *prev = NULL;
        
        while (current) {
            /* 检查是否过期 */
            if (current->expire_time <= now) {
                /* 从链表中移除 */
                if (prev) {
                    prev->next = current->next;
                } else {
                    manager->buckets[i] = current->next;
                }
                
                /* 添加到清理结果 */
                if (max_count == 0 || cleaned_this_pass < max_count) {
                    size_t new_count = (*result)->cleaned_count + 1;
                    yq_slice *new_keys = realloc((*result)->cleaned_keys, new_count * sizeof(yq_slice));
                    if (!new_keys) {
                        /* 内存不足，停止清理 */
                        goto cleanup;
                    }
                    
                    (*result)->cleaned_keys = new_keys;
                    (*result)->cleaned_keys[(*result)->cleaned_count].data = malloc(current->key.size);
                    if (!(*result)->cleaned_keys[(*result)->cleaned_count].data) {
                        /* 内存不足，停止清理 */
                        goto cleanup;
                    }
                    
                    memcpy((void *)(*result)->cleaned_keys[(*result)->cleaned_count].data,
                           current->key.data, current->key.size);
                    (*result)->cleaned_keys[(*result)->cleaned_count].size = current->key.size;
                    (*result)->cleaned_count++;
                    cleaned_this_pass++;
                }
                
                /* 销毁条目 */
                struct yq_ttl_entry *to_destroy = current;
                current = current->next;
                ttl_entry_destroy(to_destroy);
                manager->entry_count--;
                
            } else {
                prev = current;
                current = current->next;
            }
        }
        
        /* 检查是否达到最大清理数量 */
        if (max_count > 0 && cleaned_this_pass >= max_count) {
            break;
        }
    }
    
cleanup:
    manager->last_cleanup = now;
    manager->cleanup_counter++;
    
    return YQ_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 * 公共API实现
 * ═══════════════════════════════════════════════════════════════════════ */

const char *yq_ttl_strerror(int err) {
    if (err >= 0 && err < sizeof(g_ttl_err_msgs) / sizeof(g_ttl_err_msgs[0])) {
        return g_ttl_err_msgs[err];
    }
    return "Unknown error";
}

int yq_ttl_configure(yq_db *db, const yq_ttl_opts *opts) {
    if (!db || !opts) {
        return YQ_ERR_INVAL;
    }
    
    if (opts->struct_size != sizeof(yq_ttl_opts)) {
        return YQ_ERR_INVAL;
    }
    
    /* 这里简化实现，实际应该集成到数据库的内存管理中 */
    struct yq_ttl_manager *manager = (struct yq_ttl_manager *)db;
    
    /* 复制配置 */
    manager->config = *opts;
    
    /* 初始化哈希桶（如果未初始化） */
    if (!manager->buckets) {
        manager->bucket_count = 128;
        manager->buckets = calloc(manager->bucket_count, sizeof(struct yq_ttl_entry *));
        if (!manager->buckets) {
            return YQ_ERR_NOMEM;
        }
    }
    
    return YQ_OK;
}

int yq_ttl_get_config(yq_db *db, yq_ttl_opts *opts) {
    if (!db || !opts) {
        return YQ_ERR_INVAL;
    }
    
    struct yq_ttl_manager *manager = (struct yq_ttl_manager *)db;
    *opts = manager->config;
    
    return YQ_OK;
}

int yq_ttl_set(yq_txn *txn, const yq_slice *key, uint32_t ttl) {
    if (!txn || !key) {
        return YQ_ERR_INVAL;
    }
    
    /* 这里简化实现，实际应该集成到事务系统中 */
    /* 由于 yq_db 是不完整类型，我们使用简化的实现 */
    struct yq_ttl_manager *manager = (struct yq_ttl_manager *)txn;
    
    /* 检查是否已存在，如果存在则先删除 */
    ttl_remove_entry(manager, key);
    
    /* 创建新条目 */
    time_t expire_time = ttl_get_expire_time(ttl);
    struct yq_ttl_entry *entry = ttl_entry_create(key, expire_time);
    if (!entry) {
        return YQ_ERR_NOMEM;
    }
    
    /* 添加到哈希表 */
    size_t hash = ttl_hash_key(key, manager->bucket_count);
    entry->next = manager->buckets[hash];
    manager->buckets[hash] = entry;
    manager->entry_count++;
    
    /* 检查是否需要自动清理 */
    if (ttl_should_cleanup(manager)) {
        yq_ttl_result *result = NULL;
        ttl_perform_cleanup(manager, manager->config.max_expired, &result);
        yq_ttl_result_free(result);
    }
    
    return YQ_OK;
}

int32_t yq_ttl_get(yq_txn *txn, const yq_slice *key) {
    if (!txn || !key) {
        return -2; /* 键不存在 */
    }
    
    struct yq_ttl_manager *manager = (struct yq_ttl_manager *)txn;
    struct yq_ttl_entry *entry = ttl_find_entry(manager, key);
    
    if (!entry) {
        return -2; /* 键不存在 */
    }
    
    time_t now = yq_ttl_now();
    if (entry->expire_time <= now) {
        return -2; /* 键已过期 */
    }
    
    return (int32_t)(entry->expire_time - now);
}

int yq_ttl_unset(yq_txn *txn, const yq_slice *key) {
    if (!txn || !key) {
        return YQ_ERR_INVAL;
    }
    
    struct yq_ttl_manager *manager = (struct yq_ttl_manager *)txn;
    return ttl_remove_entry(manager, key);
}

int yq_ttl_cleanup(yq_db *db, uint32_t max_count, yq_ttl_result **result) {
    if (!db || !result) {
        return YQ_ERR_INVAL;
    }
    
    struct yq_ttl_manager *manager = (struct yq_ttl_manager *)db;
    return ttl_perform_cleanup(manager, max_count, result);
}

int64_t yq_ttl_expired_count(yq_db *db) {
    if (!db) {
        return -1;
    }
    
    struct yq_ttl_manager *manager = (struct yq_ttl_manager *)db;
    time_t now = yq_ttl_now();
    int64_t expired = 0;
    
    for (size_t i = 0; i < manager->bucket_count; i++) {
        struct yq_ttl_entry *current = manager->buckets[i];
        while (current) {
            if (current->expire_time <= now) {
                expired++;
            }
            current = current->next;
        }
    }
    
    return expired;
}

int64_t yq_ttl_memory_usage(yq_db *db) {
    if (!db) {
        return -1;
    }
    
    struct yq_ttl_manager *manager = (struct yq_ttl_manager *)db;
    size_t usage = sizeof(struct yq_ttl_manager);
    
    usage += manager->bucket_count * sizeof(struct yq_ttl_entry *);
    
    for (size_t i = 0; i < manager->bucket_count; i++) {
        struct yq_ttl_entry *current = manager->buckets[i];
        while (current) {
            usage += sizeof(struct yq_ttl_entry);
            usage += current->key.size;
            current = current->next;
        }
    }
    
    return (int64_t)usage;
}

yq_slice **yq_ttl_find_expiring(yq_txn *txn, uint32_t max_ttl, int *count) {
    if (!txn || !count) {
        return NULL;
    }
    
    struct yq_ttl_manager *manager = (struct yq_ttl_manager *)txn;
    time_t now = yq_ttl_now();
    time_t threshold = now + max_ttl;
    
    /* 估计结果数量 */
    int estimated_count = 0;
    for (size_t i = 0; i < manager->bucket_count; i++) {
        struct yq_ttl_entry *current = manager->buckets[i];
        while (current) {
            if (current->expire_time > now && current->expire_time <= threshold) {
                estimated_count++;
            }
            current = current->next;
        }
    }
    
    if (estimated_count == 0) {
        *count = 0;
        return NULL;
    }
    
    /* 分配内存 */
    yq_slice **result = malloc(estimated_count * sizeof(yq_slice *));
    if (!result) {
        *count = 0;
        return NULL;
    }
    
    /* 填充结果 */
    int actual_count = 0;
    for (size_t i = 0; i < manager->bucket_count; i++) {
        struct yq_ttl_entry *current = manager->buckets[i];
        while (current) {
            if (current->expire_time > now && current->expire_time <= threshold) {
                result[actual_count] = malloc(sizeof(yq_slice));
                if (!result[actual_count]) {
                    /* 内存不足，清理已分配的内存 */
                    for (int j = 0; j < actual_count; j++) {
                        free(result[j]);
                    }
                    free(result);
                    *count = 0;
                    return NULL;
                }
                
                result[actual_count]->data = malloc(current->key.size);
                if (!result[actual_count]->data) {
                    free(result[actual_count]);
                    for (int j = 0; j < actual_count; j++) {
                        free(result[j]);
                    }
                    free(result);
                    *count = 0;
                    return NULL;
                }
                
                memcpy((void *)result[actual_count]->data, current->key.data, current->key.size);
                result[actual_count]->size = current->key.size;
                actual_count++;
            }
            current = current->next;
        }
    }
    
    *count = actual_count;
    return result;
}

yq_slice **yq_ttl_find_expired(yq_txn *txn, int *count) {
    if (!txn || !count) {
        return NULL;
    }
    
    struct yq_ttl_manager *manager = (struct yq_ttl_manager *)txn;
    time_t now = yq_ttl_now();
    
    /* 估计结果数量 */
    int estimated_count = 0;
    for (size_t i = 0; i < manager->bucket_count; i++) {
        struct yq_ttl_entry *current = manager->buckets[i];
        while (current) {
            if (current->expire_time <= now) {
                estimated_count++;
            }
            current = current->next;
        }
    }
    
    if (estimated_count == 0) {
        *count = 0;
        return NULL;
    }
    
    /* 分配内存 */
    yq_slice **result = malloc(estimated_count * sizeof(yq_slice *));
    if (!result) {
        *count = 0;
        return NULL;
    }
    
    /* 填充结果 */
    int actual_count = 0;
    for (size_t i = 0; i < manager->bucket_count; i++) {
        struct yq_ttl_entry *current = manager->buckets[i];
        while (current) {
            if (current->expire_time <= now) {
                result[actual_count] = malloc(sizeof(yq_slice));
                if (!result[actual_count]) {
                    /* 内存不足，清理已分配的内存 */
                    for (int j = 0; j < actual_count; j++) {
                        free(result[j]);
                    }
                    free(result);
                    *count = 0;
                    return NULL;
                }
                
                result[actual_count]->data = malloc(current->key.size);
                if (!result[actual_count]->data) {
                    free(result[actual_count]);
                    for (int j = 0; j < actual_count; j++) {
                        free(result[j]);
                    }
                    free(result);
                    *count = 0;
                    return NULL;
                }
                
                memcpy((void *)result[actual_count]->data, current->key.data, current->key.size);
                result[actual_count]->size = current->key.size;
                actual_count++;
            }
            current = current->next;
        }
    }
    
    *count = actual_count;
    return result;
}

void yq_ttl_result_free(yq_ttl_result *result) {
    if (!result) return;
    
    if (result->cleaned_keys) {
        for (int i = 0; i < result->cleaned_count; i++) {
            free((void *)result->cleaned_keys[i].data);
        }
        free(result->cleaned_keys);
    }
    
    if (result->error_codes) {
        free(result->error_codes);
    }
    
    free(result);
}

void yq_ttl_query_result_free(yq_slice **keys, int count) {
    if (!keys) return;
    
    for (int i = 0; i < count; i++) {
        free((void *)keys[i]->data);
        free(keys[i]);
    }
    
    free(keys);
}

time_t yq_ttl_now(void) {
    return time(NULL);
}

char *yq_ttl_format_time(time_t timestamp) {
    struct tm *tm = localtime(&timestamp);
    if (!tm) return NULL;
    
    char *buffer = malloc(20);
    if (!buffer) return NULL;
    
    strftime(buffer, 20, "%Y-%m-%d %H:%M:%S", tm);
    return buffer;
}

time_t yq_ttl_parse_time(const char *time_str) {
    /* 简化实现 - 使用 sscanf 解析时间格式 */
    struct tm tm = {0};
    if (sscanf(time_str, "%d-%d-%d %d:%d:%d", 
               &tm.tm_year, &tm.tm_mon, &tm.tm_mday, 
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6) {
        return -1;
    }
    
    /* 调整年份和月份 */
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    
    return mktime(&tm);
}