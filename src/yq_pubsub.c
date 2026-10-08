/*
 * yq_pubsub.c — yq-DB Publish/Subscribe messaging implementation
 *
 * Version : 1.0.0
 * Language: C11
 * Format version: 1
 */

#if YQ_ENABLE_PUBSUB

#include "yq.h"
#include "yq_pubsub.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <sys/time.h>
#include <stdarg.h>
#include <stdio.h>
#include <errno.h>

/* ═══════════════════════════════════════════════════════════════════════
 * 内部结构定义
 * ═══════════════════════════════════════════════════════════════════════ */

/* 内部消息结构 */
typedef struct yq_internal_message {
    uint64_t message_id;
    uint64_t timestamp;
    uint32_t priority;
    uint32_t flags;
    uint32_t topic_len;
    uint32_t payload_len;
    uint32_t metadata_len;
    char *topic;
    void *payload;
    void *metadata;
    struct yq_internal_message *next;
    uint32_t status;
    uint32_t retry_count;
} yq_internal_message;

/* 内部主题结构 */
typedef struct yq_internal_topic {
    char *name;
    yq_internal_message *message_list;
    uint32_t subscriber_count;
    pthread_mutex_t mutex;
    uint64_t message_count;
    uint64_t total_bytes;
    struct yq_internal_topic *next;
} yq_internal_topic;

/* 内部订阅者结构 */
typedef struct yq_internal_subscriber {
    char *topic_filter;
    yq_subscriber_opts opts;
    yq_message_callback message_cb;
    yq_error_callback error_cb;
    void *user_data;
    pthread_t thread;
    int running;
    struct yq_internal_message_queue *queue;
    struct yq_internal_subscriber *next;
} yq_internal_subscriber;

/* 内部消息队列结构 */
typedef struct yq_internal_message_queue {
    yq_internal_message **messages;
    size_t capacity;
    size_t size;
    size_t head;
    size_t tail;
    pthread_mutex_t mutex;
    pthread_cond_t cond_not_empty;
    pthread_cond_t cond_not_full;
} yq_internal_message_queue;

/* 内部Pub/Sub结构 */
struct yq_pubsub {
    yq_db *db;
    struct yq_internal_topic *topics;
    struct yq_internal_subscriber *subscribers;
    pthread_mutex_t mutex;
    uint64_t next_message_id;
    struct {
        uint64_t total_messages;
        uint64_t delivered_messages;
        uint64_t failed_messages;
        uint32_t active_topics;
        uint32_t active_subscribers;
        uint64_t total_bytes;
    } stats;
    yq_stats_callback stats_callback;
    void *stats_user_data;
    int debug_enabled;
};

/* ═══════════════════════════════════════════════════════════════════════
 * 辅助函数
 * ═══════════════════════════════════════════════════════════════════════ */

static uint64_t yq_current_timestamp_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}

static int yq_strcmp_prefix(const char *str, const char *prefix) {
    return strncmp(str, prefix, strlen(prefix)) == 0;
}

static int yq_topic_matches_filter(const char *topic, const yq_topic_filter *filter) {
    if (!filter || !filter->pattern) {
        return 1;
    }

    switch (filter->type) {
        case YQ_FILTER_NONE:
            return 1;
        case YQ_FILTER_PREFIX:
            return yq_strcmp_prefix(topic, filter->pattern);
        case YQ_FILTER_REGEX:
            /* 简化的正则匹配，实际实现应使用正则库 */
            return strstr(topic, filter->pattern) != NULL;
        case YQ_FILTER_CUSTOM:
            /* 自定义过滤逻辑 */
            return yq_strcmp_prefix(topic, filter->pattern);
        default:
            return 0;
    }
}

