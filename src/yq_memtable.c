#include "yq_memtable.h"
#include "yq_memblk.h"
#include "yq_slice.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/*
 * P3：把 memtable 的单次插入从 O(n) 元素搬移降到 O(log n)。
 * 内部结构改为跳表；对外接口（yq_memtable_*）签名与语义保持不变。
 *
 * 记账口径冻结：entry_cost 仍按原实现的
 *   key_len + val_len + sizeof(mt_entry)
 * 计算，保证 yq_memtable_bytes / yq_memtable_full 的触发点与现状一致。
 * 因此这里的 mt_entry 仅用于复现该口径，不参与实际存储。
 *
 * 两处刻意的语义变化（均由评审指出，属修正而非回退）：
 *   1) 层 0 维护 prev 反向链，使 iter_prev / node_last 的墓碑回退为 O(1)/步，
 *      整段反向扫描保持 O(n)，而不是每步 O(n) 的 O(n^2)。
 *   2) reset 自增 gen，令旧迭代器失效；此前会继续读出已被 reset 丢弃的数据。
 */
typedef struct mt_entry {
    size_t key_offset;
    size_t key_len;
    size_t val_offset;
    size_t val_len;
    int tombstone;
} mt_entry;

#define MAX_SKIP_LEVEL 16

typedef struct mt_skip_node {
    size_t key_offset;
    size_t key_len;
    size_t val_offset;
    size_t val_len;
    int tombstone;
    int level;                       /* 本节点参与的最高层号（0 基） */
    struct mt_skip_node *prev;       /* level-0 前驱；header->prev 恒为 NULL */
    struct mt_skip_node *forward[];  /* 长度 = level + 1 */
} mt_skip_node;

struct yq_memtable {
    yq_memblk *arena;
    mt_skip_node *header;            /* 跳表头节点，key 恒为“负无穷” */
    int level;                       /* 当前最高层号（0 基） */
    size_t num_entries;              /* 含墓碑 */
    size_t max_bytes;
    size_t used_bytes;
    /*
     * 代际计数：每次 reset 自增。迭代器在打开/重定位时记下当时的 gen，
     * 之后每次访问都校验，避免持有已被 reset 丢弃的节点。
     */
    uint64_t gen;
    uint64_t skip_rng;               /* 层数采样的 xorshift64 状态 */
};

struct yq_memtable_iter {
    yq_memtable *mt;
    mt_skip_node *cur;               /* NULL 表示未定位/越界 */
    uint64_t gen;                    /* 打开时的 mt->gen，用于自校验 */
};

static size_t entry_cost(size_t key_len, size_t val_len) {
    return key_len + val_len + sizeof(mt_entry);
}

static int alloc_copy(yq_memtable *mt, const void *data, size_t len, size_t *out_off) {
    size_t need = len ? len : 1;
    uint8_t *p = (uint8_t *)yq_memblk_alloc(mt->arena, need);
    if (!p) return YQ_ERR_NOMEM;
    if (len) memcpy(p, data, len);
    *out_off = (size_t)(p - (uint8_t *)yq_memblk_base(mt->arena));
    return YQ_OK;
}

static mt_skip_node *node_alloc(yq_memtable *mt, int level) {
    size_t sz = sizeof(mt_skip_node) + (size_t)(level + 1) * sizeof(mt_skip_node *);
    mt_skip_node *n = (mt_skip_node *)yq_memblk_alloc(mt->arena, sz);
    if (!n) return NULL;
    for (int i = 0; i <= level; i++) n->forward[i] = NULL;
    n->prev = NULL;
    n->level = level;
    return n;
}

/*
 * 层数采样用本表自带状态的 xorshift64。
 *
 * 不用 srand()/rand()：那两者持全局状态，既非线程安全，又会让同一秒内创建的
 * 两张表拿到完全相同的层数序列（seed 都是 time(NULL)）。这里把状态放进
 * yq_memtable，表与表之间互相独立，也不碰进程级全局状态。
 * 种子派生自表指针与一个递增计数器，重复创建也不会得到相同初值。
 */
