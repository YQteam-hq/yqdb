# yq-DB

[![Language](https://img.shields.io/badge/language-C11-blue.svg)](#环境要求)
[![Version](https://img.shields.io/badge/version-1.0.0-green.svg)](#版本与兼容策略)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)
[![Dependencies](https://img.shields.io/badge/dependencies-none-brightgreen.svg)](#环境要求)

**用纯 C11 编写的轻量级嵌入式键值存储引擎。**

yq-DB 是一个面向嵌入场景的单文件、零依赖 KV 存储引擎。它提供 C ABI 的键值读写、
事务与游标遍历，不提供 SQL —— 目标是**比 SQLite 更轻量**，同时坚持正确性优先的设计。

> [English](README.md) · **简体中文**

---

## 目录

- [为什么选择 yq-DB](#为什么选择-yq-db)
- [特性](#特性)
- [架构概览](#架构概览)
- [环境要求](#环境要求)
- [构建](#构建)
- [快速开始](#快速开始)
- [API 速览](#api-速览)
- [数据文件](#数据文件)
- [默认配置](#默认配置)
- [测试](#测试)
- [当前范围与限制](#当前范围与限制)
- [版本与兼容策略](#版本与兼容策略)
- [文档索引](#文档索引)
- [许可证](#许可证)

---

## 为什么选择 yq-DB

- **比 SQLite 更轻量。** 无第三方依赖，除 C11 编译器外不需要任何构建工具，无独立
  服务进程。核心引擎约 3000 行 C 代码。
- **单文件。** 一个 `.yqdb` 文件承载全部数据，附属文件仅用于日志、共享内存读者槽位
  与跨进程锁。
- **嵌入式。** 直接链接进你的进程。无网络、无守护进程、无后台线程。
- **正确性优先。** 每个页、每条日志记录都带 CRC32C。一旦出现损坏，引擎拒绝"猜测"，
  而是降级为只读保护态，绝不静默"修复"数据。

## 特性

| 领域 | 支持情况 |
|------|----------|
| 写时复制 B+Tree | slotted page、叶/内部节点、页大小可配置 |
| MVCC 快照读 | 只读事务无锁获取 `(txn_id, root_page)` 快照，不阻塞写者 |
| 预写日志 | 提交先落 WAL，记录头与负载均带 CRC32C，容忍撕裂写 |
| 事务 | 单写多读，提交具原子性与持久性 |
| 暂存式写入 | 写操作先入私有队列，提交时原子应用 |
| 崩溃恢复 | 打开时回放日志，仅已提交事务存活 |
| 双元数据页 | 页 0 / 页 1 按 `txn_id` / `meta_seq` 轮换，保证崩溃安全 |
| 零拷贝读取 | 值以指向 mmap 的借用指针返回 |
| 游标 | 双向有序遍历，支持 seek / seek_le / seek_exact |
| 可移植 | Windows（Win32）与 POSIX 后端统一在同一 VFS 接口下 |

## 架构概览

```
        应用程序
            │  C ABI (yq.h)
            ▼
   ┌──────────────────┐
   │   yq / yq_txn    │  公共 API、暂存式事务
   └────────┬─────────┘
            │
   ┌────────▼─────────┐   ┌──────────────┐   ┌──────────────┐
   │    memtable      │   │  WAL + recover│   │  MVCC / shm  │
   │  （有序 KV）      │   │   （持久化）   │   │  （快照）     │
   └────────┬─────────┘   └──────────────┘   └──────────────┘
            │
   ┌────────▼─────────┐
   │  COW B+Tree 页    │
   └────────┬─────────┘
            │
   ┌────────▼─────────┐
   │   VFS（Win32 /   │
   │      POSIX）     │
   └──────────────────┘
```

磁盘布局、页结构、日志记录格式与恢复算法详见 [FORMAT.md](FORMAT.md)。

## 环境要求

- C11 编译器：**GCC**、**Clang** 或 **MSVC**。
- **CMake ≥ 3.10**（可选 —— 也可直接用 `gcc` 构建）。
- 无第三方库。POSIX 下无需 `-lpthread`，库自身不使用线程。

## 构建

### 使用 CMake

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

### 直接使用 gcc

```sh
gcc -std=c11 -Wall -Wextra -Iinclude -O2 -c src/*.c
ar rcs libyqdb.a *.o
```

### 集成到你的项目

```sh
gcc -std=c11 -I path/to/yqdb/include your_app.c path/to/yqdb/libyqdb.a -o your_app
```

## 快速开始

```c
#include <stdio.h>
#include <string.h>
#include "yq.h"

int main(void) {
    yq_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(yq_opts);   /* 必须填写 */
    opts.flags       = YQ_OPEN_CREATE;    /* 不存在则创建 */

    yq_db *db = NULL;
    if (yq_open("my.yqdb", &opts, &db) != YQ_OK) {
        return 1;
    }

    /* 写入 */
    yq_txn *txn = NULL;
    yq_txn_begin(db, YQ_TXN_READWRITE, &txn);
    yq_slice k = { "hello", 5 };
    yq_slice v = { "world", 5 };
    yq_put(txn, k, v, YQ_PUT_UPSERT);
    yq_txn_commit(txn);

    /* 读取 */
    yq_txn_begin(db, YQ_TXN_READONLY, &txn);
    yq_slice out = { 0 };
    if (yq_get(txn, k, &out) == YQ_OK) {
        printf("hello = %.*s\n", (int)out.size, (const char *)out.data);
    }
    yq_txn_commit(txn);

    yq_close(db);
    return 0;
}
```

更完整的示例（游标、错误处理、checkpoint、并发）见[使用文档](docs/USAGE.zh-CN.md)。

## API 速览

**生命周期**

| 函数 | 说明 |
|------|------|
| `yq_open` | 打开数据库（含崩溃恢复） |
| `yq_close` | 回滚活跃事务、checkpoint、同步、解除映射 |
| `yq_version` | 返回 `major.minor.patch` |
| `yq_strerror` | 错误码的可读描述 |
| `yq_last_io_error` | 最近一次系统 I/O 错误（`errno` / `GetLastError`） |

**事务**

| 函数 | 说明 |
|------|------|
| `yq_txn_begin` | 开启 `READONLY` 或 `READWRITE` 事务 |
| `yq_txn_commit` | 提交（持久性由 `sync_mode` 决定） |
| `yq_txn_abort` | 丢弃全部变更并释放快照 |

**键值操作**

| 函数 | 说明 |
|------|------|
| `yq_put` | 写入（`UPSERT` 或 `NOOVERWRITE`） |
| `yq_del` | 删除（幂等） |
| `yq_get` | 点查（零拷贝借用指针） |

**游标**

| 函数 | 说明 |
|------|------|
| `yq_cur_open` / `yq_cur_close` | 创建 / 销毁绑定事务的游标 |
| `yq_cur_first` / `yq_cur_last` | 定位到首 / 末条记录 |
| `yq_cur_seek` / `yq_cur_seek_le` / `yq_cur_seek_exact` | 按键定位 |
| `yq_cur_next` / `yq_cur_prev` | 前移 / 后移一条 |
| `yq_cur_valid` / `yq_cur_key` / `yq_cur_val` | 读取当前记录 |

**维护**

| 函数 | 说明 |
|------|------|
| `yq_checkpoint` | 内存表合并进 B+Tree 并截断日志 |
| `yq_sync` | 将内存表与日志刷到稳定存储 |
| `yq_db_stat` | 读取引擎统计信息 |

所有返回 `int` 的函数，成功返回 `YQ_OK`（0）。其余错误码见 [ERRORS.md](ERRORS.md)。
`yq_get` 与游标取数返回**借用指针**，仅在文档规定生命周期内有效；如需长期持有请自行
复制字节。

## 数据文件

传入 `yq_open` 的 `path` **就是数据库文件本身**。同目录下会有三个附属文件：

| 文件 | 作用 | 可否安全删除 |
|------|------|--------------|
| `path` | 主库文件（meta 页 + B+Tree 页） | 否 |
| `path.log` | 预写日志（WAL） | 否 —— 承载尚未落盘的数据 |
| `path.shm` | 读者槽位表（仅运行时） | 可 —— 打开时自动重建 |
| `path.lock` | 跨进程写者锁 | 可 |

约定上，yq-DB 数据库文件使用 `.yqdb` 扩展名（如 `my.yqdb`），对应附属文件为
`my.yqdb.log`、`my.yqdb.shm`、`my.yqdb.lock`。

完整磁盘布局见 [FORMAT.md](FORMAT.md)。

## 默认配置

`yq_opts` 的所有字段都可零初始化，零值选择默认值（除非另有说明）。

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `page_size` | 4096 | `[4096, 65536]` 内的 2 的幂 |
| `sync_mode` | `YQ_SYNC_NORMAL` | 组提交；`YQ_SYNC_FULL` 每次提交都同步 |
| `map_size` | 1 GiB | mmap 映射区大小 |
| `memtable_bytes` | 64 MiB | checkpoint 阈值 |
| `log_bytes` | 256 MiB | 日志 checkpoint 阈值 |
| `max_readers` | 126 | 并发读者槽位（硬上限 65535） |
| `lock_timeout_ms` | 0 | 0 = 不等待；`>0` 表示等待写锁的毫秒数 |

## 测试

```sh
# CMake
ctest --test-dir build --output-on-failure

# 或直接编译运行
gcc -std=c11 -Iinclude src/yq_test_basic.c src/*.o -o yq_test_basic
gcc -std=c11 -Iinclude src/yq_test_integration.c src/*.o -o yq_test_integration
```

| 测试 | 覆盖范围 |
|------|----------|
| `yq_test_basic` | varint 编解码、字节切片比较、CRC32C、内存块分配器 |
| `yq_test_integration` | 版本 / 错误码、开关库、读写删、覆盖写保护、事务回滚、游标遍历、checkpoint、并发读者、批量吞吐、统计信息 |

当前 `yq_test_integration` 的 12 项全部通过。

## 当前范围与限制

本版为 **1.0 首个稳定版**，请在使用前了解以下边界：

1. **持久化路径。** 当前通过「提交写 WAL → 打开时回放已提交事务到内存表」保证持久性；
   B+Tree 页落盘与日志截断属于预留能力，尚未启用。因此：
   - 关闭后数据可由 `path.log` 恢复；
   - 长运行且不做 checkpoint 时，`path.log` 会持续增长。
2. **单写者。** 同一时刻仅一个读写事务，跨进程由 `path.lock` 保证；只读事务可并发，
   上限为 `max_readers`。
3. **数据规模。** 活跃数据集受 `memtable_bytes` / `map_size` 约束，默认 64 MiB / 1 GiB。
4. **无 SQL。** 仅提供 KV 原语，不含查询语言、二级索引、表结构。

以上均不影响 1.0 的功能正确性，属于后续版本的演进方向。

## 版本与兼容策略

- **库版本**遵循 `YQ_VERSION_MAJOR.MINOR.PATCH`（当前 `1.0.0`），运行时可经
  `yq_version` 查询。
- **磁盘格式版本**为 `format_version = 1`（见 `yq_stat`）。破坏性变更时递增；
  未知的更高格式由 `YQ_ERR_VERSION` 拒绝。
- **错误码**一经发布即固定，不重排、不复用。
- **ABI**：公开结构体只允许在尾部追加字段，且必须同步更新 `struct_size`。

## 文档索引

| 文档 | 语言 | 内容 |
|------|------|------|
| [README.md](README.md) | English | 本文档的英文版 |
| [docs/USAGE.zh-CN.md](docs/USAGE.zh-CN.md) | 中文 | 使用文档：打开、事务、游标、调优、故障排查 |
| [docs/USAGE.md](docs/USAGE.md) | English | 使用文档英文版 |
| [docs/BENCHMARKS.zh-CN.md](docs/BENCHMARKS.zh-CN.md) | 中文 | 性能对比报告：与 SQLite、LMDB 对比 |
| [docs/BENCHMARKS.md](docs/BENCHMARKS.md) | English | 性能对比报告英文版 |
| [FORMAT.md](FORMAT.md) | 中文 | 磁盘格式规格（v1） |
| [ERRORS.md](ERRORS.md) | 中文 | 错误码语义与重试策略 |
| [include/yq.h](include/yq.h) | — | 权威公共 ABI 契约 |

## 许可证

以 [Apache License 2.0](LICENSE) 发布。

```
Copyright 2026 YQTeam

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0
```
