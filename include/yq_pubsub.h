/*
 * yq_pubsub.h — yq-DB Publish/Subscribe messaging API
 *
 * Version : 1.0.0
 * Language: C11
 * Format version: 1（见 FORMAT.md）
 *
 * Design constraints（改动本头文件前请先读）：
 *   1. 本头文件是唯一的对外 ABI 契约。SQL 层、行/列编码层不得进入本文件。
 *   2. 结构体一律带 struct_size 字段，新增字段只能追加在尾部。
 *   3. 错误码数值一旦发布即固定，不得重排或复用。
 *   4. 不暴露任何内部结构，全部句柄为不透明类型。
 */

#ifndef YQ_PUBSUB_H
#define YQ_PUBSUB_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Pub/sub support is optional. To enable pub/sub functionality, define YQ_ENABLE_PUBSUB
 * before including yq.h or compile with -DYQ_ENABLE_PUBSUB.
 */

#if YQ_ENABLE_PUBSUB

/* ═══════════════════════════════════════════════════════════════════════
 * 错误码扩展
 * ═══════════════════════════════════════════════════════════════════════ */

/* Basic error codes (from main yq.h) */
#define YQ_OK              0   /* 成功 */
#define YQ_ERR             1   /* 通用错误 */
#define YQ_ERR_NOMEM       2   /* 内存分配失败 */
#define YQ_ERR_INVAL       3   /* 无效参数 */
#define YQ_ERR_NOTFOUND    4   /* 未找到 */
#define YQ_ERR_EXISTS      5   /* 已存在 */

typedef enum yq_pubsub_rc {
    YQ_PUBSUB_OK               = 0,  /* 成功 */
    YQ_PUBSUB_ERR             = 100, /* 通用错误 */
    YQ_PUBSUB_ERR_TOPIC       = 101, /* 主题相关错误 */
    YQ_PUBSUB_ERR_SUBSCRIBER  = 102, /* 订阅者相关错误 */
    YQ_PUBSUB_ERR_MESSAGE     = 103, /* 消息相关错误 */
    YQ_PUBSUB_ERR_CALLBACK    = 104, /* 回调相关错误 */
    YQ_PUBSUB_ERR_QUEUE       = 105, /* 消息队列错误 */
    YQ_PUBSUB_ERR_OVERFLOW    = 106, /* 消息队列溢出 */
    YQ_PUBSUB_ERR_TIMEOUT     = 107, /* 操作超时 */
    YQ_PUBSUB_ERR_DUPLICATE   = 108, /* 重复订阅 */
    YQ_PUBSUB_ERR_NOTSUBSCRIBED = 109 /* 未订阅 */
} yq_pubsub_rc;

/* ═══════════════════════════════════════════════════════════════════════
 * 消息优先级
 * ═══════════════════════════════════════════════════════════════════════ */

typedef enum yq_message_priority {
    YQ_MESSAGE_LOW    = 0,  /* 低优先级，最后处理 */
    YQ_MESSAGE_NORMAL = 1,  /* 正常优先级，默认 */
    YQ_MESSAGE_HIGH   = 2,  /* 高优先级，优先处理 */
    YQ_MESSAGE_URGENT = 3   /* 紧急优先级，立即处理 */
} yq_message_priority;

/* ═══════════════════════════════════════════════════════════════════════
 * 消息标志
 * ═══════════════════════════════════════════════════════════════════════ */

#define YQ_MESSAGE_FLAG_PERSISTENT  0x0001u /* 持久化消息，不随数据库关闭丢失 */
#define YQ_MESSAGE_FLAG_BROADCAST   0x0002u /* 广播消息，所有订阅者都收到 */
#define YQ_MESSAGE_FLAG_FILTERED     0x0004u /* 需要过滤的消息 */
#define YQ_MESSAGE_FLAG_COMPRESSED   0x0008u /* 压缩的消息 */
#define YQ_MESSAGE_FLAG_ENCRYPTED    0x0010u /* 加密的消息 */

/* ═══════════════════════════════════════════════════════════════════════
 * 消息结构
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct yq_message {
    uint32_t struct_size;      /* 必须 = sizeof(yq_message) */
    uint64_t message_id;      /* 消息唯一ID */
    uint64_t timestamp;       /* 消息时间戳（毫秒） */
    uint32_t priority;        /* yq_message_priority */
    uint32_t flags;           /* YQ_MESSAGE_FLAG_* 位或 */
    uint32_t topic_len;       /* 主题字符串长度 */
    uint32_t payload_len;     /* 消息负载长度 */
    uint32_t metadata_len;    /* 元数据长度 */
    uint32_t status;          /* 消息状态 */
    uint32_t reserved[3];      /* 必须为 0 */
    
    /* 注意：以下字段是指针，实际数据需要通过专门的访问函数获取 */
    const char *topic;        /* 主题字符串 */
    const void *payload;      /* 消息负载 */
    const void *metadata;     /* 消息元数据 */
} yq_message;

/* ═══════════════════════════════════════════════════════════════════════
 * 订阅者配置
 * ═══════════════════════════════════════════════════════════════════════ */