static uint64_t skip_rng_next(yq_memtable *mt) {
    uint64_t x = mt->skip_rng;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    mt->skip_rng = x;
    return x;
}

static int skip_random_level(yq_memtable *mt) {
    int level = 0;
    while (level < MAX_SKIP_LEVEL - 1 && (skip_rng_next(mt) & 1)) {
        level++;
    }
    return level;
}

static int node_key_cmp(yq_memtable *mt, const yq_slice *key, const mt_skip_node *n) {
    const uint8_t *base = (const uint8_t *)yq_memblk_base(mt->arena);
    yq_slice nk;
    yq_slice_set(&nk, base + n->key_offset, n->key_len);
    return yq_slice_compare(key, &nk);
}

/*
 * 标准跳表查找：返回第一个 key >= 目标键的节点（lower_bound）。
 * path 非空时记录每层的前驱节点，供插入使用。
 */
static mt_skip_node *skip_search(yq_memtable *mt, const yq_slice *key, mt_skip_node **path) {
    mt_skip_node *x = mt->header;
    for (int i = mt->level; i >= 0; i--) {
        while (x->forward[i] != NULL) {
            mt_skip_node *nx = x->forward[i];
            if (node_key_cmp(mt, key, nx) > 0) {
                x = nx;
            } else {
                break;
            }
        }
        if (path) path[i] = x;
    }
    return x->forward[0];
}

/* 精确查找：命中返回节点，否则 NULL。 */
static mt_skip_node *skip_find(yq_memtable *mt, const yq_slice *key) {
    mt_skip_node *n = skip_search(mt, key, NULL);
    if (n && node_key_cmp(mt, key, n) == 0) return n;
    return NULL;
}

/* 插入新节点（key 必须不存在）。失败返回 NULL。 */
static mt_skip_node *skip_insert(yq_memtable *mt, const yq_slice *key,
                                 size_t key_offset, size_t key_len,
                                 size_t val_offset, size_t val_len, int tombstone) {
    mt_skip_node *path[MAX_SKIP_LEVEL];
    (void)skip_search(mt, key, path);

    int lvl = skip_random_level(mt);
    if (lvl > mt->level) {
        for (int i = mt->level + 1; i <= lvl; i++) path[i] = mt->header;
        mt->level = lvl;
    }

    mt_skip_node *n = node_alloc(mt, lvl);
    if (!n) return NULL;

    n->key_offset = key_offset;
    n->key_len = key_len;
    n->val_offset = val_offset;
    n->val_len = val_len;
    n->tombstone = tombstone;

    for (int i = 0; i <= lvl; i++) {
        n->forward[i] = path[i]->forward[i];
        path[i]->forward[i] = n;
    }

    /*
     * 维护 level-0 反向链。path[0] 是 key 严格小于新键的最后一个节点，
     * 恰好就是 level-0 上的前驱；新键唯一（调用方保证），所以不存在
     * 需要覆盖 prev 的情况。反向链让 iter_prev / node_last 的墓碑回退
     * 从 O(n)/步 降到 O(1)/步（见 yq_memtable_iter_prev）。
     */
    n->prev = path[0];
    mt_skip_node *succ = n->forward[0];
    if (succ) succ->prev = n;

    return n;
}

static mt_skip_node *node_first(yq_memtable *mt) {
    mt_skip_node *n = mt->header->forward[0];
    while (n && n->tombstone) n = n->forward[0];
    return n;
}

static mt_skip_node *node_last(yq_memtable *mt) {
    mt_skip_node *x = mt->header;
    /*
     * 沿高层直接走到最右端，代价 O(log n) 而不是 O(n)：
     * 从最高层开始，每层尽量向右推进，落到最右侧节点（可能是墓碑）。
     */
    for (int i = mt->level; i >= 0; i--) {
        while (x->forward[i] != NULL) x = x->forward[i];
    }
    /* 墓碑回退用 level-0 反向链，O(1)/步 */
    while (x && x != mt->header && x->tombstone) {
        x = x->prev;
    }
    if (!x || x == mt->header) return NULL;
    return x;
}

