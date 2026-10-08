/*
 * yq.h — yq-DB 核心 KV 引擎公开 C ABI
 *
 * 版本   : 1.0.0
 * 语言   : C11
 * 格式版本: 1（见 FORMAT.md）
 *
 * 设计约束（改动本头文件前请先读）：
 *   1. 本头文件是唯一的对外 ABI 契约。SQL 层、行/列编码层不得进入本文件。
 *   2. 结构体一律带 struct_size 字段，新增字段只能追加在尾部。
 *   3. 错误码数值一旦发布即固定，不得重排或复用。
 *   4. 不暴露任何内部结构，全部句柄为不透明类型。
 */

#ifndef YQ_H
#define YQ_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════
 * JSON Support (Optional)
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * JSON support is optional. To enable JSON functionality, define YQ_ENABLE_JSON
 * before including yq.h or compile with -DYQ_ENABLE_JSON.
 */
#ifndef YQ_ENABLE_JSON
#define YQ_ENABLE_JSON 0
#endif

#if YQ_ENABLE_JSON
#include "yq_json.h"
#endif

#if YQ_ENABLE_BATCH
#include "yq_batch.h"
#endif

#if YQ_ENABLE_INDEX
#include "yq_index.h"
#endif

#if YQ_ENABLE_TTL
#include "yq_ttl.h"
#endif

#if YQ_ENABLE_COMPRESS
#include "yq_compress.h"
#endif

#if YQ_ENABLE_CRYPTO
#include "yq_crypto.h"
#endif

#if YQ_ENABLE_PUBSUB
#include "yq_pubsub.h"
#endif

/* ═══════════════════════════════════════════════════════════════════════
 * 版本
 * ═══════════════════════════════════════════════════════════════════════ */

#define YQ_VERSION_MAJOR 1
#define YQ_VERSION_MINOR 0
#define YQ_VERSION_PATCH 0

/* ═══════════════════════════════════════════════════════════════════════
 * 错误码
 *
 * 0 为成功，非 0 一律失败。语义与重试策略见 ERRORS.md。
 * 所有返回 int 的函数都返回本枚举中的值。
 * ═══════════════════════════════════════════════════════════════════════ */

typedef enum yq_rc {
    YQ_OK              = 0,  /* 成功 */
    YQ_ERR             = 1,  /* 通用错误，无更具体分类 */
    YQ_ERR_NOMEM       = 2,  /* 内存分配失败 */
    YQ_ERR_IO          = 3,  /* 底层 I/O 失败，详见 yq_last_io_error() */
    YQ_ERR_CORRUPT     = 4,  /* 校验失败或格式非法；库已停止提供写能力 */
    YQ_ERR_VERSION     = 5,  /* 格式版本不兼容（库版本高于本库支持的版本） */
    YQ_ERR_NOTFOUND    = 6,  /* 键不存在 */
    YQ_ERR_EXISTS      = 7,  /* 目标已存在：NOOVERWRITE 遇已有键，或 OPEN_EXCL 遇已有文件 */
    YQ_ERR_BUSY        = 8,  /* 写锁被其他进程持有且未配置等待 */
    YQ_ERR_READONLY    = 9,  /* 只读句柄或只读事务尝试写入 */
    YQ_ERR_INVAL       = 10, /* 参数非法 */
    YQ_ERR_TOOBIG      = 11, /* 键或值超出上限（键 > 1024 字节） */
    YQ_ERR_TXN_CLOSED  = 12, /* 事务已提交或回滚 */
    YQ_ERR_TXN_BROKEN  = 13, /* 事务因并发冲突作废，需重试 */
    YQ_ERR_CURSOR      = 14, /* 游标状态非法（未定位或已越界） */
    YQ_ERR_NOSPACE     = 15, /* 磁盘空间不足 */
    YQ_ERR_MAP_FULL    = 16, /* 映射区已满，需调大 opts.map_size 后重开 */
    YQ_ERR_READER_FULL = 17, /* 读者槽位耗尽，需调大 opts.max_readers 后重开 */
    YQ_ERR_NOTSUP      = 18, /* 当前构建未启用该功能；为 SQL 等可选模块预留 */
    YQ_ERR_TIMEOUT     = 19, /* 等待写锁超时 */
    YQ_ERR_PANIC       = 20  /* 内部不变量被破坏；库进入只读保护态，必须重开进程 */
} yq_rc;

