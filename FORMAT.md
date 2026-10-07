# yq-DB 文件格式规格 v1

状态：**M0 冻结候选**。本规格通过评审后不再做破坏性变更；任何破坏性变更必须递增 `format_version` 并提供独立迁移工具，不做原地升级。

适用范围：yq-DB 核心 KV 引擎。SQL 层、行/列编码属于上层约定，见附录 B，不属于页格式。

---

## 1. 术语与总则

| 术语 | 含义 |
|---|---|
| page | 固定长度的磁盘单元，`page_size` 必须是 2 的幂，范围 `[4096, 65536]`，默认 `4096` |
| page_no | 从 0 开始的页号，`uint64` |
| txn_id | 单调递增的提交序号，`uint64`，从 1 开始，0 保留 |
| lsn | 日志序号，`uint64`，从 1 开始，0 表示"无日志" |
| checkpoint | 把内存表整体合并进 B+Tree 并截断日志的动作 |
| snapshot | 一个只读视图，由 `(txn_id, root_page)` 唯一确定 |

总则：

- **G1 字节序**：所有多字节整数一律**小端序**（LE），无论宿主平台。理由：x86-64 / ARM64 原生 LE，AArch64 大端变体已消亡，换取零字节序转换开销。
- **G2 校验**：所有页与所有日志记录都带 CRC32C。算法为 Castagnoli 多项式，若 CPU 支持 SSE4.2 / ARMv8 CRC 指令则用硬件实现。
- **G3 对齐**：所有页从 `page_size` 边界开始，页内偏移无额外对齐要求。
- **G4 保留字段**：所有 `reserved` 字段写入时必须置 0，读取时忽略。它们只能用于同 `format_version` 内的非破坏性扩展。
- **G5 校验失败即失败**：任何 CRC 不匹配都必须返回 `YQ_ERR_CORRUPT`，禁止"猜一个合理值继续"。

---

## 2. 文件布局

```text
yq.yqdb  ─┬─ page 0   meta A
         ├─ page 1   meta B
         ├─ page 2..n  数据页（B+Tree 内部节点 / 叶节点 / 溢出页 / 空闲页）
         └─ ...

yq.log    追加写日志，独立文件；不存在时等价于"无待重放日志"
yq.shm    读者槽位表，运行期共享内存，非持久化；可随时删除
yq.lock   仅用于跨进程写者互斥，不存业务数据
```

- `page 0` 与 `page 1` 是**双 meta 轮换**：任一时刻只写其中一个，另一个保持上一次有效提交的内容。这是崩溃安全的根。
- 数据页从 `page 2` 开始分配。
- 文件长度必须始终是 `page_size` 的整数倍（不变量 I5）。扩容以 `page_size` 为单位，每次至少扩 1 页。
- 日志与主库文件分离，使只读部署可以不带 `yq.log`。

### 2.1 为什么读者槽位必须放在独立文件

读者需要**写**槽位表来登记快照，但只读打开的进程对 `yq.yqdb` 没有写权限。若把槽位表放在 `yq.yqdb` 内，多进程只读就会失效。

因此槽位表放在独立的 `yq.shm`：

- 写者以读写方式打开 `yq.shm`，不存在则创建。
- 读者以读写方式打开 `yq.shm` 并登记槽位。**`YQ_OPEN_READONLY` 只表示"不修改主库"，不代表"不写 shm"**——这与 SQLite WAL 模式下 `-shm` 仍需可写的行为一致。
- 当读者既无法写 `yq.shm`、又检测到存在活跃写者时，**必须拒绝打开并返回 `YQ_ERR_IO`**，绝不降级为"不登记的读取"——那样会让写者看不到这个读者，从而回收它正在使用的页。要冒险读取就显式传 `YQ_OPEN_IMMUTABLE`，把承诺写进代码。
- 当读者既无法写 `yq.shm`，且确认无活跃写者时（例如只读介质、单进程离线分析），同样应传 `YQ_OPEN_IMMUTABLE`。该标志是**调用方的承诺**：承诺期内无其他进程写入本库。违反该承诺会导致数据损坏，责任在调用方。
- `yq.shm` 不是持久化数据，删除它是安全的；下次打开会自动重建。