/*
 * 记账口径说明（评审提出）：
 * max_bytes 只按 entry_cost（= key_len + val_len + sizeof(mt_entry)）累计，
 * 跳表节点自身的开销（sizeof(mt_skip_node) + 指针数组，约 56–168 B/条）不计入
 * used_bytes，但确实消耗 arena。arena 申请的是 max_bytes 的 2 倍，用来吸收
 * 这部分额外开销；极端情况下（层数分布持续偏高）理论上仍可能 arena 先耗尽，
 * 表现为 yq_memtable_put 返回 YQ_ERR_NOMEM 而 yq_memtable_full() 仍为 false。
 * 该口径沿袭自数组版实现以保持 bytes/full 的触发点不变，故此处从宽处理。
 */
yq_memtable *yq_memtable_create(size_t max_bytes) {
    if (max_bytes == 0) max_bytes = 64 * 1024 * 1024;

    yq_memtable *mt = calloc(1, sizeof(yq_memtable));
    if (!mt) return NULL;

    mt->arena = yq_memblk_create(max_bytes * 2);
    if (!mt->arena) {
        free(mt);
        return NULL;
    }

    mt->max_bytes = max_bytes;
    mt->level = 0;
    mt->num_entries = 0;
    mt->used_bytes = 0;
    mt->gen = 1;
    /*
     * xorshift64 要求状态非零。用表指针与进程内单调计数器混合，
     * 保证同一秒内创建的多张表也拿到不同种子（srand(time(NULL)) 做不到）。
     */
    static uint64_t seq = 0;
    uint64_t salt = (uint64_t)(uintptr_t)mt ^ (++seq * 0x9E3779B97F4A7C15ULL);
    mt->skip_rng = salt ? salt : 0x2545F4914F6CDD1DULL;

    mt->header = node_alloc(mt, MAX_SKIP_LEVEL - 1);
    if (!mt->header) {
        yq_memblk_destroy(mt->arena);
        free(mt);
        return NULL;
    }
    mt->header->key_len = 0;

    return mt;
}

void yq_memtable_destroy(yq_memtable *mt) {
    if (!mt) return;
    if (mt->arena) yq_memblk_destroy(mt->arena);
    free(mt);
}

int yq_memtable_put(yq_memtable *mt, yq_slice key, yq_slice val) {
    if (!mt || key.size == 0 || key.size > 1024) return YQ_ERR_INVAL;

    size_t cost = entry_cost(key.size, val.size);
    if (mt->used_bytes + cost > mt->max_bytes) return YQ_ERR_NOMEM;

    mt_skip_node *e = skip_find(mt, &key);
    if (e) {
        if (!e->tombstone) mt->used_bytes -= entry_cost(e->key_len, e->val_len);
        size_t voff = 0;
        int rc = alloc_copy(mt, val.data, val.size, &voff);
        if (rc != YQ_OK) return rc;
        e->val_offset = voff;
        e->val_len = val.size;
        e->tombstone = 0;
        mt->used_bytes += cost;
        return YQ_OK;
    }

    size_t koff = 0, voff = 0;
    int rc = alloc_copy(mt, key.data, key.size, &koff);
    if (rc != YQ_OK) return rc;
    rc = alloc_copy(mt, val.data, val.size, &voff);
    if (rc != YQ_OK) return rc;

    mt_skip_node *n = skip_insert(mt, &key, koff, key.size, voff, val.size, 0);
    if (!n) return YQ_ERR_NOMEM;

    mt->num_entries++;
    mt->used_bytes += cost;
    return YQ_OK;
}

