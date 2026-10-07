# yq-DB 使用文档

面向实践、以任务为导向的指南，帮助你在 C 应用中嵌入 yq-DB。

> [English](USAGE.md) · **简体中文**
> 另见：[README](../README.zh-CN.md) · [FORMAT](../FORMAT.md) · [ERRORS](../ERRORS.md) · [yq.h](../include/yq.h)

## 目录

- [1. 链接库](#1-链接库)
- [2. 打开数据库](#2-打开数据库)
- [3. 基础读写](#3-基础读写)
- [4. 事务与持久性](#4-事务与持久性)
- [5. 处理借用指针](#5-处理借用指针)
- [6. 游标与遍历](#6-游标与遍历)
- [7. 并发](#7-并发)
- [8. 维护：checkpoint、sync、统计](#8-维护checkpointsync统计)
- [9. 错误处理模式](#9-错误处理模式)
- [10. 调优](#10-调优)
- [11. 崩溃恢复与备份](#11-崩溃恢复与备份)
- [12. 完整示例](#12-完整示例)
- [13. 故障排查](#13-故障排查)

---

## 1. 链接库

先构建静态库：

```sh
gcc -std=c11 -Wall -Wextra -Iinclude -O2 -c src/*.c
ar rcs libyqdb.a *.o
```

再针对库编译你的应用：

```sh
gcc -std=c11 -I path/to/yqdb/include your_app.c path/to/yqdb/libyqdb.a -o your_app
```

只需包含 `yq.h`。所有句柄都是不透明类型，切勿对其做 `sizeof` 或解引用。

```c
#include "yq.h"
```

---

## 2. 打开数据库

务必先零初始化 `yq_opts` 并填入 `struct_size`。其余字段保持 `0` 即选择库默认值。

```c
yq_opts opts;
memset(&opts, 0, sizeof(opts));
opts.struct_size = sizeof(yq_opts);

yq_db *db = NULL;
int rc = yq_open("app.yqdb", &opts, &db);
if (rc != YQ_OK) {
    fprintf(stderr, "open failed: %s\n", yq_strerror(rc));
    return rc;
}
```

### 打开标志

| 标志 | 效果 |
|------|------|
| `YQ_OPEN_CREATE` | 文件不存在则创建 |
| `YQ_OPEN_EXCL` | 与 `CREATE` 同用；已存在则返回 `YQ_ERR_EXISTS` |
| `YQ_OPEN_READONLY` | 不修改主库文件（仍会写 `path.shm` 以登记读者槽位） |
| `YQ_OPEN_NOSYNC` | 忽略所有持久化等待（仅用于可丢弃数据） |
| `YQ_OPEN_IMMUTABLE` | 跳过锁与读者登记；调用方承诺无并发写者 |

> `YQ_OPEN_IMMUTABLE` 是**承诺**，不是便利开关。若你持有不可变句柄期间有其他进程写入，
> 数据损坏的责任在调用方。仅用于只读介质或单进程离线分析。

典型组合：

```c
/* 不存在则创建，读写 */
opts.flags = YQ_OPEN_CREATE;

/* 只读打开已存在的库 */
opts.flags = YQ_OPEN_READONLY;

/* 仅在新建时成功，否则失败 */
opts.flags = YQ_OPEN_CREATE | YQ_OPEN_EXCL;
```

### 关闭

```c
yq_close(db);   /* 允许传 NULL，返回 YQ_OK */
```

`yq_close` 会回滚所有活跃事务、执行 checkpoint、同步并解除映射。它**不等待**其他进程
的读者退出；若其他进程仍持有快照，checkpoint 可能被跳过、日志不截断 —— 这是正常行为，
依旧返回 `YQ_OK`。

---

## 3. 基础读写

所有写操作都必须在读写事务内进行。

```c
yq_txn *txn = NULL;
yq_txn_begin(db, YQ_TXN_READWRITE, &txn);

yq_slice k = { "user:1001", 9 };
yq_slice v = { "Ada", 3 };

int rc = yq_put(txn, k, v, YQ_PUT_UPSERT);   /* 插入或覆盖 */
if (rc != YQ_OK) {
    yq_txn_abort(txn);
    return rc;
}

yq_txn_commit(txn);
```

### 覆盖写 vs. 禁覆盖

| 模式 | 键已存在时的行为 |
|------|------------------|
| `YQ_PUT_UPSERT` | 覆盖 |
| `YQ_PUT_NOOVERWRITE` | 返回 `YQ_ERR_EXISTS`，不改动 |

### 删除

```c
yq_txn_begin(db, YQ_TXN_READWRITE, &txn);
yq_del(txn, k);          /* 幂等：删除不存在的键返回 YQ_OK */
yq_txn_commit(txn);
```

### 点查

```c
yq_txn_begin(db, YQ_TXN_READONLY, &txn);

yq_slice out = { 0 };
rc = yq_get(txn, k, &out);
if (rc == YQ_OK) {
    /* out.data 指向 mmap，当前 out.size 字节有效 */
} else if (rc == YQ_ERR_NOTFOUND) {
    /* 正常的"不存在"分支，不是错误 */
}

yq_txn_commit(txn);
```

### 尺寸上限

| 项目 | 上限 |
|------|------|
| 键 | `key.size <= 1024` 字节 |
| 值 | `val.size <= 1 GiB` |

超出上限返回 `YQ_ERR_TOOBIG`。

---

## 4. 事务与持久性

### 只读 vs. 读写

```c
yq_txn_begin(db, YQ_TXN_READONLY,  &rtxn);  /* 快照，绝不阻塞写者 */
yq_txn_begin(db, YQ_TXN_READWRITE, &wtxn);  /* 需要写锁 */
```

当其他进程持有写锁且 `lock_timeout_ms` 为 `0` 时，读写 `begin` 返回 `YQ_ERR_BUSY`。

### 提交与回滚

```c
yq_txn_commit(txn);   /* 按 sync_mode 决定持久性 */
yq_txn_abort(txn);    /* 丢弃全部变更并释放快照 */
```

两者执行后句柄即失效，再次使用返回 `YQ_ERR_TXN_CLOSED`。提交只读事务等价于释放快照。

### 同步模式

在打开时通过 `opts.sync_mode` 设置，它决定提交时等待数据落盘的强度。

| 模式 | 含义 | 崩溃暴露 |
|------|------|----------|
| `YQ_SYNC_OFF` | 不等待磁盘 | 最近若干次提交可能丢失 |
| `YQ_SYNC_NORMAL` | 组提交（默认） | 持久；多个事务可共享一次刷盘 |
| `YQ_SYNC_FULL` | 每次提交都刷盘后才返回 | 持久，最慢 |

若以 `YQ_SYNC_OFF` 打开，`yq_sync` 就是你手动建立持久性点的手段。

### 推荐的写入模式

```c
yq_txn *txn = NULL;
if (yq_txn_begin(db, YQ_TXN_READWRITE, &txn) != YQ_OK) {
    return -1;
}
/* ... 一次或多次 yq_put / yq_del ... */
if (yq_txn_commit(txn) != YQ_OK) {
    /* 提交失败：变更未生效 */
    return -1;
}
```

把相关写入合并进一个事务 —— 既更快又具原子性。

---

## 5. 处理借用指针

`yq_get` 与游标取数返回指向内部内存的**借用**指针，仅在规定窗口内有效：

| 场景 | 有效期至 |
|------|----------|
| 只读事务 | 该事务提交或回滚 |
| 读写事务 | 同一事务内下一次 `yq_put` / `yq_del` |
| 游标 | 下一次游标移动或事务结束 |
| 跨事务、跨线程 | 一律无效 |

需要保留字节就复制：

```c
yq_slice out;
if (yq_get(txn, k, &out) == YQ_OK) {
    char *copy = malloc(out.size);
    memcpy(copy, out.data, out.size);
    /* 事务结束后仍可使用 copy，最后 free(copy) */
}
```

---

## 6. 游标与遍历

游标绑定单个事务，按 `memcmp` 键序遍历。新建的游标处于*无效*状态，必须先定位。

```c
yq_cur *cur = NULL;
yq_cur_open(txn, &cur);

for (int rc = yq_cur_first(cur); rc == YQ_OK; rc = yq_cur_next(cur)) {
    yq_slice ck, cv;
    yq_cur_key(cur, &ck);
    yq_cur_val(cur, &cv);
    printf("%.*s = %.*s\n",
           (int)ck.size, (const char *)ck.data,
           (int)cv.size, (const char *)cv.data);
}

yq_cur_close(cur);
```

### 定位调用

| 调用 | 定位到 |
|------|--------|
| `yq_cur_first` / `yq_cur_last` | 首 / 末条记录 |
| `yq_cur_seek(cur, key)` | 第一条 `key >= 目标键` 的记录 |
| `yq_cur_seek_le(cur, key)` | 最后一条 `key <= 目标键` 的记录 |
| `yq_cur_seek_exact(cur, key)` | 与 `key` 相等的记录 |

无匹配记录时游标置为无效并返回 `YQ_ERR_NOTFOUND` —— 这是正常情况，不是失败。

### 范围扫描

```c
yq_slice lo = { "user:", 5 };
yq_slice hi = { "user;", 5 };   /* ';' 是 ':' 的后继字符 */

yq_cur *cur = NULL;
yq_cur_open(txn, &cur);
for (int rc = yq_cur_seek(cur, lo); rc == YQ_OK; rc = yq_cur_next(cur)) {
    yq_slice ck;
    yq_cur_key(cur, &ck);
    if (ck.size >= hi.size && memcmp(ck.data, hi.data, hi.size) >= 0) break;
    /* 处理记录 */
}
yq_cur_close(cur);
```

### 判断有效性

```c
if (yq_cur_valid(cur)) {
    yq_slice key;
    yq_cur_key(cur, &key);
}
```

对无效游标调用 `yq_cur_key` / `yq_cur_val` 返回 `YQ_ERR_CURSOR`。

---

## 7. 并发

yq-DB 采用**单写多读**模型。

- **读者**无锁获取一致快照，绝不阻塞写者。读者数量上限为 `max_readers`（默认 126）；
  槽位耗尽返回 `YQ_ERR_READER_FULL`，需以更大值重开。
- **写者**串行化。同一进程内，每线程同一时刻只允许一个读写事务；跨进程由 `path.lock`
  保证单写者。

### 等待而非失败

默认情况下第二个写者立即得到 `YQ_ERR_BUSY`。要改为等待：

```c
opts.lock_timeout_ms = 5000;   /* 最多等待 5 秒获取写锁 */
```

超时后返回 `YQ_ERR_TIMEOUT`。

### 长读快照

长时间存活的只读事务会钉住其快照，可能延迟回收。请让读事务尽可能短。

---

## 8. 维护：checkpoint、sync、统计

```c
yq_checkpoint(db);        /* 内存表合并进 B+Tree，截断日志 */
yq_sync(db);              /* 将内存表 + 日志刷到稳定存储 */
```

- 长运行写入场景应定期调用 `yq_checkpoint` 以限制日志增长。它是阻塞调用，写锁被占用时
  返回 `YQ_ERR_BUSY`。
- `yq_sync` 只刷盘、不做页整理；在 `YQ_SYNC_OFF` 模式下它是唯一的手动持久化手段。

### 读取统计信息

```c
yq_stat st;
memset(&st, 0, sizeof(st));
st.struct_size = sizeof(yq_stat);
yq_db_stat(db, &st);

printf("format=%u txn=%llu pages=%llu log=%llu mem=%llu readers=%u/%u\n",
       st.format_version,
       (unsigned long long)st.txn_id,
       (unsigned long long)st.npages,
       (unsigned long long)st.log_bytes,
       (unsigned long long)st.memtable_bytes,
       st.active_readers, st.max_readers);
```

可依据 `log_bytes` 与 `memtable_bytes` 决定何时 checkpoint。

---

## 9. 错误处理模式

`0`（`YQ_OK`）表示成功，其余皆失败。切勿宽松地判断 `YQ_OK` —— 要显式比较。

### 正常、非错误的结果

- `yq_get` / 游标 seek 返回的 `YQ_ERR_NOTFOUND` —— 普通的"不存在"分支。
- `YQ_PUT_NOOVERWRITE` 返回的 `YQ_ERR_EXISTS` —— 预期中的冲突。

不要把这两种当作故障打日志。

### 可重试错误

`YQ_ERR_BUSY`、`YQ_ERR_TIMEOUT`、`YQ_ERR_NOMEM`、`YQ_ERR_IO`、`YQ_ERR_NOSPACE`
可通过带退避的重试成功。更好的做法是设置 `lock_timeout_ms`，让库内部等待写锁，而非
应用层轮询。

```c
for (int attempt = 0; attempt < 5; ++attempt) {
    rc = yq_txn_begin(db, YQ_TXN_READWRITE, &txn);
    if (rc == YQ_OK) break;
    if (rc != YQ_ERR_BUSY && rc != YQ_ERR_TIMEOUT) break;
    Sleep(1 << attempt);   /* 1, 2, 4, 8, 16 ms；POSIX 用 usleep */
}
```

### 损坏：立即停止写入

`YQ_ERR_CORRUPT`、`YQ_ERR_VERSION`、`YQ_ERR_PANIC` 会让数据库进入**只读保护态**：
写操作失败，读操作仍可用于导出，且绝不自动修复。

建议的应对：

1. 停止所有写入路径。
2. 把 `.yqdb` 与 `.log` 文件整份复制出来保留现场。
3. 用只读句柄尽力导出数据。
4. 上报，附 `yq_db_stat` 输出与 `yq_last_io_error`。
5. 禁止原地修复后继续使用。

完整分类见 [ERRORS.md](../ERRORS.md)。

```c
const char *msg = yq_strerror(rc);   /* 静态字符串，勿释放 */
int sys = yq_last_io_error();        /* 仅对 YQ_ERR_IO 有意义 */
```

---

## 10. 调优

| 字段 | 默认值 | 何时调整 |
|------|--------|----------|
| `page_size` | 4096 | 更大的页有利于顺序 / 范围吞吐 |
| `map_size` | 1 GiB | 出现 `YQ_ERR_MAP_FULL` 时调大 |
| `memtable_bytes` | 64 MiB | 内存工作集更大时调大 |
| `log_bytes` | 256 MiB | 想减少 checkpoint 频率时调大 |
| `max_readers` | 126 | 出现 `YQ_ERR_READER_FULL` 时调大 |
| `lock_timeout_ms` | 0 | 设为 `>0` 以等待写者而非直接失败 |

```c
opts.page_size       = 8192;
opts.map_size        = 4ULL * 1024 * 1024 * 1024;
opts.memtable_bytes  = 256ULL * 1024 * 1024;
opts.max_readers     = 512;
opts.lock_timeout_ms = 3000;
opts.sync_mode       = YQ_SYNC_NORMAL;
```

---

## 11. 崩溃恢复与备份

### 恢复是自动的

打开数据库时会扫描 WAL，仅把**已提交**事务回放到内存表，未提交事务被丢弃。恢复期间
持有写锁，无需任何手动步骤。

### 备份

由于持久化经由 `path.log`，正确的备份必须同时包含两个文件。最稳妥的做法是先关闭数据库
（关闭时会 checkpoint 并同步），再复制：

```text
app.yqdb
app.yqdb.log
```

`app.yqdb.shm` 与 `app.yqdb.lock` 仅运行时使用，无需复制。

---

## 12. 完整示例

```c
#include <stdio.h>
#include <string.h>
#include "yq.h"

int main(void) {
    yq_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size     = sizeof(yq_opts);
    opts.flags           = YQ_OPEN_CREATE;
    opts.lock_timeout_ms = 2000;

    yq_db *db = NULL;
    int rc = yq_open("demo.yqdb", &opts, &db);
    if (rc != YQ_OK) {
        fprintf(stderr, "open: %s\n", yq_strerror(rc));
        return 1;
    }

    /* 单事务批量插入 */
    yq_txn *txn = NULL;
    yq_txn_begin(db, YQ_TXN_READWRITE, &txn);
    for (int i = 0; i < 100; ++i) {
        char kb[32], vb[32];
        int kl = snprintf(kb, sizeof(kb), "key:%04d", i);
        int vl = snprintf(vb, sizeof(vb), "value-%d", i);
        yq_slice k = { kb, (size_t)kl };
        yq_slice v = { vb, (size_t)vl };
        yq_put(txn, k, v, YQ_PUT_UPSERT);
    }
    if (yq_txn_commit(txn) != YQ_OK) {
        fprintf(stderr, "commit failed\n");
        yq_close(db);
        return 1;
    }

    /* 扫描 */
    yq_txn_begin(db, YQ_TXN_READONLY, &txn);
    yq_cur *cur = NULL;
    yq_cur_open(txn, &cur);
    for (int r = yq_cur_first(cur); r == YQ_OK; r = yq_cur_next(cur)) {
        yq_slice ck, cv;
        yq_cur_key(cur, &ck);
        yq_cur_val(cur, &cv);
        printf("%.*s = %.*s\n",
               (int)ck.size, (const char *)ck.data,
               (int)cv.size, (const char *)cv.data);
    }
    yq_cur_close(cur);
    yq_txn_commit(txn);

    yq_stat st;
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(yq_stat);
    yq_db_stat(db, &st);
    printf("txn_id=%llu log_bytes=%llu\n",
           (unsigned long long)st.txn_id,
           (unsigned long long)st.log_bytes);

    yq_close(db);
    return 0;
}
```

---

## 13. 故障排查

| 现象 | 可能原因 | 处理 |
|------|----------|------|
| 打开时 `YQ_ERR_INVAL` | 未填 `struct_size`、`page_size` 非法、`path` 为 NULL | 填 `struct_size = sizeof(yq_opts)`；`page_size` 用 2 的幂 |
| `txn_begin` 返回 `YQ_ERR_BUSY` | 另一写者持锁，且 `lock_timeout_ms == 0` | 重试，或设 `lock_timeout_ms > 0` |
| `YQ_ERR_READER_FULL` | 读者数超过 `max_readers` | 以更大的 `max_readers` 重开 |
| `YQ_ERR_MAP_FULL` | 数据量超过 `map_size` | 以更大的 `map_size` 重开 |
| `YQ_ERR_TOOBIG` | 键 > 1024 字节 | 缩短键或改用哈希键 |
| `YQ_ERR_CORRUPT` | CRC 不匹配或格式非法 | 停止写入、复制文件、只读导出、上报 |
| 日志持续增长 | 长运行中未 checkpoint | 定期调用 `yq_checkpoint` |
| 重开后数据缺失 | 以 `YQ_SYNC_OFF` 提交 | 改用 `YQ_SYNC_NORMAL`/`FULL`，或调用 `yq_sync` |
| 写入后 `yq_get` 得到失效指针 | 借用指针被 `yq_put`/`yq_del` 作废 | 在后续写入前先复制值 |

---

磁盘格式见 [FORMAT.md](../FORMAT.md)；错误语义见 [ERRORS.md](../ERRORS.md)；精确 ABI 见
[include/yq.h](../include/yq.h)。