---

## 3. Meta 页格式

一页，从偏移 0 开始，尾部补 0 至 `page_size`。

| 偏移 | 长度 | 字段 | 说明 |
|---|---|---|---|
| 0 | 8 | `magic` | 固定 `59 51 44 42 1A 0A 00 01`（`"YQDB"` + EOF/SUB 双字节 + 版本 1） |
| 8 | 4 | `format_version` | v1 = 1 |
| 12 | 4 | `page_size` | 打开时校验，与打开参数不一致则报 `YQ_ERR_INVAL` |
| 16 | 8 | `txn_id` | 本 meta 对应的最新已提交事务号 |
| 24 | 8 | `root_page` | 最新 B+Tree 根页号；空库为 0 |
| 32 | 8 | `free_head` | 空闲页链表头；空链为 0 |
| 40 | 8 | `npages` | 文件当前总页数 |
| 48 | 8 | `ckpt_lsn` | 本 meta 已包含到该 lsn 为止的日志效果 |
| 56 | 8 | `log_trunc_lsn` | 日志可安全截断到该位置（= 最小活跃快照所需） |
| 64 | 8 | `meta_seq` | 同一 `txn_id` 内的写入计数，用于判定 A/B 谁更新 |
| 72 | 8 | `reserved0` | 必须为 0。运行期的读者纪元属于 `yq.shm`，不持久化到 meta |
| 80 | 4 | `node_encoding` | 节点编码版本，v1 = 1 |
| 84 | 4 | `flags` | 见 §3.1 |
| 88 | 8 | `reserved1` | 必须为 0 |
| 96 | 4 | `header_crc32c` | 覆盖偏移 `0..96`（即不包含自身） |
| 100 | `page_size-100` | `padding` | 全 0 |

### 3.1 flags 位定义

| 位 | 名称 | 含义 |
|---|---|---|
| 0 | `YQ_MF_COMPRESSED_KEYS` | 内部节点启用了键前缀压缩 |
| 1 | `YQ_MF_PARTIAL_COMMIT` | 保留。当前版本必须为 0 |
| 2–31 | 保留 | 必须为 0 |

### 3.2 Meta 选择算法（打开库时）

```text
1. 读 page 0 与 page 1，各自校验 magic 与 header_crc32c
2. 对校验通过的 meta，检查 format_version：
     - 高于本库支持的最高版本 → 该 meta 标记为 "版本不符"
     - 全部 meta 都是版本不符 → 返回 YQ_ERR_VERSION
        （此时不做任何降级读取尝试）
3. 在剩余可用的 meta 中选 txn_id 大者；txn_id 相同则选 meta_seq 大者
4. 仅一个可用 → 选它，并对另一个页记一条 WARN 日志
5. 两个都 CRC 失败 → 返回 YQ_ERR_CORRUPT，拒绝打开。不做任何修复尝试
```

顺序理由：**先校验完整性，再判断版本**。CRC 未通过时版本字段本身不可信，拿一个可能被撕裂的版本号去报 `YQ_ERR_VERSION` 会把"数据损坏"误报成"版本不兼容"，掩盖真正的故障。


### 3.3 Meta 写入协议（提交路径）

```text
1. 写入目标 = A/B 中 txn_id 较小的那个（txn_id 相同则 meta_seq 较小者）
2. 组装新 meta：txn_id+1、新的 root_page/free_head/npages、更新 ckpt_lsn 与 meta_seq
3. pwrite 整页写入目标 meta 页
4. 持久化策略决定是否 fdatasync：
   - YQ_SYNC_FULL：fdatasync 后返回
   - YQ_SYNC_NORMAL：等待组提交轮次统一 fdatasync 后返回
   - YQ_SYNC_OFF：不等待（仅用于可丢弃数据的场景）
5. fdatasync 成功返回后，新 meta 才被视为生效
```

