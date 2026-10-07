#include "yq_recover.h"
#include "yq_memtable.h"
#include "yq_enc.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct {
    uint64_t txn_id;
    int type;
    uint8_t *key;
    size_t key_len;
    uint8_t *val;
    size_t val_len;
} rec_op;

typedef struct {
    yq_memtable *mt;
    rec_op *ops;
    size_t ops_count;
    size_t ops_cap;
    uint64_t *committed;
    size_t committed_count;
    size_t committed_cap;
} recover_ctx;

static int ctx_push_op(recover_ctx *ctx, uint64_t txn_id, int type,
                       const uint8_t *key, size_t klen,
                       const uint8_t *val, size_t vlen) {
    if (ctx->ops_count >= ctx->ops_cap) {
        size_t nc = ctx->ops_cap ? ctx->ops_cap * 2 : 128;
        rec_op *na = realloc(ctx->ops, nc * sizeof(rec_op));
        if (!na) return YQ_ERR_NOMEM;
        ctx->ops = na;
        ctx->ops_cap = nc;
    }
    rec_op *op = &ctx->ops[ctx->ops_count];
    op->txn_id = txn_id;
    op->type = type;
    op->key = malloc(klen ? klen : 1);
    if (!op->key) return YQ_ERR_NOMEM;
    if (klen) memcpy(op->key, key, klen);
    op->key_len = klen;
    op->val = NULL;
    op->val_len = 0;
    if (type == 2) {
        op->val = malloc(vlen ? vlen : 1);
        if (!op->val) { free(op->key); return YQ_ERR_NOMEM; }
        if (vlen) memcpy(op->val, val, vlen);
        op->val_len = vlen;
    }
    ctx->ops_count++;
    return YQ_OK;
}

static int ctx_mark_committed(recover_ctx *ctx, uint64_t txn_id) {
    if (ctx->committed_count >= ctx->committed_cap) {
        size_t nc = ctx->committed_cap ? ctx->committed_cap * 2 : 64;
        uint64_t *na = realloc(ctx->committed, nc * sizeof(uint64_t));
        if (!na) return YQ_ERR_NOMEM;
        ctx->committed = na;
        ctx->committed_cap = nc;
    }
    ctx->committed[ctx->committed_count++] = txn_id;
    return YQ_OK;
}

static int ctx_is_committed(recover_ctx *ctx, uint64_t txn_id) {
    for (size_t i = 0; i < ctx->committed_count; i++) {
        if (ctx->committed[i] == txn_id) return 1;
    }
    return 0;
}

static int recovery_visitor(void *ctx_arg, uint64_t lsn, uint64_t txn_id, int rec_type,
    const uint8_t *payload, size_t paylen) {
    recover_ctx *ctx = (recover_ctx *)ctx_arg;
    (void)lsn;

    if (rec_type == 4) {
        return ctx_mark_committed(ctx, txn_id);
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

int yq_recover(yq_wal *wal, yq_memtable *mt) {
    if (!wal || !mt) return YQ_ERR_INVAL;
    if (yq_wal_size(wal) == 0) return YQ_OK;

    recover_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.mt = mt;

    int rc = yq_wal_scan(wal, 0, recovery_visitor, &ctx);

    if (rc == YQ_OK || rc == YQ_ERR_CORRUPT) {
        for (size_t i = 0; i < ctx.ops_count; i++) {
            rec_op *op = &ctx.ops[i];
            if (!ctx_is_committed(&ctx, op->txn_id)) continue;
            yq_slice k;
            k.data = op->key;
            k.size = op->key_len;
            if (op->type == 2) {
                yq_slice v;
                v.data = op->val;
                v.size = op->val_len;
                yq_memtable_put(mt, k, v);
            } else if (op->type == 3) {
                yq_memtable_del(mt, k);
            }
        }
        rc = YQ_OK;
    }

    for (size_t i = 0; i < ctx.ops_count; i++) {
        free(ctx.ops[i].key);
        free(ctx.ops[i].val);
    }
    free(ctx.ops);
    free(ctx.committed);

    return rc;
}

int yq_recover_replay(yq_recover_ctx *ctx) {
    (void)ctx;
    return YQ_OK;
}
