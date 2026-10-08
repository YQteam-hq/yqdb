#include "yq_memtable.h"
#include "yq_memblk.h"
#include "yq_slice.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

/*
 * P3：把 memtable 的单次插入从 O(n) 元素搬移降到 O(log n)。
 * 内部结构改为跳表；对外接口（yq_memtable_*）签名与语义保持不变。
 *
 * 记账口径冻结：entry_cost 仍按原实现的
 *   key_len + val_len + sizeof(mt_entry)
 * 计算，保证 yq_memtable_bytes / yq_memtable_full 的触发点与现状一致。
 * 因此这里的 mt_entry 仅用于复现该口径，不参与实际存储。
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
    struct mt_skip_node *forward[];  /* 长度 = level + 1 */
} mt_skip_node;

struct yq_memtable {
    yq_memblk *arena;
    mt_skip_node *header;            /* 跳表头节点，key 恒为“负无穷” */
    int level;                       /* 当前最高层号（0 基） */
    size_t num_entries;              /* 含墓碑 */
    size_t max_bytes;
    size_t used_bytes;
};

struct yq_memtable_iter {
    yq_memtable *mt;
    mt_skip_node *cur;               /* NULL 表示未定位/越界 */
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
    n->level = level;
    return n;
}

static int skip_random_level(void) {
    static int seeded = 0;
    if (!seeded) {
        srand((unsigned)time(NULL));
        seeded = 1;
    }
    int level = 0;
    while (level < MAX_SKIP_LEVEL - 1 && (rand() & 1)) {
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

    int lvl = skip_random_level();
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
    return n;
}

/* 层 0 上的前驱（可能是 header）。 */
static mt_skip_node *node_prev(yq_memtable *mt, mt_skip_node *node) {
    mt_skip_node *x = mt->header;
    while (x->forward[0] != NULL && x->forward[0] != node) {
        x = x->forward[0];
    }
    return x->forward[0] == node ? x : NULL;
}

static mt_skip_node *node_first(yq_memtable *mt) {
    mt_skip_node *n = mt->header->forward[0];
    while (n && n->tombstone) n = n->forward[0];
    return n;
}

static mt_skip_node *node_last(yq_memtable *mt) {
    mt_skip_node *x = mt->header;
    for (int i = mt->level; i >= 0; i--) {
        while (x->forward[i] != NULL) x = x->forward[i];
    }
    while (x && x != mt->header && x->tombstone) {
        x = node_prev(mt, x);
    }
    if (!x || x == mt->header) return NULL;
    return x;
}

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

    *out = it;
    return YQ_OK;
}

void yq_memtable_iter_close(yq_memtable_iter *it) {
    free(it);
}

int yq_memtable_iter_first(yq_memtable_iter *it) {
    if (!it || !it->mt) return YQ_ERR_INVAL;

    it->cur = node_first(it->mt);
    return it->cur ? YQ_OK : YQ_ERR_NOTFOUND;
}

int yq_memtable_iter_next(yq_memtable_iter *it) {
    if (!it || !it->mt) return YQ_ERR_INVAL;
    if (!it->cur) return YQ_ERR_NOTFOUND;

    mt_skip_node *n = it->cur->forward[0];
    while (n && n->tombstone) n = n->forward[0];
    it->cur = n;

    return n ? YQ_OK : YQ_ERR_NOTFOUND;
}

int yq_memtable_iter_last(yq_memtable_iter *it) {
    if (!it || !it->mt) return YQ_ERR_INVAL;

    it->cur = node_last(it->mt);
    return it->cur ? YQ_OK : YQ_ERR_NOTFOUND;
}

int yq_memtable_iter_prev(yq_memtable_iter *it) {
    if (!it || !it->mt) return YQ_ERR_INVAL;

    if (!it->cur) {
        /* 从“越界”状态回退到最后一条有效记录 */
        it->cur = node_last(it->mt);
        return it->cur ? YQ_OK : YQ_ERR_NOTFOUND;
    }

    mt_skip_node *p = node_prev(it->mt, it->cur);
    while (p && p != it->mt->header && p->tombstone) {
        p = node_prev(it->mt, p);
    }
    if (!p || p == it->mt->header) return YQ_ERR_NOTFOUND;

    it->cur = p;
    return YQ_OK;
}

int yq_memtable_iter_key(yq_memtable_iter *it, yq_slice *out) {
    if (!it || !it->mt || !it->cur || !out) return YQ_ERR_CURSOR;

    uint8_t *base = (uint8_t *)yq_memblk_base(it->mt->arena);
    yq_slice_set(out, base + it->cur->key_offset, it->cur->key_len);
    return YQ_OK;
}

int yq_memtable_iter_val(yq_memtable_iter *it, yq_slice *out) {
    if (!it || !it->mt || !it->cur || !out) return YQ_ERR_CURSOR;

    uint8_t *base = (uint8_t *)yq_memblk_base(it->mt->arena);
    yq_slice_set(out, base + it->cur->val_offset, it->cur->val_len);
    return YQ_OK;
}

int yq_memtable_iter_valid(yq_memtable_iter *it) {
    if (!it || !it->mt) return 0;
    return it->cur != NULL;
}

void yq_memtable_reset(yq_memtable *mt) {
    if (!mt) return;

    yq_memblk_reset(mt->arena);

    mt->header = node_alloc(mt, MAX_SKIP_LEVEL - 1);
    mt->level = 0;
    mt->num_entries = 0;
    mt->used_bytes = 0;
}