崩溃落在步骤 3 与 4 之间：目标 meta 页 CRC 可能不匹配，另一个 meta 仍完好，回退到上一提交。

---

## 4. 数据页格式

所有数据页共用一个 24 字节页头。

| 偏移 | 长度 | 字段 | 说明 |
|---|---|---|---|
| 0 | 1 | `page_type` | 1=leaf，2=internal，3=overflow，4=free |
| 1 | 1 | `flags` | 页级标志，位定义见 §4.1 |
| 2 | 2 | `nkeys` | 当前 cell 数；overflow/free 页该字段为 0 |
| 4 | 8 | `right_sibling` | 叶链表右邻居页号，0 表示无 |
| 12 | 8 | `left_sibling` | 叶链表左邻居页号，0 表示无 |
| 20 | 2 | `free_bytes` | 页内连续空闲字节数（不含碎片） |
| 22 | 2 | `header_size` | 页头长度，v1 为 24 |
| 24 | 2×nkeys | `slot[]` | 槽位数组，每项是 cell 相对本页的偏移 |
| … | … | 空闲区 | 从 `header_size + 2×nkeys` 起向下 |
| … | … | cell 区 | 从页尾**倒序**分配，向上生长 |
| page_size-4 | 4 | `page_crc32c` | 覆盖偏移 `0 .. page_size-4` |

布局为经典 slotted page：槽位升序 = 键升序，cell 从页尾倒放。删除只改 `nkeys` 与槽位，不搬移其他 cell。

### 4.1 页 flags 位定义

| 位 | 名称 | 含义 |
|---|---|---|
| 0 | `YQ_PF_LEAF_ROOT` | 该叶节点同时是根（即库里只有这一页） |
| 1 | `YQ_PF_DEFRAG_NEEDED` | 碎片率高，checkpoint 时优先整理 |
| 2–7 | 保留 | 必须为 0 |

### 4.2 内部节点 cell

```text
[key_len:varint][key:bytes][child_page_no:uint64]
```

- `child_page_no` 指向子页。语义为"所有 key < 本 cell 的 key 的记录都在左子树"。
- 最右孩子由 `right_sibling` 之外的约定给出：内部节点的**最后一个槽位之后**预留一个 `rightmost_child:uint64`，紧跟 slot 数组。若 `nkeys == 0`，`rightmost_child` 即唯一孩子。
- 键上限 `key_len <= 1024`（可由配置下调，不可上调超过 `page_size/4`）。超限由 API 层拒绝并返回 `YQ_ERR_TOOBIG`。

### 4.3 叶节点 cell

值内联：

```text
[key_len:varint][key:bytes][val_len:varint][value:bytes]
```

值溢出（`val_len > inline_max`，`inline_max` 默认 `page_size/4`）：

```text
[key_len:varint][key:bytes][overflow_page_no:uint64][total_val_len:varint]
```

### 4.4 溢出页 cell

```text
页头（page_type=3）
[data_len:uint32][payload]
```

`payload` 长度 = `min(page_size - 24 - 4, 剩余值长度)`，`right_sibling` 复用为**下一页号**构成链。链尾 `right_sibling = 0`。`nkeys` 置 0。

### 4.5 空闲页 cell

`page_type=4`。`right_sibling` 复用为**下一个空闲页号**，构成单向链表，表头存于 `meta.free_head`。写 `page_crc32c`，其余字节可为任意值（读取时忽略）。

**关键约束**：COW 事务中被释放的页不能立即复用，必须等所有活跃快照都不再引用旧 root 后才能挂回空闲链表。这是 MVCC 正确性的必要条件。

---

## 5. 键编码