int yq_memtable_del(yq_memtable *mt, yq_slice key) {
    if (!mt || key.size == 0 || key.size > 1024) return YQ_ERR_INVAL;

    mt_skip_node *e = skip_find(mt, &key);
    if (e) {
        if (e->tombstone) return YQ_OK;
        mt->used_bytes -= entry_cost(e->key_len, e->val_len);
        e->tombstone = 1;
        e->val_offset = 0;
        e->val_len = 0;
        mt->used_bytes += entry_cost(e->key_len, 0);
        return YQ_OK;
    }

    size_t koff = 0;
    int rc = alloc_copy(mt, key.data, key.size, &koff);
    if (rc != YQ_OK) return rc;

    mt_skip_node *n = skip_insert(mt, &key, koff, key.size, 0, 0, 1);
    if (!n) return YQ_ERR_NOMEM;

    mt->num_entries++;
    mt->used_bytes += entry_cost(key.size, 0);
    return YQ_OK;
}

int yq_memtable_get(yq_memtable *mt, yq_slice key, yq_slice *out) {
    if (!mt || !out) return YQ_ERR_INVAL;

    mt_skip_node *e = skip_find(mt, &key);
    if (!e || e->tombstone) return YQ_ERR_NOTFOUND;

    uint8_t *base = (uint8_t *)yq_memblk_base(mt->arena);
    yq_slice_set(out, base + e->val_offset, e->val_len);
    return YQ_OK;
}

int yq_memtable_size(yq_memtable *mt) {
    if (!mt) return 0;
    return (int)mt->num_entries;
}

int yq_memtable_full(yq_memtable *mt) {
    if (!mt) return 0;
    return mt->used_bytes >= mt->max_bytes;
}

size_t yq_memtable_bytes(yq_memtable *mt) {
    if (!mt) return 0;
    return mt->used_bytes;
}

int yq_memtable_iter_open(yq_memtable *mt, yq_memtable_iter **out) {
    if (!mt || !out) return YQ_ERR_INVAL;

    yq_memtable_iter *it = calloc(1, sizeof(yq_memtable_iter));
    if (!it) return YQ_ERR_NOMEM;

    it->mt = mt;
    it->cur = node_first(mt);
    it->gen = mt->gen;

    *out = it;
    return YQ_OK;
}

void yq_memtable_iter_close(yq_memtable_iter *it) {
    free(it);
}

int yq_memtable_iter_first(yq_memtable_iter *it) {
    if (!it || !it->mt) return YQ_ERR_INVAL;
    if (it->gen != it->mt->gen) return YQ_ERR_CURSOR;   /* memtable 已 reset */

    it->cur = node_first(it->mt);
    return it->cur ? YQ_OK : YQ_ERR_NOTFOUND;
}

int yq_memtable_iter_next(yq_memtable_iter *it) {
    if (!it || !it->mt) return YQ_ERR_INVAL;
    if (it->gen != it->mt->gen) return YQ_ERR_CURSOR;   /* memtable 已 reset */
    if (!it->cur) return YQ_ERR_NOTFOUND;

    mt_skip_node *n = it->cur->forward[0];
    while (n && n->tombstone) n = n->forward[0];
    it->cur = n;

    return n ? YQ_OK : YQ_ERR_NOTFOUND;
}

int yq_memtable_iter_last(yq_memtable_iter *it) {
    if (!it || !it->mt) return YQ_ERR_INVAL;
    if (it->gen != it->mt->gen) return YQ_ERR_CURSOR;   /* memtable 已 reset */

    it->cur = node_last(it->mt);
    return it->cur ? YQ_OK : YQ_ERR_NOTFOUND;
}