/*
 * 返回当前库版本。任一参数可为 NULL。
 */
int yq_version(int *major, int *minor, int *patch);

/*
 * 返回错误码的静态描述字符串。返回值生命周期为整个进程，调用方不得释放。
 * 未识别的码返回 "unknown error"。
 */
const char *yq_strerror(int rc);

/* ═══════════════════════════════════════════════════════════════════════
 * 基础类型
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * 字节切片。data 为 NULL 时 size 必须为 0。
 * 键与值都是不透明字节串，默认比较器为 memcmp。
 */
typedef struct yq_slice {
    const void *data;
    size_t      size;
} yq_slice;

/* 不透明句柄 */
typedef struct yq_db  yq_db;
typedef struct yq_txn yq_txn;
typedef struct yq_cur yq_cur;

#if YQ_ENABLE_PUBSUB
#include "yq_pubsub.h"
#endif

/* ═══════════════════════════════════════════════════════════════════════
 * 打开参数
 * ═══════════════════════════════════════════════════════════════════════ */

/* opts.flags */
#define YQ_OPEN_READONLY  0x0001u /* 只读打开：不修改主库文件；仍会写 yq.shm 以登记读者槽位 */
#define YQ_OPEN_CREATE    0x0002u /* 不存在则创建 */
#define YQ_OPEN_EXCL      0x0004u /* 与 CREATE 同用，已存在则返回 YQ_ERR_EXISTS */
#define YQ_OPEN_NOSYNC    0x0008u /* 忽略所有持久化等待，等效于 YQ_SYNC_OFF，仅用于可丢弃数据 */
#define YQ_OPEN_IMMUTABLE 0x0010u /*
                                   * 跳过所有锁与读者登记，假定无其他进程在写。
                                   * 用于只读介质或单进程离线分析。
                                   * 这是调用方的承诺：承诺期内若有其他进程写入，
                                   * 将导致数据损坏，责任在调用方。
                                   */

/*
 * opts.sync_mode：持久化强度。
 * 注意 YQ_SYNC_OFF 从 1 开始而非 0——0 保留给"用默认值"，
 * 否则调用方无法在零初始化的 opts 中区分"未设置"与"显式关闭持久化"。
 */
#define YQ_SYNC_DEFAULT 0u /* 未设置；库按 YQ_SYNC_NORMAL 处理 */
#define YQ_SYNC_OFF     1u /* 不等待落盘，崩溃可能丢失最近若干次提交 */
#define YQ_SYNC_NORMAL  2u /* 默认。组提交，多事务合并为一次 fdatasync */
#define YQ_SYNC_FULL    3u /* 每次提交都 fdatasync 后才返回 */

/*
 * 打开参数。struct_size 必须填 sizeof(yq_opts)。
 * 除 struct_size 外，所有字段填 0 表示使用默认值。
 */
typedef struct yq_opts {
    uint32_t struct_size;      /* 必须 = sizeof(yq_opts) */
    uint32_t flags;            /* YQ_OPEN_* 位或；0 = 无标志 */
    uint32_t page_size;        /* 0 = 4096；必须是 4096..65536 的 2 的幂 */
    uint32_t sync_mode;        /* YQ_SYNC_*；0 视作 YQ_SYNC_NORMAL */
    uint64_t map_size;         /* 0 = 1 GiB；mmap 映射区大小 */
    uint64_t memtable_bytes;   /* 0 = 64 MiB；内存表 checkpoint 阈值 */
    uint64_t log_bytes;        /* 0 = 256 MiB；日志 checkpoint 阈值 */
    uint32_t max_readers;      /* 0 = 126；并发读者槽位数，硬上限 65535 */
    uint32_t lock_timeout_ms;  /* 0 = 不等待；>0 表示等待写锁的毫秒数 */
    uint32_t reserved[8];      /* 必须为 0 */
} yq_opts;