typedef enum yq_delivery_mode {
    YQ_DELIVERY_AT_MOST_ONCE = 0, /* 最多一次，可能丢失 */
    YQ_DELIVERY_AT_LEAST_ONCE = 1, /* 至少一次，可能重复 */
    YQ_DELIVERY_EXACTLY_ONCE = 2  /* 恰好一次，不丢失不重复 */
} yq_delivery_mode;

typedef struct yq_subscriber_opts {
    uint32_t struct_size;      /* 必须 = sizeof(yq_subscriber_opts) */
    uint32_t queue_size;       /* 消息队列大小，0 = 默认（1000） */
    uint32_t max_retry;        /* 最大重试次数，0 = 默认（3） */
    uint32_t retry_delay_ms;   /* 重试延迟毫秒，0 = 默认（1000） */
    uint32_t timeout_ms;       /* 消息处理超时，0 = 无限 */
    yq_delivery_mode delivery_mode; /* 投递模式 */
    uint32_t reserved[8];      /* 必须为 0 */
} yq_subscriber_opts;

/* ═══════════════════════════════════════════════════════════════════════
 * 主题过滤器
 * ═══════════════════════════════════════════════════════════════════════ */

typedef enum yq_filter_type {
    YQ_FILTER_NONE = 0,       /* 不过滤 */
    YQ_FILTER_PREFIX = 1,     /* 前缀过滤 */
    YQ_FILTER_REGEX = 2,      /* 正则表达式过滤 */
    YQ_FILTER_CUSTOM = 3     /* 自定义过滤 */
} yq_filter_type;

typedef struct yq_topic_filter {
    uint32_t struct_size;      /* 必须 = sizeof(yq_topic_filter) */
    yq_filter_type type;       /* 过滤器类型 */
    uint32_t pattern_len;      /* 模式字符串长度 */
    uint32_t reserved[7];      /* 必须为 0 */
    const char *pattern;       /* 过滤模式 */
} yq_topic_filter;

/* ═══════════════════════════════════════════════════════════════════════
 * 不透明句柄
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct yq_pubsub yq_pubsub;
typedef struct yq_topic yq_topic;
typedef struct yq_subscriber yq_subscriber;
typedef struct yq_message yq_message;
typedef struct yq_message_queue yq_message_queue;

/* ═══════════════════════════════════════════════════════════════════════
 * 回调函数类型
 * ═══════════════════════════════════════════════════════════════════════ */

typedef int (*yq_message_callback)(yq_subscriber *sub, const yq_message *msg, void *user_data);
typedef int (*yq_error_callback)(yq_subscriber *sub, int error_code, const char *error_msg, void *user_data);
typedef void (*yq_stats_callback)(yq_pubsub *pubsub, void *user_data);

/* ═══════════════════════════════════════════════════════════════════════
 * Pub/Sub 管理
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * 初始化 Pub/Sub 系统
 */
int yq_pubsub_init(void *db, yq_pubsub **out);

/*
 * 关闭 Pub/Sub 系统
 */
int yq_pubsub_close(yq_pubsub *pubsub);

/*
 * 获取 Pub/Sub 统计信息
 */
typedef struct yq_pubsub_stats {
    uint32_t struct_size;      /* 必须 = sizeof(yq_pubsub_stats) */
    uint64_t total_messages;   /* 总消息数 */
    uint64_t delivered_messages; /* 已投递消息数 */
    uint64_t failed_messages;  /* 投递失败消息数 */
    uint32_t active_topics;    /* 活跃主题数 */
    uint32_t active_subscribers; /* 活跃订阅者数 */
    uint32_t queue_usage;     /* 队列使用量 */
    uint32_t max_queue_size;   /* 最大队列大小 */
    uint64_t total_bytes;      /* 总字节数 */
    uint32_t reserved[8];      /* 必须为 0 */
} yq_pubsub_stats;

int yq_pubsub_get_stats(yq_pubsub *pubsub, yq_pubsub_stats *stats);

/* ═══════════════════════════════════════════════════════════════════════
 * 主题管理
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * 创建主题
 */
int yq_topic_create(yq_pubsub *pubsub, const char *name, yq_topic **out);

/*
 * 删除主题
 */
int yq_topic_delete(yq_pubsub *pubsub, const char *name);

/*
 * 获取主题
 */
int yq_topic_get(yq_pubsub *pubsub, const char *name, yq_topic **out);

/*
 * 列出所有主题
 */
int yq_topic_list(yq_pubsub *pubsub, char ***topics, uint32_t *count);

/*
 * 主题统计信息
 */
typedef struct yq_topic_stats {
    uint32_t struct_size;      /* 必须 = sizeof(yq_topic_stats) */
    uint64_t message_count;   /* 消息数量 */
    uint64_t subscriber_count; /* 订阅者数量 */
    uint64_t total_bytes;     /* 总字节数 */
    uint32_t message_rate;     /* 消息速率（每秒） */
    uint32_t reserved[8];      /* 必须为 0 */
} yq_topic_stats;