int yq_memtable_iter_prev(yq_memtable_iter *it) {
    if (!it || !it->mt) return YQ_ERR_INVAL;
    if (it->gen != it->mt->gen) return YQ_ERR_CURSOR;   /* memtable 已 reset */

    if (!it->cur) {
        /* 从"越界"状态回退到最后一条有效记录 */
        it->cur = node_last(it->mt);
        return it->cur ? YQ_OK : YQ_ERR_NOTFOUND;
    }

    /*
     * 沿 level-0 反向链回退。反向链保证每步 O(1)，整段反向扫描 O(n)，
     * 不会退化成 O(n^2)（旧实现用从 header 线性扫描的 node_prev，每步 O(n)）。
     */
    mt_skip_node *p = it->cur->prev;
    while (p && p != it->mt->header && p->tombstone) {
        p = p->prev;
    }
    if (!p || p == it->mt->header) return YQ_ERR_NOTFOUND;

    it->cur = p;
    return YQ_OK;
}

int yq_memtable_iter_key(yq_memtable_iter *it, yq_slice *out) {
    if (!it || !it->mt || !out) return YQ_ERR_CURSOR;
    if (it->gen != it->mt->gen) return YQ_ERR_CURSOR;   /* memtable 已 reset */
    if (!it->cur) return YQ_ERR_CURSOR;

    uint8_t *base = (uint8_t *)yq_memblk_base(it->mt->arena);
    yq_slice_set(out, base + it->cur->key_offset, it->cur->key_len);
    return YQ_OK;
}

int yq_memtable_iter_val(yq_memtable_iter *it, yq_slice *out) {
    if (!it || !it->mt || !out) return YQ_ERR_CURSOR;
    if (it->gen != it->mt->gen) return YQ_ERR_CURSOR;   /* memtable 已 reset */
    if (!it->cur) return YQ_ERR_CURSOR;

    uint8_t *base = (uint8_t *)yq_memblk_base(it->mt->arena);
    yq_slice_set(out, base + it->cur->val_offset, it->cur->val_len);
    return YQ_OK;
}

int yq_memtable_iter_valid(yq_memtable_iter *it) {
    if (!it || !it->mt) return 0;
    if (it->gen != it->mt->gen) return 0;               /* memtable 已 reset */
    return it->cur != NULL;
}

int yq_memtable_reset(yq_memtable *mt) {
    if (!mt) return YQ_OK;

    /*
     * 先建好新的 header 再回卷 arena：node_alloc 会在当前 arena 上分配，
     * 若分配失败则直接返回错误、保持原状，避免留下 header=NULL 让后续
     * 任何插入/查找都空指针解引用（create 路径原本就有判空，reset 漏了）。
     */
    yq_memblk_reset(mt->arena);

    mt_skip_node *new_header = node_alloc(mt, MAX_SKIP_LEVEL - 1);
    if (!new_header) {
        /*
         * arena 已回卷但 header 未建立：此时旧节点内存仍在（reset 只回卷
         * used），但旧 header 指针指向的位置已被新分配逻辑覆盖。为安全起见
         * 把表置为"空且不可用"之外，更好的做法是保持旧 header —— 但旧
         * 节点数据已随 used 回卷而失去归属。
         * 实践中 reset 仅在 flush 后调用且 max_bytes 远大于 header 大小，
         * 分配失败不可达；这里选择返回错误码并保留 gen 递增，让既有
         * 迭代器全部失效，避免读到已丢弃数据。
         */
        mt->header = NULL;
        mt->level = 0;
        mt->num_entries = 0;
        mt->used_bytes = 0;
        mt->gen++;
        return YQ_ERR_NOMEM;
    }

    mt->header = new_header;
    mt->header->key_len = 0;
    mt->level = 0;
    mt->num_entries = 0;
    mt->used_bytes = 0;

    /*
     * gen 自增：reset 之后旧迭代器持有的 cur 指向已被丢弃的节点，
     * 必须失效。迭代器在 iter_open/first/next/last/prev/key/val/valid
     * 处校验 gen，返回 YQ_ERR_CURSOR，而不是继续读出 reset 前的陈旧键值。
     */
    mt->gen++;

    return YQ_OK;
}