核心 KV 层的键是**不透明字节串**，默认比较器为 `memcmp`。这既是性能选择（比较即一次内存比较），也是职责划分（类型语义留给上层）。

上层需要有序类型键时（SQL 层、二级索引），统一使用下列**保序编码**，定义在附录 A。

---

## 6. 日志格式

文件 `yq.log`，一条记录接一条，无页对齐要求。

### 6.1 记录头（29 字节）

| 偏移 | 长度 | 字段 |
|---|---|---|
| 0 | 8 | `lsn` |
| 8 | 8 | `txn_id` |
| 16 | 1 | `record_type` |
| 17 | 4 | `payload_len` |
| 21 | 4 | `header_crc32c`（覆盖 `0..21`） |
| 25 | 4 | `payload_crc32c`（覆盖 payload） |
| 29 | `payload_len` | `payload` |

### 6.2 记录类型与 payload

| 值 | 类型 | payload |
|---|---|---|
| 1 | `BEGIN` | 空 |
| 2 | `PUT` | `[key_len:varint][key][val_len:varint][value]` |
| 3 | `DEL` | `[key_len:varint][key]` |
| 4 | `COMMIT` | 空。**唯一的持久化生效点** |
| 5 | `ABORT` | 空 |
| 6 | `CKPT_BEGIN` | `[root_page:uint64][txn_id:uint64]` |
| 7 | `CKPT_END` | `[ckpt_lsn:uint64]`，日志可截断至此 |

**分组约束**：一个事务在日志中恰好是 `BEGIN` → 若干 `PUT`/`DEL` → `COMMIT`（或 `ABORT`）。禁止嵌套事务，禁止交错。组提交时多个事务的记录连续追加，各自成组。

### 6.3 日志与主库的关系（重要简化）

日志**只是加速层**。`checkpoint` 完成之后，B+Tree 才是唯一真相；日志中 `lsn <= meta.ckpt_lsn` 的部分对恢复无意义，可直接截断。

这条约束让恢复逻辑限制在约 200 行以内。任何"日志与树互为补充、需要双向对账"的设计都在本版本被明确否决。

---

## 7. 崩溃恢复算法

```text
recover(db):
  1. meta = select_meta(db)                     # 见 §3.2
  2. if meta 无效: return YQ_ERR_CORRUPT
  3. if 不存在 yq.log 或日志为空:
       return 直接使用 meta，恢复完成
  4. 从 meta.ckpt_lsn 起顺序扫描日志
  5. pending = {}                               # txn_id -> 变更集合
  6. loop:
       a. 读记录头 29 字节；不足 29 字节 → 尾部撕裂，break
       b. 校验 header_crc32c；失败 → break
       c. 读 payload；校验 payload_crc32c；失败 → break
       d. switch record_type:
            BEGIN   → pending[txn_id] = 空集合
            PUT/DEL → 若 pending 无该 txn_id → 记录 WARN 并 break（日志已损坏）
                      否则追加到 pending[txn_id]
            COMMIT  → 把 pending[txn_id] 标记为已提交，保留待重放
            ABORT   → 丢弃 pending[txn_id]
            CKPT_BEGIN → 记录 root_page，继续扫描
            CKPT_END   → 更新 ckpt_lsn，继续扫描（通常已到文件尾）
  7. 丢弃所有未标记已提交的 pending 项
  8. 把已提交项按 key 合并（同一 key 多次写取 lsn 最大者，DEL 为墓碑）
  9. 用合并结果重建内存表（不直接改 B+Tree）
 10. 更新 meta.ckpt_lsn = 扫描到的最后有效 lsn，写回 meta
 11. 恢复完成；是否立即 checkpoint 交由配置决定
```

要点：

- **步骤 6c 的 "break" 不等于报错**。日志尾部撕裂是正常崩溃形态，只丢尾部未提交部分。
- **步骤 6d 的异常才是真损坏**，此时返回 `YQ_ERR_CORRUPT`（保守优先，不猜）。
- 恢复不直接改 B+Tree，只重建内存表，避免恢复路径与正常写路径出现两套页操作逻辑。