int yq_topic_get_stats(yq_topic *topic, yq_topic_stats *stats);

/* ═══════════════════════════════════════════════════════════════════════
 * 消息发布
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * 发布消息到主题
 */
int yq_publish(yq_pubsub *pubsub, const char *topic, const void *payload, size_t payload_len, 
               uint32_t flags, uint64_t *message_id);

/*
 * 发布带元数据的消息
 */
int yq_publish_ex(yq_pubsub *pubsub, const char *topic, const void *payload, size_t payload_len,
                  const void *metadata, size_t metadata_len, uint32_t flags, 
                  yq_message_priority priority, uint64_t *message_id);

/*
 * 批量发布消息
 */
int yq_publish_batch(yq_pubsub *pubsub, const yq_message **messages, size_t count, 
                      uint64_t *message_ids);

/* ═══════════════════════════════════════════════════════════════════════
 * 订阅管理
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * 订阅主题
 */
int yq_subscribe(yq_pubsub *pubsub, const char *topic, const yq_subscriber_opts *opts,
                 yq_message_callback message_cb, yq_error_callback error_cb,
                 void *user_data, yq_subscriber **out);

/*
 * 订阅带过滤器的主题
 */
int yq_subscribe_filtered(yq_pubsub *pubsub, const yq_topic_filter *filter,
                          const yq_subscriber_opts *opts,
                          yq_message_callback message_cb, yq_error_callback error_cb,
                          void *user_data, yq_subscriber **out);

/*
 * 取消订阅
 */
int yq_unsubscribe(yq_subscriber *sub);

/*
 * 取消订阅所有主题
 */
int yq_unsubscribe_all(yq_pubsub *pubsub);

/* ═══════════════════════════════════════════════════════════════════════
 * 消息队列管理
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * 获取订阅者的消息队列
 */
int yq_message_queue_get(yq_subscriber *sub, yq_message_queue **out);

/*
 * 从队列中获取消息
 */
int yq_message_queue_poll(yq_message_queue *queue, yq_message **out, uint32_t timeout_ms);

/*
 * 从队列中批量获取消息
 */
int yq_message_queue_poll_batch(yq_message_queue *queue, yq_message **messages, 
                                 size_t max_count, uint32_t timeout_ms, size_t *actual_count);

/*
 * 确认消息处理完成
 */
int yq_message_ack(yq_message_queue *queue, const yq_message *msg);

/*
 * 拒绝消息（重新投递）
 */
int yq_message_nack(yq_message_queue *queue, const yq_message *msg, uint32_t retry_delay_ms);

/*
 * 队列统计信息
 */
typedef struct yq_queue_stats {
    uint32_t struct_size;      /* 必须 = sizeof(yq_queue_stats) */
    uint32_t queue_size;      /* 队列大小 */
    uint32_t queue_usage;     /* 当前使用量 */
    uint32_t pending_count;   /* 待处理消息数 */
    uint32_t processed_count; /* 已处理消息数 */
    uint32_t failed_count;    /* 失败消息数 */
    uint32_t avg_process_ms;  /* 平均处理时间（毫秒） */
    uint32_t reserved[8];      /* 必须为 0 */
} yq_queue_stats;

int yq_message_queue_stats(yq_message_queue *queue, yq_queue_stats *stats);

/* ═══════════════════════════════════════════════════════════════════════
 * 消息操作
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * 创建消息对象
 */
int yq_message_create(uint64_t message_id, const char *topic, const void *payload, size_t payload_len,
                      const void *metadata, size_t metadata_len, uint32_t flags,
                      yq_message_priority priority, yq_message **out);

/*
 * 释放消息对象
 */
void yq_message_free(yq_message *msg);

/*
 * 复制消息
 */
int yq_message_copy(const yq_message *src, yq_message **dst);

/*
 * 获取消息负载的副本
 */
int yq_message_get_payload(const yq_message *msg, void **payload, size_t *payload_len);

/*
 * 获取消息元数据的副本
 */
int yq_message_get_metadata(const yq_message *msg, void **metadata, size_t *metadata_len);

/*
 * 设置消息处理状态
 */
int yq_message_set_status(yq_message *msg, uint32_t status);

/*
 * 获取消息处理状态
 */
int yq_message_get_status(const yq_message *msg, uint32_t *status);

/* ═══════════════════════════════════════════════════════════════════════
 * 监控和调试
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * 设置统计回调
 */
int yq_pubsub_set_stats_callback(yq_pubsub *pubsub, yq_stats_callback callback, void *user_data);

/*
 * 启用/禁用调试模式
 */
int yq_pubsub_set_debug(yq_pubsub *pubsub, int enabled);

/*
 * 获取调试信息
 */
int yq_pubsub_get_debug_info(yq_pubsub *pubsub, char **debug_info);

/*
 * 清理过期消息
 */
int yq_pubsub_cleanup_expired(yq_pubsub *pubsub, uint64_t max_age_ms);

#endif /* YQ_ENABLE_PUBSUB */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* YQ_PUBSUB_H */