/* ═══════════════════════════════════════════════════════════════════════
 * 统计信息
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct yq_stat {
    uint32_t struct_size;      /* 必须 = sizeof(yq_stat) */
    uint32_t format_version;   /* 磁盘格式版本 */
    uint64_t txn_id;           /* 最新已提交事务号 */
    uint64_t npages;           /* 文件总页数 */
    uint64_t page_size;        /* 页大小 */
    uint64_t free_pages;       /* 空闲页数 */
    uint64_t log_bytes;        /* 当前日志文件字节数 */
    uint64_t memtable_bytes;   /* 当前内存表占用字节数 */
    uint32_t active_readers;   /* 当前活跃读者数 */
    uint32_t max_readers;      /* 读者槽位总数 */
    uint32_t reserved[4];      /* 必须为 0 */
} yq_stat;

/* ═══════════════════════════════════════════════════════════════════════
 * 生命周期
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * 打开数据库。成功时 *out 写入句柄，失败时 *out 置 NULL。
 *
 * path 为库文件路径。同目录下会使用三个附属文件：
 *   path + ".log"   追加写日志
 *   path + ".shm"   读者槽位表（非持久化，可安全删除）
 *   path + ".lock"  跨进程写者互斥
 *
 * 打开过程会执行崩溃恢复（见 FORMAT.md §7）。恢复期间持有写锁。
 */
int yq_open(const char *path, const yq_opts *opts, yq_db **out);

/*
 * 关闭数据库。会依次：回滚所有活跃事务、强制 checkpoint、fdatasync、释放映射。
 * 可传入 NULL，直接返回 YQ_OK。
 *
 * 注意：本函数不等待其他进程的读者退出。其他进程仍持有快照时，
 * checkpoint 可能被跳过，此时返回 YQ_OK 但日志未被截断（属正常行为）。
 */
int yq_close(yq_db *db);

/*
 * 返回最近一次 I/O 错误的系统错误码（errno / GetLastError），无错误返回 0。
 * 线程局部，随最近一次失败操作更新。
 */
int yq_last_io_error(void);

/* ═══════════════════════════════════════════════════════════════════════
 * 事务
 * ═══════════════════════════════════════════════════════════════════════ */

/* txn_begin 的 flags */
#define YQ_TXN_READONLY  0x0000u /* 只读事务，获取 MVCC 快照，绝不阻塞写者 */
#define YQ_TXN_READWRITE 0x0001u /* 读写事务，需要写锁；拿不到返回 YQ_ERR_BUSY */

/*
 * 开启事务。
 *
 * 只读事务：原子获取 (txn_id, root_page) 快照并登记读者槽位，无锁，极快。
 * 读写事务：先选举写者（单写者），再登记快照。
 *
 * 同一线程同一时刻只允许一个活跃读写事务。只读事务可嵌套。
 */
int yq_txn_begin(yq_db *db, uint32_t flags, yq_txn **out);

/*
 * 提交事务。写事务在返回前按 sync_mode 决定是否等待落盘
 * （YQ_SYNC_NORMAL 下参与组提交，可能与其他线程共享一次 fdatasync）。
 *
 * 提交后 txn 句柄失效，再次使用返回 YQ_ERR_TXN_CLOSED。只读事务提交等价于释放快照。
 */
int yq_txn_commit(yq_txn *txn);

/*
 * 回滚事务。等同于丢弃所有变更并释放快照。
 * 可对已提交/已回滚的事务调用，返回 YQ_ERR_TXN_CLOSED。
 */
int yq_txn_abort(yq_txn *txn);

/* ═══════════════════════════════════════════════════════════════════════
 * 读写
 * ═══════════════════════════════════════════════════════════════════════ */