---

## 8. MVCC、快照与并发

### 8.1 快照获取

`(txn_id, root_page)` 定义一个只读视图。为避免读到"新 txn_id 配旧 root_page"的撕裂组合，二者必须原子读取：

```text
实现要求：meta 内的 {
    txn_id, root_page, free_head, npages, ckpt_lsn
} 打包为一个 8 字节对齐的连续块（v1 中为偏移 16..64 共 48 字节），
读者用 seqlock 协议读取：
   do {
     s1 = seq;  barrier();
     拷贝数据块;
     barrier(); s2 = seq;
   } while ((s1 & 1) || s1 != s2);
写者在修改该块前后各递增 seq。
```

### 8.2 读者槽位表（`yq.shm`）

`yq.shm` 布局：

| 偏移 | 长度 | 内容 |
|---|---|---|
| 0 | 4 | `shm_magic`，固定 `59 51 53 48`（`"YQSH"`） |
| 4 | 4 | `shm_version`，v1 = 1 |
| 8 | 8 | `shm_epoch`，本文件每次被重新创建时递增 |
| 16 | 8 | `slot_count` |
| 24 | 40 | 保留，必须为 0 |
| 64 | `slot_count × 64` | 槽位数组 |

槽位数组从偏移 64 开始，每槽 64 字节（正好一条缓存行，避免伪共享）：

| 槽内偏移 | 长度 | 字段 |
|---|---|---|
| 0 | 4 | `pid` |
| 4 | 4 | `tid` |
| 8 | 8 | `snapshot_txn` |
| 16 | 8 | `snapshot_root_page` |
| 24 | 4 | `active`（0=空闲，1=占用） |
| 28 | 4 | `shm_epoch`（登记时从 shm 头抄写） |
| 32 | 32 | 保留 |

读者协议：

```text
acquire_snapshot:
  1. 用 seqlock 原子读取 meta 的 { txn_id, root_page, ... } 块（见 §8.1）
  2. 在槽位数组中找一个 active==0 的槽位，用 CAS 置为 1
  3. 写入 pid / tid / snapshot_txn / snapshot_root_page / shm_epoch
  4. 槽位用尽 → 返回 YQ_ERR_READER_FULL
  5. YQ_OPEN_IMMUTABLE 下跳过全部步骤 2-3（不登记）

release_snapshot:
  1. 把 snapshot_txn 置为 UINT64_MAX（表示"即将空闲"）
  2. 置 active = 0
```

写者回收规则（**只允许保守回收**）：

```text
reclaim_watermark = min(所有 active==1 槽位的 snapshot_txn)

回收单个槽位的条件（满足任一）：
  a. active == 1 且 pid 已不存在（POSIX: kill(pid,0) == ESRCH；
     Windows: OpenProcess 失败且 GetLastError() == ERROR_INVALID_PARAMETER）
  b. active == 1 且 slot.shm_epoch != shm.shm_epoch（槽位来自已被重建的旧 shm）
```

**失败方向必须向安全侧**：回收过早会让活跃读者正在使用的页被复用，直接导致数据损坏；回收过晚只会多占空间。因此规则 a 必须确认进程已消亡才回收。

代价是被 pid 复用误判时槽位可能长时间不回收，表现为文件体积增长而非数据损坏。这是有意接受的取舍。检测到长期未回收的槽位时应输出 WARN，由运维介入。


### 8.3 写者与回收

- **单写者**：通过 `yq.lock` 上的 OS 建议锁（POSIX `fcntl` / Windows `LockFileEx`）选举。拿不到即返回 `YQ_ERR_BUSY`（可配置阻塞等待 + 超时 → `YQ_ERR_TIMEOUT`）。
- 写者永不阻塞读者；读者永不阻塞写者。
- **页回收水位**：`reclaim_watermark = min(所有活跃槽位 snapshot_txn)`。仅当被释放页所属的 `txn_id < reclaim_watermark` 时才可挂回空闲链表。