static void yq_log_debug(yq_pubsub *pubsub, const char *format, ...) {
    if (pubsub && pubsub->debug_enabled) {
        va_list args;
        va_start(args, format);
        printf("[YQ-PUBSUB] ");
        vprintf(format, args);
        printf("\n");
        va_end(args);
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 * 消息队列实现
 * ═══════════════════════════════════════════════════════════════════════ */

static struct yq_internal_message_queue *yq_internal_message_queue_create(size_t capacity) {
    struct yq_internal_message_queue *queue = malloc(sizeof(struct yq_internal_message_queue));
    if (!queue) return NULL;

    queue->messages = malloc(sizeof(struct yq_internal_message *) * capacity);
    if (!queue->messages) {
        free(queue);
        return NULL;
    }

    queue->capacity = capacity;
    queue->size = 0;
    queue->head = 0;
    queue->tail = 0;
    pthread_mutex_init(&queue->mutex, NULL);
    pthread_cond_init(&queue->cond_not_empty, NULL);
    pthread_cond_init(&queue->cond_not_full, NULL);

    return queue;
}

static void yq_internal_message_queue_free(struct yq_internal_message_queue *queue) {
    if (!queue) return;

    pthread_mutex_destroy(&queue->mutex);
    pthread_cond_destroy(&queue->cond_not_empty);
    pthread_cond_destroy(&queue->cond_not_full);
    free(queue->messages);
    free(queue);
}

static int yq_internal_message_queue_push(struct yq_internal_message_queue *queue, struct yq_internal_message *msg) {
    pthread_mutex_lock(&queue->mutex);

    while (queue->size >= queue->capacity) {
        pthread_cond_wait(&queue->cond_not_full, &queue->mutex);
    }

    queue->messages[queue->tail] = msg;
    queue->tail = (queue->tail + 1) % queue->capacity;
    queue->size++;

    pthread_cond_signal(&queue->cond_not_empty);
    pthread_mutex_unlock(&queue->mutex);

    return 0;
}

static struct yq_internal_message *yq_internal_message_queue_pop(struct yq_internal_message_queue *queue, uint32_t timeout_ms) {
    pthread_mutex_lock(&queue->mutex);

    while (queue->size == 0) {
        if (timeout_ms == 0) {
            pthread_cond_wait(&queue->cond_not_empty, &queue->mutex);
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += (timeout_ms % 1000) * 1000000;
            ts.tv_sec += timeout_ms / 1000 + ts.tv_nsec / 1000000000;
            ts.tv_nsec %= 1000000000;

            int ret = pthread_cond_timedwait(&queue->cond_not_empty, &queue->mutex, &ts);
            if (ret == ETIMEDOUT) {
                pthread_mutex_unlock(&queue->mutex);
                return NULL;
            }
        }
    }

    struct yq_internal_message *msg = queue->messages[queue->head];
    queue->head = (queue->head + 1) % queue->capacity;
    queue->size--;

    pthread_cond_signal(&queue->cond_not_full);
    pthread_mutex_unlock(&queue->mutex);

    return msg;
}

/* ═══════════════════════════════════════════════════════════════════════
 * 主题管理实现
 * ═══════════════════════════════════════════════════════════════════════ */

static struct yq_internal_topic *yq_internal_topic_find(struct yq_pubsub *pubsub, const char *name) {
    struct yq_internal_topic *topic = pubsub->topics;
    while (topic) {
        if (strcmp(topic->name, name) == 0) {
            return topic;
        }
        topic = topic->next;
    }
    return NULL;
}

static struct yq_internal_topic *yq_internal_topic_create(struct yq_pubsub *pubsub, const char *name) {
    struct yq_internal_topic *topic = malloc(sizeof(struct yq_internal_topic));
    if (!topic) return NULL;

    topic->name = strdup(name);
    if (!topic->name) {
        free(topic);
        return NULL;
    }

    topic->message_list = NULL;
    topic->subscriber_count = 0;
    pthread_mutex_init(&topic->mutex, NULL);
    topic->message_count = 0;
    topic->total_bytes = 0;
    topic->next = pubsub->topics;
    pubsub->topics = topic;

    return topic;
}

static void yq_internal_topic_free(struct yq_internal_topic *topic) {
    if (!topic) return;

    pthread_mutex_destroy(&topic->mutex);
    free(topic->name);

    struct yq_internal_message *msg = topic->message_list;
    while (msg) {
        struct yq_internal_message *next = msg->next;
        free(msg->topic);
        free(msg->payload);
        free(msg->metadata);
        free(msg);
        msg = next;
    }

    free(topic);
}

/* ═══════════════════════════════════════════════════════════════════════
 * 消息处理线程
 * ═══════════════════════════════════════════════════════════════════════ */

static void *yq_subscriber_thread(void *arg) {
    struct yq_internal_subscriber *sub = (struct yq_internal_subscriber *)arg;
    struct yq_internal_message_queue *queue = sub->queue;

    while (sub->running) {
        struct yq_internal_message *msg = yq_internal_message_queue_pop(queue, 1000);
        if (msg) {
            /* 转换为公共消息格式 */
            yq_message public_msg;
            memset(&public_msg, 0, sizeof(public_msg));
            public_msg.struct_size = sizeof(public_msg);
            public_msg.message_id = msg->message_id;
            public_msg.timestamp = msg->timestamp;
            public_msg.priority = msg->priority;
            public_msg.flags = msg->flags;
            public_msg.topic_len = msg->topic_len;
            public_msg.payload_len = msg->payload_len;
            public_msg.metadata_len = msg->metadata_len;
            public_msg.topic = msg->topic;
            public_msg.payload = msg->payload;
            public_msg.metadata = msg->metadata;

            /* 调用消息回调 */
            if (sub->message_cb) {
                int result = sub->message_cb((yq_subscriber *)sub, &public_msg, sub->user_data);
                if (result != 0) {
                    /* 处理失败，根据重试策略决定是否重新投递 */
                    if (sub->opts.max_retry > 0 && msg->retry_count < sub->opts.max_retry) {
                        msg->retry_count++;
                        yq_internal_message_queue_push(queue, msg);
                    } else {
                        /* 超过最大重试次数，调用错误回调 */
                        if (sub->error_cb) {
                            sub->error_cb((yq_subscriber *)sub, YQ_PUBSUB_ERR_MESSAGE, "Message processing failed", sub->user_data);
                        }
                    }
                }
            }

            free(msg->topic);
            free(msg->payload);
            free(msg->metadata);
            free(msg);
        }
    }

    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════
 * 公共API实现
 * ═══════════════════════════════════════════════════════════════════════ */

int yq_pubsub_init(yq_db *db, struct yq_pubsub **out) {
    if (!db || !out) {
        return YQ_ERR_INVAL;
    }

    struct yq_pubsub *pubsub = malloc(sizeof(struct yq_pubsub));
    if (!pubsub) {
        return YQ_ERR_NOMEM;
    }

    memset(pubsub, 0, sizeof(struct yq_pubsub));
    pubsub->db = db;
    pubsub->next_message_id = 1;
    pubsub->debug_enabled = 0;

    pthread_mutex_init(&pubsub->mutex, NULL);

    *out = pubsub;
    yq_log_debug(pubsub, "Pub/Sub system initialized");
    return YQ_OK;
}

int yq_pubsub_close(struct yq_pubsub *pubsub) {
    if (!pubsub) {
        return YQ_OK;
    }

    pthread_mutex_lock(&pubsub->mutex);

    /* 停止所有订阅者线程 */
    struct yq_internal_subscriber *sub = pubsub->subscribers;
    while (sub) {
        struct yq_internal_subscriber *next = sub->next;
        sub->running = 0;
        pthread_join(sub->thread, NULL);
        free(sub->topic_filter);
        free(sub);
        sub = next;
    }

    /* 释放所有主题 */
    struct yq_internal_topic *topic = pubsub->topics;
    while (topic) {
        struct yq_internal_topic *next = topic->next;
        yq_internal_topic_free(topic);
        topic = next;
    }

    pthread_mutex_unlock(&pubsub->mutex);
    pthread_mutex_destroy(&pubsub->mutex);
    free(pubsub);

    return YQ_OK;
}

int yq_pubsub_get_stats(struct yq_pubsub *pubsub, yq_pubsub_stats *stats) {
    if (!pubsub || !stats) {
        return YQ_ERR_INVAL;
    }

    memset(stats, 0, sizeof(yq_pubsub_stats));
    stats->struct_size = sizeof(yq_pubsub_stats);

    pthread_mutex_lock(&pubsub->mutex);

    stats->total_messages = pubsub->stats.total_messages;
    stats->delivered_messages = pubsub->stats.delivered_messages;
    stats->failed_messages = pubsub->stats.failed_messages;
    stats->total_bytes = pubsub->stats.total_bytes;

    /* 统计活跃主题数 */
    struct yq_internal_topic *topic = pubsub->topics;
    while (topic) {
        stats->active_topics++;
        topic = topic->next;
    }

    /* 统计活跃订阅者数 */
    struct yq_internal_subscriber *sub = pubsub->subscribers;
    while (sub) {
        stats->active_subscribers++;
        sub = sub->next;
    }

    pthread_mutex_unlock(&pubsub->mutex);

    return YQ_OK;
}

int yq_topic_create(struct yq_pubsub *pubsub, const char *name, struct yq_topic **out) {
    if (!pubsub || !name || !out) {
        return YQ_ERR_INVAL;
    }

    pthread_mutex_lock(&pubsub->mutex);

    struct yq_internal_topic *topic = yq_internal_topic_find(pubsub, name);
    if (topic) {
        pthread_mutex_unlock(&pubsub->mutex);
        return YQ_ERR_EXISTS;
    }

    topic = yq_internal_topic_create(pubsub, name);
    if (!topic) {
        pthread_mutex_unlock(&pubsub->mutex);
        return YQ_ERR_NOMEM;
    }

    *out = (struct yq_topic *)topic;
    pthread_mutex_unlock(&pubsub->mutex);

    yq_log_debug(pubsub, "Topic created: %s", name);
    return YQ_OK;
}

int yq_topic_delete(struct yq_pubsub *pubsub, const char *name) {
    if (!pubsub || !name) {
        return YQ_ERR_INVAL;
    }

    pthread_mutex_lock(&pubsub->mutex);

    struct yq_internal_topic *prev = NULL;
    struct yq_internal_topic *topic = pubsub->topics;
    while (topic) {
        if (strcmp(topic->name, name) == 0) {
            if (prev) {
                prev->next = topic->next;
            } else {
                pubsub->topics = topic->next;
            }
            yq_internal_topic_free(topic);
            pthread_mutex_unlock(&pubsub->mutex);
            yq_log_debug(pubsub, "Topic deleted: %s", name);
            return YQ_OK;
        }
        prev = topic;
        topic = topic->next;
    }

    pthread_mutex_unlock(&pubsub->mutex);
    return YQ_ERR_NOTFOUND;
}

int yq_topic_get(struct yq_pubsub *pubsub, const char *name, struct yq_topic **out) {
    if (!pubsub || !name || !out) {
        return YQ_ERR_INVAL;
    }

    pthread_mutex_lock(&pubsub->mutex);

    struct yq_internal_topic *topic = yq_internal_topic_find(pubsub, name);
    if (!topic) {
        pthread_mutex_unlock(&pubsub->mutex);
        return YQ_ERR_NOTFOUND;
    }

    *out = (struct yq_topic *)topic;
    pthread_mutex_unlock(&pubsub->mutex);

    return YQ_OK;
}

int yq_topic_list(struct yq_pubsub *pubsub, char ***topics, uint32_t *count) {
    if (!pubsub || !topics || !count) {
        return YQ_ERR_INVAL;
    }

    pthread_mutex_lock(&pubsub->mutex);

    /* 先计算主题数量 */
    uint32_t topic_count = 0;
    struct yq_internal_topic *topic = pubsub->topics;
    while (topic) {
        topic_count++;
        topic = topic->next;
    }

    if (topic_count == 0) {
        *topics = NULL;
        *count = 0;
        pthread_mutex_unlock(&pubsub->mutex);
        return YQ_OK;
    }

    /* 分配字符串数组 */
    char **topic_list = malloc(sizeof(char *) * topic_count);
    if (!topic_list) {
        pthread_mutex_unlock(&pubsub->mutex);
        return YQ_ERR_NOMEM;
    }

    /* 复制主题名称 */
    topic = pubsub->topics;
    for (uint32_t i = 0; i < topic_count; i++) {
        topic_list[i] = strdup(topic->name);
        if (!topic_list[i]) {
            /* 清理已分配的内存 */
            for (uint32_t j = 0; j < i; j++) {
                free(topic_list[j]);
            }
            free(topic_list);
            pthread_mutex_unlock(&pubsub->mutex);
            return YQ_ERR_NOMEM;
        }
        topic = topic->next;
    }

    *topics = topic_list;
    *count = topic_count;
    pthread_mutex_unlock(&pubsub->mutex);

    return YQ_OK;
}

int yq_topic_get_stats(struct yq_topic *topic, yq_topic_stats *stats) {
    if (!topic || !stats) {
        return YQ_ERR_INVAL;
    }

    memset(stats, 0, sizeof(yq_topic_stats));
    stats->struct_size = sizeof(yq_topic_stats);

    struct yq_internal_topic *internal_topic = (struct yq_internal_topic *)topic;
    pthread_mutex_lock(&internal_topic->mutex);

    stats->message_count = internal_topic->message_count;
    stats->subscriber_count = internal_topic->subscriber_count;
    stats->total_bytes = internal_topic->total_bytes;

    pthread_mutex_unlock(&internal_topic->mutex);

    return YQ_OK;
}

int yq_publish(struct yq_pubsub *pubsub, const char *topic, const void *payload, size_t payload_len, 
               uint32_t flags, uint64_t *message_id) {
    return yq_publish_ex(pubsub, topic, payload, payload_len, NULL, 0, flags, 
                         YQ_MESSAGE_NORMAL, message_id);
}

int yq_publish_ex(struct yq_pubsub *pubsub, const char *topic, const void *payload, size_t payload_len,
                  const void *metadata, size_t metadata_len, uint32_t flags, 
                  uint32_t priority, uint64_t *message_id) {
    if (!pubsub || !topic || !payload || payload_len == 0 || !message_id) {
        return YQ_ERR_INVAL;
    }

    pthread_mutex_lock(&pubsub->mutex);

    /* 创建或获取主题 */
    struct yq_internal_topic *internal_topic = yq_internal_topic_find(pubsub, topic);
    if (!internal_topic) {
        internal_topic = yq_internal_topic_create(pubsub, topic);
        if (!internal_topic) {
            pthread_mutex_unlock(&pubsub->mutex);
            return YQ_ERR_NOMEM;
        }
    }

    /* 创建消息 */
    struct yq_internal_message *msg = malloc(sizeof(struct yq_internal_message));
    if (!msg) {
        pthread_mutex_unlock(&pubsub->mutex);
        return YQ_ERR_NOMEM;
    }

    memset(msg, 0, sizeof(struct yq_internal_message));
    msg->message_id = pubsub->next_message_id++;
    msg->timestamp = yq_current_timestamp_ms();
    msg->priority = priority;
    msg->flags = flags;
    msg->topic_len = strlen(topic);
    msg->payload_len = payload_len;
    msg->metadata_len = metadata_len;
    msg->status = 0;
    msg->retry_count = 0;

    /* 复制主题 */
    msg->topic = malloc(msg->topic_len + 1);
    if (!msg->topic) {
        free(msg);
        pthread_mutex_unlock(&pubsub->mutex);
        return YQ_ERR_NOMEM;
    }
    strcpy(msg->topic, topic);

    /* 复制负载 */
    msg->payload = malloc(msg->payload_len);
    if (!msg->payload) {
        free(msg->topic);
        free(msg);
        pthread_mutex_unlock(&pubsub->mutex);
        return YQ_ERR_NOMEM;
    }
    memcpy(msg->payload, payload, msg->payload_len);

    /* 复制元数据 */
    if (metadata_len > 0) {
        msg->metadata = malloc(msg->metadata_len);
        if (!msg->metadata) {
            free(msg->topic);
            free(msg->payload);
            free(msg);
            pthread_mutex_unlock(&pubsub->mutex);
            return YQ_ERR_NOMEM;
        }
        memcpy(msg->metadata, metadata, metadata_len);
    }

    /* 添加到主题消息列表 */
    msg->next = internal_topic->message_list;
    internal_topic->message_list = msg;

    /* 更新统计 */
    internal_topic->message_count++;
    internal_topic->total_bytes += msg->topic_len + msg->payload_len + msg->metadata_len;
    pubsub->stats.total_messages++;
    pubsub->stats.total_bytes += msg->topic_len + msg->payload_len + msg->metadata_len;

    /* 投递给所有订阅者 */
    struct yq_internal_subscriber *sub = pubsub->subscribers;
    while (sub) {
        if (yq_topic_matches_filter(topic, &(yq_topic_filter){ 
            sizeof(yq_topic_filter), YQ_FILTER_PREFIX, 0, {0}, sub->topic_filter })) {
            /* 这里应该投递到订阅者的队列 */
            /* 简化实现，直接调用回调 */
            if (sub->message_cb) {
                yq_message public_msg;
                memset(&public_msg, 0, sizeof(public_msg));
                public_msg.struct_size = sizeof(public_msg);
                public_msg.message_id = msg->message_id;
                public_msg.timestamp = msg->timestamp;
                public_msg.priority = msg->priority;
                public_msg.flags = msg->flags;
                public_msg.topic_len = msg->topic_len;
                public_msg.payload_len = msg->payload_len;
                public_msg.metadata_len = msg->metadata_len;
                public_msg.topic = msg->topic;
                public_msg.payload = msg->payload;
                public_msg.metadata = msg->metadata;

                int result = sub->message_cb((yq_subscriber *)sub, &public_msg, sub->user_data);
                if (result == 0) {
                    pubsub->stats.delivered_messages++;
                } else {
                    pubsub->stats.failed_messages++;
                }
            }
        }
        sub = sub->next;
    }

    *message_id = msg->message_id;
    pthread_mutex_unlock(&pubsub->mutex);

    yq_log_debug(pubsub, "Published message %lu to topic %s", msg->message_id, topic);
    return YQ_OK;
}

int yq_publish_batch(struct yq_pubsub *pubsub, const yq_message **messages, size_t count, 
                      uint64_t *message_ids) {
    if (!pubsub || !messages || count == 0 || !message_ids) {
        return YQ_ERR_INVAL;
    }

    for (size_t i = 0; i < count; i++) {
        int result = yq_publish_ex(pubsub, messages[i]->topic, messages[i]->payload, 
                                   messages[i]->payload_len, messages[i]->metadata,
                                   messages[i]->metadata_len, messages[i]->flags,
                                   messages[i]->priority, &message_ids[i]);
        if (result != YQ_OK) {
            return result;
        }
    }

    return YQ_OK;
}

int yq_subscribe(struct yq_pubsub *pubsub, const char *topic, const yq_subscriber_opts *opts,
                 yq_message_callback message_cb, yq_error_callback error_cb,
                 void *user_data, struct yq_subscriber **out) {
    if (!pubsub || !topic || !message_cb || !out) {
        return YQ_ERR_INVAL;
    }

    /* 使用默认选项 */
    yq_subscriber_opts default_opts;
    memset(&default_opts, 0, sizeof(default_opts));
    default_opts.struct_size = sizeof(default_opts);
    default_opts.queue_size = 1000;
    default_opts.max_retry = 3;
    default_opts.retry_delay_ms = 1000;
    default_opts.delivery_mode = YQ_DELIVERY_AT_LEAST_ONCE;

    if (opts) {
        memcpy(&default_opts, opts, sizeof(default_opts));
    }

    return yq_subscribe_filtered(pubsub, &(yq_topic_filter){
        sizeof(yq_topic_filter), YQ_FILTER_PREFIX, 0, {0}, topic
    }, &default_opts, message_cb, error_cb, user_data, out);
}

int yq_subscribe_filtered(struct yq_pubsub *pubsub, const yq_topic_filter *filter,
                          const yq_subscriber_opts *opts,
                          yq_message_callback message_cb, yq_error_callback error_cb,
                          void *user_data, struct yq_subscriber **out) {
    if (!pubsub || !filter || !filter->pattern || !message_cb || !out) {
        return YQ_ERR_INVAL;
    }

    pthread_mutex_lock(&pubsub->mutex);

    /* 检查是否已经订阅过相同的过滤器 */
    struct yq_internal_subscriber *sub = pubsub->subscribers;
    while (sub) {
        if (strcmp(sub->topic_filter, filter->pattern) == 0) {
            pthread_mutex_unlock(&pubsub->mutex);
            return YQ_ERR_EXISTS;
        }
        sub = sub->next;
    }

    /* 创建订阅者 */
    struct yq_internal_subscriber *internal_sub = malloc(sizeof(struct yq_internal_subscriber));
    if (!internal_sub) {
        pthread_mutex_unlock(&pubsub->mutex);
        return YQ_ERR_NOMEM;
    }

    memset(internal_sub, 0, sizeof(struct yq_internal_subscriber));
    internal_sub->topic_filter = strdup(filter->pattern);
    if (!internal_sub->topic_filter) {
        free(internal_sub);
        pthread_mutex_unlock(&pubsub->mutex);
        return YQ_ERR_NOMEM;
    }

    memcpy(&internal_sub->opts, opts, sizeof(yq_subscriber_opts));
    internal_sub->message_cb = message_cb;
    internal_sub->error_cb = error_cb;
    internal_sub->user_data = user_data;
    internal_sub->running = 1;

    /* 创建消息队列 */
    internal_sub->queue = yq_internal_message_queue_create(internal_sub->opts.queue_size);
    if (!internal_sub->queue) {
        free(internal_sub->topic_filter);
        free(internal_sub);
        pthread_mutex_unlock(&pubsub->mutex);
        return YQ_ERR_NOMEM;
    }

    /* 启动处理线程 */
    if (pthread_create(&internal_sub->thread, NULL, yq_subscriber_thread, internal_sub) != 0) {
        yq_internal_message_queue_free(internal_sub->queue);
        free(internal_sub->topic_filter);
        free(internal_sub);
        pthread_mutex_unlock(&pubsub->mutex);
        return YQ_ERR_NOMEM;
    }

    /* 添加到订阅者列表 */
    internal_sub->next = pubsub->subscribers;
    pubsub->subscribers = internal_sub;

    *out = (struct yq_subscriber *)internal_sub;
    pthread_mutex_unlock(&pubsub->mutex);

    yq_log_debug(pubsub, "Subscribed to filter: %s", filter->pattern);
    return YQ_OK;
}

int yq_unsubscribe(struct yq_subscriber *sub) {
    if (!sub) {
        return YQ_OK;
    }

    /* 这是一个简化实现，实际应该通过pubsub来管理 */
    return YQ_OK;
}

int yq_unsubscribe_all(struct yq_pubsub *pubsub) {
    if (!pubsub) {
        return YQ_OK;
    }

    pthread_mutex_lock(&pubsub->mutex);

    struct yq_internal_subscriber *sub = pubsub->subscribers;
    while (sub) {
        struct yq_internal_subscriber *next = sub->next;
        sub->running = 0;
        pthread_join(sub->thread, NULL);
        free(sub->topic_filter);
        free(sub);
        sub = next;
    }

    pubsub->subscribers = NULL;
    pthread_mutex_unlock(&pubsub->mutex);

    yq_log_debug(pubsub, "Unsubscribed all subscribers");
    return YQ_OK;
}

int yq_message_queue_get(struct yq_subscriber *sub, struct yq_message_queue **out) {
    if (!sub || !out) {
        return YQ_ERR_INVAL;
    }

    struct yq_internal_subscriber *internal_sub = (struct yq_internal_subscriber *)sub;
    *out = (struct yq_message_queue *)internal_sub->queue;
    return YQ_OK;
}

int yq_message_queue_poll(struct yq_message_queue *queue, yq_message **out, uint32_t timeout_ms) {
    if (!queue || !out) {
        return YQ_ERR_INVAL;
    }

    struct yq_internal_message_queue *internal_queue = (struct yq_internal_message_queue *)queue;
    struct yq_internal_message *msg = yq_internal_message_queue_pop(internal_queue, timeout_ms);
    
    if (!msg) {
        return YQ_ERR_NOTFOUND;
    }

    /* 转换为公共消息格式 */
    *out = malloc(sizeof(yq_message));
    if (!*out) {
        free(msg->topic);
        free(msg->payload);
        free(msg->metadata);
        free(msg);
        return YQ_ERR_NOMEM;
    }

    memset(*out, 0, sizeof(yq_message));
    (*out)->struct_size = sizeof(yq_message);
    (*out)->message_id = msg->message_id;
    (*out)->timestamp = msg->timestamp;
    (*out)->priority = msg->priority;
    (*out)->flags = msg->flags;
    (*out)->topic_len = msg->topic_len;
    (*out)->payload_len = msg->payload_len;
    (*out)->metadata_len = msg->metadata_len;
    (*out)->topic = msg->topic;
    (*out)->payload = msg->payload;
    (*out)->metadata = msg->metadata;

    return YQ_OK;
}

int yq_message_queue_poll_batch(struct yq_message_queue *queue, yq_message **messages, 
                                 size_t max_count, uint32_t timeout_ms, size_t *actual_count) {
    if (!queue || !messages || max_count == 0 || !actual_count) {
        return YQ_ERR_INVAL;
    }

    struct yq_internal_message_queue *internal_queue = (struct yq_internal_message_queue *)queue;
    *actual_count = 0;

    for (size_t i = 0; i < max_count; i++) {
        struct yq_internal_message *msg = yq_internal_message_queue_pop(internal_queue, 0);
        if (!msg) {
            break;
        }

        /* 转换为公共消息格式 */
        messages[i] = malloc(sizeof(yq_message));
        if (!messages[i]) {
            free(msg->topic);
            free(msg->payload);
            free(msg->metadata);
            free(msg);
            return YQ_ERR_NOMEM;
        }

        memset(messages[i], 0, sizeof(yq_message));
        messages[i]->struct_size = sizeof(yq_message);
        messages[i]->message_id = msg->message_id;
        messages[i]->timestamp = msg->timestamp;
        messages[i]->priority = msg->priority;
        messages[i]->flags = msg->flags;
        messages[i]->topic_len = msg->topic_len;
        messages[i]->payload_len = msg->payload_len;
        messages[i]->metadata_len = msg->metadata_len;
        messages[i]->topic = msg->topic;
        messages[i]->payload = msg->payload;
        messages[i]->metadata = msg->metadata;

        (*actual_count)++;
    }

    return YQ_OK;
}

int yq_message_ack(struct yq_message_queue *queue, const yq_message *msg) {
    if (!queue || !msg) {
        return YQ_ERR_INVAL;
    }

    /* 简化实现，实际应该从队列中移除消息 */
    return YQ_OK;
}

int yq_message_nack(struct yq_message_queue *queue, const yq_message *msg, uint32_t retry_delay_ms) {
    if (!queue || !msg) {
        return YQ_ERR_INVAL;
    }

    /* 简化实现，实际应该重新投递消息 */
    return YQ_OK;
}

int yq_message_queue_stats(struct yq_message_queue *queue, yq_queue_stats *stats) {
    if (!queue || !stats) {
        return YQ_ERR_INVAL;
    }

    memset(stats, 0, sizeof(yq_queue_stats));
    stats->struct_size = sizeof(yq_queue_stats);

    struct yq_internal_message_queue *internal_queue = (struct yq_internal_message_queue *)queue;
    pthread_mutex_lock(&internal_queue->mutex);

    stats->queue_size = internal_queue->capacity;
    stats->queue_usage = internal_queue->size;
    stats->pending_count = internal_queue->size;

    pthread_mutex_unlock(&internal_queue->mutex);

    return YQ_OK;
}

int yq_message_create(uint64_t message_id, const char *topic, const void *payload, size_t payload_len,
                      const void *metadata, size_t metadata_len, uint32_t flags,
                      uint32_t priority, yq_message **out) {
    if (!topic || !payload || payload_len == 0 || !out) {
        return YQ_ERR_INVAL;
    }

    *out = malloc(sizeof(yq_message));
    if (!*out) {
        return YQ_ERR_NOMEM;
    }

    memset(*out, 0, sizeof(yq_message));
    (*out)->struct_size = sizeof(yq_message);
    (*out)->message_id = message_id;
    (*out)->timestamp = yq_current_timestamp_ms();
    (*out)->priority = priority;
    (*out)->flags = flags;
    (*out)->topic_len = strlen(topic);
    (*out)->payload_len = payload_len;
    (*out)->metadata_len = metadata_len;
    (*out)->topic = topic;
    (*out)->payload = payload;
    (*out)->metadata = metadata;

    return YQ_OK;
}

void yq_message_free(yq_message *msg) {
    if (!msg) return;
    free(msg);
}

int yq_message_copy(const yq_message *src, yq_message **dst) {
    if (!src || !dst) {
        return YQ_ERR_INVAL;
    }

    *dst = malloc(sizeof(yq_message));
    if (!*dst) {
        return YQ_ERR_NOMEM;
    }

    memcpy(*dst, src, sizeof(yq_message));
    (*dst)->topic = src->topic;
    (*dst)->payload = src->payload;
    (*dst)->metadata = src->metadata;

    return YQ_OK;
}

int yq_message_get_payload(const yq_message *msg, void **payload, size_t *payload_len) {
    if (!msg || !payload || !payload_len) {
        return YQ_ERR_INVAL;
    }

    *payload_len = msg->payload_len;
    *payload = malloc(msg->payload_len);
    if (!*payload) {
        return YQ_ERR_NOMEM;
    }

    memcpy(*payload, msg->payload, msg->payload_len);
    return YQ_OK;
}

int yq_message_get_metadata(const yq_message *msg, void **metadata, size_t *metadata_len) {
    if (!msg || !metadata || !metadata_len) {
        return YQ_ERR_INVAL;
    }

    *metadata_len = msg->metadata_len;
    if (*metadata_len == 0) {
        *metadata = NULL;
        return YQ_OK;
    }

    *metadata = malloc(msg->metadata_len);
    if (!*metadata) {
        return YQ_ERR_NOMEM;
    }

    memcpy(*metadata, msg->metadata, msg->metadata_len);
    return YQ_OK;
}

int yq_message_set_status(yq_message *msg, uint32_t status) {
    if (!msg) {
        return YQ_ERR_INVAL;
    }

    msg->status = status;
    return YQ_OK;
}

int yq_message_get_status(const yq_message *msg, uint32_t *status) {
    if (!msg || !status) {
        return YQ_ERR_INVAL;
    }

    *status = msg->status;
    return YQ_OK;
}

int yq_pubsub_set_stats_callback(struct yq_pubsub *pubsub, yq_stats_callback callback, void *user_data) {
    if (!pubsub) {
        return YQ_ERR_INVAL;
    }

    pthread_mutex_lock(&pubsub->mutex);
    pubsub->stats_callback = callback;
    pubsub->stats_user_data = user_data;
    pthread_mutex_unlock(&pubsub->mutex);

    return YQ_OK;
}

int yq_pubsub_set_debug(struct yq_pubsub *pubsub, int enabled) {
    if (!pubsub) {
        return YQ_ERR_INVAL;
    }

    pthread_mutex_lock(&pubsub->mutex);
    pubsub->debug_enabled = enabled ? 1 : 0;
    pthread_mutex_unlock(&pubsub->mutex);

    yq_log_debug(pubsub, "Debug mode %s", enabled ? "enabled" : "disabled");
    return YQ_OK;
}

int yq_pubsub_get_debug_info(struct yq_pubsub *pubsub, char **debug_info) {
    if (!pubsub || !debug_info) {
        return YQ_ERR_INVAL;
    }

    pthread_mutex_lock(&pubsub->mutex);

    /* 生成调试信息 */
    char info[1024];
    snprintf(info, sizeof(info), 
             "Pub/Sub Debug Info:\n"
             "  Total Messages: %lu\n"
             "  Delivered Messages: %lu\n"
             "  Failed Messages: %lu\n"
             "  Active Topics: %u\n"
             "  Active Subscribers: %u\n"
             "  Total Bytes: %lu\n"
             "  Debug Mode: %s\n",
             pubsub->stats.total_messages,
             pubsub->stats.delivered_messages,
             pubsub->stats.failed_messages,
             pubsub->stats.active_topics,
             pubsub->stats.active_subscribers,
             pubsub->stats.total_bytes,
             pubsub->debug_enabled ? "Enabled" : "Disabled");

    size_t len = strlen(info) + 1;
    *debug_info = malloc(len);
    if (!*debug_info) {
        pthread_mutex_unlock(&pubsub->mutex);
        return YQ_ERR_NOMEM;
    }

    strcpy(*debug_info, info);
    pthread_mutex_unlock(&pubsub->mutex);

    return YQ_OK;
}

int yq_pubsub_cleanup_expired(struct yq_pubsub *pubsub, uint64_t max_age_ms) {
    if (!pubsub) {
        return YQ_ERR_INVAL;
    }

    pthread_mutex_lock(&pubsub->mutex);

    uint64_t cutoff_time = yq_current_timestamp_ms() - max_age_ms;
    struct yq_internal_topic *topic = pubsub->topics;

    while (topic) {
        struct yq_internal_topic *next_topic = topic->next;
        struct yq_internal_message **prev = &topic->message_list;
        struct yq_internal_message *msg = topic->message_list;

        while (msg) {
            if (msg->timestamp < cutoff_time) {
                /* 移除过期消息 */
                *prev = msg->next;
                free(msg->topic);
                free(msg->payload);
                free(msg->metadata);
                free(msg);
                topic->message_count--;
                pubsub->stats.total_messages--;
            } else {
                prev = &msg->next;
            }
            msg = msg->next;
        }

        topic = next_topic;
    }

    pthread_mutex_unlock(&pubsub->mutex);

    yq_log_debug(pubsub, "Cleaned up expired messages older than %lu ms", max_age_ms);
    return YQ_OK;
}

#endif /* YQ_ENABLE_PUBSUB */