/* yq_put 的 mode */
#define YQ_PUT_UPSERT      0u /* 存在则覆盖 */
#define YQ_PUT_NOOVERWRITE 1u /* 已存在则返回 YQ_ERR_EXISTS */

/*
 * 写入。约束：key.size <= 1024，val.size <= 1 GiB（见 FORMAT.md 附录 C）。
 * 只读事务调用返回 YQ_ERR_READONLY。
 *
 * 变更先进入内存表，提交时才可能落盘。
 */
int yq_put(yq_txn *txn, yq_slice key, yq_slice val, uint32_t mode);

/*
 * 删除。键不存在返回 YQ_OK（幂等），不返回 YQ_ERR_NOTFOUND。
 */
int yq_del(yq_txn *txn, yq_slice key);

/*
 * 读取。命中时 *out 指向 mmap 内的数据，**不拷贝**。
 *
 * 借用指针有效期：
 *   - 同一事务内，只读事务：直到事务提交或回滚。
 *   - 同一事务内，读写事务：直到该事务内发生任何一次 yq_put / yq_del。
 *   - 跨事务、跨线程一律无效。
 *
 * 需要长期持有请自行 memcpy。未命中返回 YQ_ERR_NOTFOUND，*out 置 {NULL, 0}。
 */
int yq_get(yq_txn *txn, yq_slice key, yq_slice *out);

/* ═══════════════════════════════════════════════════════════════════════
 * 游标
 *
 * 游标绑定单个事务，按 memcmp 键序双向遍历。游标打开时定位到"无效"状态，
 * 必须先调用某个 seek/first/last 才能取值。所有取到的 key/val 都是借用指针，
 * 有效期到下一次游标移动或事务结束。
 * ═══════════════════════════════════════════════════════════════════════ */

/* 打开游标。事务结束时未关闭的游标会自动失效。 */
int yq_cur_open(yq_txn *txn, yq_cur **out);

/* 定位到第一个 key >= 目标键的记录；若不存在，游标无效并返回 YQ_ERR_NOTFOUND。 */
int yq_cur_seek(yq_cur *c, yq_slice key);

/* 定位到 key <= 目标键的最后一条记录；若不存在，游标无效并返回 YQ_ERR_NOTFOUND。 */
int yq_cur_seek_le(yq_cur *c, yq_slice key);

/* 精确定位；不存在则无效并返回 YQ_ERR_NOTFOUND。 */
int yq_cur_seek_exact(yq_cur *c, yq_slice key);

int yq_cur_first(yq_cur *c);
int yq_cur_last(yq_cur *c);

/* 前进/后退一条。越界返回 YQ_ERR_NOTFOUND，游标置为无效。 */
int yq_cur_next(yq_cur *c);
int yq_cur_prev(yq_cur *c);

/* 游标是否指向有效记录。有效返回 1，无效返回 0。 */
int yq_cur_valid(const yq_cur *c);

/* 取当前键/值（借用指针）。游标无效时返回 YQ_ERR_CURSOR。 */
int yq_cur_key(const yq_cur *c, yq_slice *out);
int yq_cur_val(const yq_cur *c, yq_slice *out);

/* 关闭游标。可传 NULL。 */
int yq_cur_close(yq_cur *c);

/* ═══════════════════════════════════════════════════════════════════════
 * 维护
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * 立即执行 checkpoint：把内存表合并进 B+Tree 并截断日志（见 FORMAT.md §9）。
 *
 * 阻塞调用。期间其他线程的读写仍可继续，只有最后发布新 root_page 是原子的。
 * 无法获取写锁时返回 YQ_ERR_BUSY。
 */
int yq_checkpoint(yq_db *db);

/*
 * 把当前内存表与日志刷到稳定存储。不清空内存表，不做页整理。
 * YQ_SYNC_OFF 模式下这是唯一的手动持久化手段。
 */
int yq_sync(yq_db *db);

/* 读取统计信息。struct_size 必须先填 sizeof(yq_stat)。 */
int yq_db_stat(yq_db *db, yq_stat *out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* YQ_H */