### 8.4 读路径顺序

```text
1. 查内存表（按代际从新到旧，只查 <= 快照 txn_id 的代际）
2. 未命中 → 查哈希索引（可选，点查加速）
3. 未命中 → 从 snapshot_root_page 走 B+Tree，比较器为 memcmp
4. 命中 → yq_get_ref() 返回 mmap 内指针，零拷贝
```

---

## 9. Checkpoint 协议

触发条件（任一满足）：内存表字节数 > 阈值（默认 64 MB）、日志文件大小 > 阈值（默认 256 MB）、显式调用 `yq_checkpoint()`、干净关闭。

```text
1. 写 CKPT_BEGIN(root_page=旧根, txn_id=当前)
2. 冻结当前内存表为不可变代际（新写入进入新代际）
3. 在 COW 事务中把冻结代际整体合并进 B+Tree：
   - 页级 COW：修改任何页都先复制，再改副本，最后原子发布新根
   - 分裂/合并规则见 §9.1
4. 发布新 root_page（写 meta，见 §3.3）
5. 写 CKPT_END(ckpt_lsn=新位置)
6. 截断日志至 ckpt_lsn
7. 释放冻结代际内存
```

步骤 3 期间读写继续：读者用旧 root 读到旧数据，写者写新代际。只有步骤 4 的 meta 写入是原子的。

### 9.1 页分裂与合并

- 叶节点插入前检查 `free_bytes >= needed + 24`，不足则分裂；分裂点为键数中位数，两半各约 50%。
- 内部节点同上，分裂后向上传播；若根分裂则新建根页并更新 `root_page`。
- 叶节点删除后若 `free_bytes > page_size × 3/4` 且左右兄弟可容纳，则合并；合并后向上递归。
- 碎片率超过 40% 且本页写操作频繁时置 `YQ_PF_DEFRAG_NEEDED`，在 checkpoint 时整体重排。

---

## 10. 不变量清单

崩溃测试、模糊测试与恢复测试必须对下列每一条断言。

| 编号 | 不变量 |
|---|---|
| I1 | 任一时刻，`page 0` 与 `page 1` 中至少有一个 CRC32C 校验通过 |
| I2 | `meta.root_page` 可达的所有页 CRC32C 校验通过 |
| I3 | 叶节点链表按 key 严格递增且无断开（`left_sibling`/`right_sibling` 互逆） |
| I4 | 同一快照内任一 key 至多出现一次 |
| I5 | 文件长度是 `page_size` 的整数倍 |
| I6 | `meta.npages` 等于文件实际页数 |
| I7 | 空闲页链表无环，且与任何可达页无交集 |
| I8 | 已提交 `txn_id` 单调递增，崩溃后不出现回退 |
| I9 | 恢复后可见数据集 = 崩溃前最后一个 `COMMIT` 的数据集 |
| I10 | 日志尾部残留的未提交记录不改变恢复结果 |
| I11 | 溢出页链长度与 `total_val_len` 一致，链尾 `right_sibling == 0` |
| I12 | 任何 `reserved` 字段读取时被忽略，写入时为 0 |

---

## 11. 版本与兼容策略

- `format_version` **单向升级**。打开更高版本 → `YQ_ERR_VERSION`，绝不尝试降级读取。
- 同 `format_version` 内只允许两类扩展：使用 `flags` 未用位、使用 `reserved` 字段。前提是加 `reserved` 字段后新旧双方仍能正确读写同一 `page_size`。
- 任何改变页布局、cell 布局、meta 偏移的变更 → `format_version + 1`，并提供**独立迁移工具**（读到新库，写新库），不做原地升级。
- 打开低版本库时允许自动升级，但升级前必须强制 `fdatasync` 并写 `.bak` 副本。

---

## 附录 A：保序键编码（上层约定，非页格式）

