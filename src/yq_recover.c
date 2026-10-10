#include "yq_recover.h"
#include "yq_memtable.h"
#include "yq_enc.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/*
 * P4: recovery replay acceleration.
 *
 * Before this change every pending operation looked up its transaction id with
 * a linear scan over the whole committed-transaction array, i.e. O(ops x txns).
 * The committed set is now an open-addressing hash set, so the commit check is
 * O(1) amortised and the recovery pass is close to O(ops).
 *
 * Pending operations keep their key/value bytes in two append-only arenas and
 * only record byte offsets into them. Offsets stay valid across arena growth,
 * which avoids the dangling-pointer hazard of storing raw pointers into a
 * buffer that may be reallocated.
 */

/* ── open-addressing hash set: txn_id -> committed? ──────────────────── */

#define RC_HASH_INIT_CAP 64u

typedef struct {
    uint64_t key;
    uint8_t  used;
} rc_hash_slot;

typedef struct {
    rc_hash_slot *slots;
    size_t cap;
    size_t size;
} rc_hash_set;

static uint64_t rc_hash_mix(uint64_t x) {
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

static int rc_hash_init(rc_hash_set *set) {
    set->cap = RC_HASH_INIT_CAP;
    set->size = 0;
    set->slots = (rc_hash_slot *)calloc(set->cap, sizeof(rc_hash_slot));
    return set->slots ? YQ_OK : YQ_ERR_NOMEM;
}

static void rc_hash_destroy(rc_hash_set *set) {
    free(set->slots);
    set->slots = NULL;
    set->cap = 0;
    set->size = 0;
}

static int rc_hash_grow(rc_hash_set *set) {
    size_t ncap = set->cap * 2;
    rc_hash_slot *nslots = (rc_hash_slot *)calloc(ncap, sizeof(rc_hash_slot));
    if (!nslots) return YQ_ERR_NOMEM;

    for (size_t i = 0; i < set->cap; i++) {
        if (!set->slots[i].used) continue;
        size_t idx = (size_t)(rc_hash_mix(set->slots[i].key) % ncap);
        while (nslots[idx].used) idx = (idx + 1) % ncap;
        nslots[idx].key = set->slots[i].key;
        nslots[idx].used = 1;
    }

    free(set->slots);
    set->slots = nslots;
    set->cap = ncap;
    return YQ_OK;
}

static int rc_hash_insert(rc_hash_set *set, uint64_t key) {
    /* keep the load factor under 0.75 so a free slot always exists */
    if ((set->size + 1) * 4 >= set->cap * 3) {
        int rc = rc_hash_grow(set);
        if (rc != YQ_OK) return rc;
    }

    size_t idx = (size_t)(rc_hash_mix(key) % set->cap);
    while (set->slots[idx].used) {
        if (set->slots[idx].key == key) return YQ_OK; /* already present */
        idx = (idx + 1) % set->cap;
    }
    set->slots[idx].key = key;
    set->slots[idx].used = 1;
    set->size++;
    return YQ_OK;
}

static int rc_hash_contains(const rc_hash_set *set, uint64_t key) {
    if (set->cap == 0) return 0;
    size_t idx = (size_t)(rc_hash_mix(key) % set->cap);
    while (set->slots[idx].used) {
        if (set->slots[idx].key == key) return 1;
        idx = (idx + 1) % set->cap;
    }
    return 0;
}

/* ── pending operations ──────────────────────────────────────────────── */

typedef struct {
    uint64_t txn_id;
    int type;
    size_t key_off;
    size_t key_len;
    size_t val_off;
    size_t val_len;
} rec_op;

typedef struct {
    yq_memtable *mt;
    rec_op *ops;
    size_t ops_count;
    size_t ops_cap;
    uint8_t *keys;
    size_t keys_used;
    size_t keys_cap;
    uint8_t *vals;
    size_t vals_used;
    size_t vals_cap;
    rc_hash_set committed;
} recover_ctx;

static int arena_put(uint8_t **buf, size_t *used, size_t *cap,
                     const uint8_t *src, size_t n, size_t *out_off) {
    if (*buf == NULL || *used + n > *cap) {
        size_t ncap = *cap ? *cap : 4096;
        while (ncap < *used + n) ncap *= 2;
        uint8_t *nb = (uint8_t *)realloc(*buf, ncap);
        if (!nb) return YQ_ERR_NOMEM;
        *buf = nb;
        *cap = ncap;
    }
    if (n) memcpy(*buf + *used, src, n);
    *out_off = *used;
    *used += n;
    return YQ_OK;
}

static int ctx_push_op(recover_ctx *ctx, uint64_t txn_id, int type,
                       const uint8_t *key, size_t klen,
                       const uint8_t *val, size_t vlen) {
    if (ctx->ops_count >= ctx->ops_cap) {
        size_t ncap = ctx->ops_cap ? ctx->ops_cap * 2 : 128;
        rec_op *na = (rec_op *)realloc(ctx->ops, ncap * sizeof(rec_op));
        if (!na) return YQ_ERR_NOMEM;
        ctx->ops = na;
        ctx->ops_cap = ncap;
    }

    size_t key_off = 0;
    int rc = arena_put(&ctx->keys, &ctx->keys_used, &ctx->keys_cap, key, klen, &key_off);
    if (rc != YQ_OK) return rc;

    size_t val_off = 0;
    if (type == 2) {
        rc = arena_put(&ctx->vals, &ctx->vals_used, &ctx->vals_cap, val, vlen, &val_off);
        if (rc != YQ_OK) return rc;
    }

    rec_op *op = &ctx->ops[ctx->ops_count];
    op->txn_id = txn_id;
    op->type = type;
    op->key_off = key_off;
    op->key_len = klen;
    op->val_off = val_off;
    op->val_len = (type == 2) ? vlen : 0;
    ctx->ops_count++;
    return YQ_OK;
}

static int recovery_visitor(void *ctx_arg, uint64_t lsn, uint64_t txn_id, int rec_type,
    const uint8_t *payload, size_t paylen) {
    recover_ctx *ctx = (recover_ctx *)ctx_arg;
    (void)lsn;

    if (rec_type == 4) {
        return rc_hash_insert(&ctx->committed, txn_id);
    }

    if (rec_type == 2) {
        size_t kpos = 0;
        uint64_t klen = 0;
        if (yq_varint_decode(payload, paylen, &klen, &kpos) != YQ_OK) return YQ_ERR_CORRUPT;
        if (klen > 1024 || kpos + klen > paylen) return YQ_ERR_CORRUPT;
        size_t vpos_rel = 0;
        uint64_t vlen = 0;
        if (yq_varint_decode(payload + kpos + klen, paylen - kpos - klen, &vlen, &vpos_rel) != YQ_OK) return YQ_ERR_CORRUPT;
        size_t vstart = kpos + klen + vpos_rel;
        if (vstart + vlen > paylen) return YQ_ERR_CORRUPT;
        return ctx_push_op(ctx, txn_id, 2, payload + kpos, (size_t)klen,
                           payload + vstart, (size_t)vlen);
    }

    if (rec_type == 3) {
        size_t kpos = 0;
        uint64_t klen = 0;
        if (yq_varint_decode(payload, paylen, &klen, &kpos) != YQ_OK) return YQ_ERR_CORRUPT;
        if (klen > 1024 || kpos + klen > paylen) return YQ_ERR_CORRUPT;
        return ctx_push_op(ctx, txn_id, 3, payload + kpos, (size_t)klen, NULL, 0);
    }

    return YQ_OK;
}

/* ── replay ──────────────────────────────────────────────────────────── */

typedef struct {
    const uint8_t *key;
    size_t key_len;
    size_t idx;   /* position in ctx->ops; a later op supersedes an earlier one */
} replay_entry;

static int replay_entry_cmp(const void *a, const void *b) {
    const replay_entry *x = (const replay_entry *)a;
    const replay_entry *y = (const replay_entry *)b;

    size_t n = x->key_len < y->key_len ? x->key_len : y->key_len;
    if (n > 0) {
        int c = memcmp(x->key, y->key, n);
        if (c != 0) return c < 0 ? -1 : 1;
    }
    if (x->key_len != y->key_len) return x->key_len < y->key_len ? -1 : 1;
    /* Same key: order by descending index so the last write comes first. */
    if (x->idx != y->idx) return x->idx > y->idx ? -1 : 1;
    return 0;
}

static void replay_apply(recover_ctx *ctx, size_t idx) {
    rec_op *op = &ctx->ops[idx];

    yq_slice k;
    k.data = ctx->keys ? ctx->keys + op->key_off : NULL;
    k.size = op->key_len;

    if (op->type == 2) {
        yq_slice v;
        v.data = ctx->vals ? ctx->vals + op->val_off : NULL;
        v.size = op->val_len;
        yq_memtable_put(ctx->mt, k, v);
    } else if (op->type == 3) {
        yq_memtable_del(ctx->mt, k);
    }
}

/*
 * Replay the committed operations into the memtable.
 *
 * Operations are collected in LSN order. Replaying them in that order is
 * correct but slow: the memtable keeps its entries in a sorted array, so a key
 * that belongs in the middle of the array makes yq_memtable_put() move the
 * whole tail, and a workload whose keys were written in random order turns
 * recovery into a quadratic pass.
 *
 * The replay therefore does two passes instead:
 *
 *   1. discard every op superseded by a later op on the same key and order
 *      what remains by key. Only the last write to a key is observable after
 *      the scan, so an earlier write to the same key is already dead.
 *   2. apply the surviving ops. They arrive in ascending key order, so every
 *      memtable insert lands at the end of the array and the pass becomes
 *      append-only.
 *
 * The resulting memtable is identical to a sequential replay: the same set of
 * final writes is applied, each op touches exactly one key, and the memtable is
 * key-ordered, so the order between distinct keys cannot matter.
 *
 * If the scratch array cannot be allocated the sequential replay runs instead,
 * so recovery still makes progress under memory pressure.
 */
static void replay_ops(recover_ctx *ctx) {
    if (ctx->ops_count == 0) return;

    replay_entry *order = (replay_entry *)malloc(ctx->ops_count * sizeof(*order));
    if (!order) {
        for (size_t i = 0; i < ctx->ops_count; i++) {
            if (rc_hash_contains(&ctx->committed, ctx->ops[i].txn_id)) {
                replay_apply(ctx, i);
            }
        }
        return;
    }

    size_t n = 0;
    for (size_t i = 0; i < ctx->ops_count; i++) {
        if (!rc_hash_contains(&ctx->committed, ctx->ops[i].txn_id)) continue;
        order[n].key = ctx->keys ? ctx->keys + ctx->ops[i].key_off : NULL;
        order[n].key_len = ctx->ops[i].key_len;
        order[n].idx = i;
        n++;
    }

    if (n == 0) {
        free(order);
        return;
    }

    qsort(order, n, sizeof(*order), replay_entry_cmp);

    int have_prev = 0;
    const uint8_t *prev_key = NULL;
    size_t prev_len = 0;

    for (size_t i = 0; i < n; i++) {
        /* Equal to the previous (already applied) key: the previous entry is
         * the later write, so this one is superseded. */
        if (have_prev && prev_len == order[i].key_len &&
            (prev_len == 0 || memcmp(prev_key, order[i].key, prev_len) == 0)) {
            continue;
        }
        replay_apply(ctx, order[i].idx);
        have_prev = 1;
        prev_key = order[i].key;
        prev_len = order[i].key_len;
    }

    free(order);
}

int yq_recover(yq_wal *wal, yq_memtable *mt) {
    if (!wal || !mt) return YQ_ERR_INVAL;
    if (yq_wal_size(wal) == 0) return YQ_OK;

    recover_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.mt = mt;

    int rc = rc_hash_init(&ctx.committed);
    if (rc != YQ_OK) return rc;

    rc = yq_wal_scan(wal, 0, recovery_visitor, &ctx);

    if (rc == YQ_OK || rc == YQ_ERR_CORRUPT) {
        replay_ops(&ctx);
        rc = YQ_OK;
    }

    rc_hash_destroy(&ctx.committed);
    free(ctx.ops);
    free(ctx.keys);
    free(ctx.vals);

    return rc;
}

int yq_recover_replay(yq_recover_ctx *ctx) {
    (void)ctx;
    return YQ_OK;
}