用于 SQL 层与二级索引，保证编码后的字节串 `memcmp` 序等于逻辑序。每列前置一个 1 字节类型标签。

| 标签 | 类型 | 编码 |
|---|---|---|
| `0x01` | int64 | `value XOR 0x8000000000000000` 后按**大端**写入 8 字节 |
| `0x02` | uint64 | 大端 8 字节 |
| `0x03` | double | IEEE754 位模式；符号位为 0 时翻转符号位，为 1 时全部位取反；大端 8 字节 |
| `0x04` | bytes | 原字节 + `0x00 0x00` 转义（防止前缀歧义） |
| `0x05` | string | UTF-8 字节，同 bytes 转义规则 |
| `0x06` | bool | `0x00` / `0x01` |
| `0x07` | timestamp64 | 同 int64，语义为 UTC 微秒 |
| `0x08` | decimal128 | 大端 16 字节，符号位翻转 |

复合键按列顺序拼接。NULL 需要显式排序位（默认 NULL 最小）时，由上层在键首额外插入一个 `0x00`/`0x01` 前缀。

---

## 附录 B：类型化记录编码（行层约定，非页格式）

KV 核心把 value 视为不透明字节串。当 value 承载一行结构体时，使用下列紧凑编码。类型标签 1 字节，字段按声明顺序排列，无字段名、无偏移表。

| 标签 | 类型 | 编码 |
|---|---|---|
| `0x00` | null | 无 payload |
| `0x01` | int64 | zigzag LEB128 |
| `0x02` | uint64 | LEB128 |
| `0x03` | double | 小端 8 字节 |
| `0x04` | bytes | LEB128 长度 + 原字节 |
| `0x05` | string | LEB128 长度 + UTF-8 |
| `0x06` | bool | 1 字节 |
| `0x07` | timestamp64 | 同 int64 |
| `0x08` | decimal128 | 小端 16 字节 |
| `0x09` | array | LEB128 元素数 + 逐元素编码 |
| `0x0A` | map | LEB128 键值对数 + 逐对编码 |

**严格类型**：字段类型由 schema 固定，解码时不做类型推断、不做隐式转换。这是与 SQLite 动态类型的关键取舍——代价是不兼容 SQLite 的灵活类型，收益是省掉逐值类型分派、行更紧凑。

---

## 附录 C：常量速查

| 常量 | 值 |
|---|---|
| `YQ_MAGIC` | `59 51 44 42 1A 0A 00 01` |
| `YQ_SHM_MAGIC` | `59 51 53 48` |
| `YQ_FORMAT_VERSION` | 1 |
| `YQ_NODE_ENCODING` | 1 |
| 默认 `page_size` | 4096 |
| `page_size` 允许范围 | 4096 – 65536，2 的幂 |
| 页头长度 | 24 字节 |
| 日志记录头长度 | 29 字节 |
| meta 头长度 | 100 字节 |
| shm 头长度 | 64 字节 |
| 默认 `inline_max` | `page_size / 4` |
| 键长上限 | 1024 字节 |
| **单值长度上限** | **1 GiB（策略上限，非结构上限）** |
| 默认读者槽位数 | 126 |
| 读者槽位硬上限 | 65535（受槽位区 4 MiB 上限约束，64 字节/槽） |
| 槽位大小 | 64 字节 |
| 默认内存表阈值 | 64 MB |
| 默认日志阈值 | 256 MB |
| 默认 `map_size` | 1 GiB |
| 碎片整理触发率 | 40% |
| 页合并触发率 | 75% |

单值长度上限为 1 GiB 是**刻意的策略选择**，不是结构限制：`total_val_len` 是 varint，溢出页链也没有结构长度上限。设置该上限是为了让"一个键的值占满整块磁盘"这类事故在 API 边界就被拒绝（`YQ_ERR_TOOBIG`），而不是在写满磁盘后以 `YQ_ERR_NOSPACE` 收场。

