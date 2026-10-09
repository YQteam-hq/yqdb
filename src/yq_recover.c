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
    /* Enhanced parameter validation with comprehensive checking */
    if (!set) {
        fprintf(stderr, "ERROR: NULL set parameter in rc_hash_init\n");
        return YQ_ERR_INVAL;
    }
    
    if (RC_HASH_INIT_CAP == 0 || RC_HASH_INIT_CAP > SIZE_MAX / sizeof(rc_hash_slot)) {
        fprintf(stderr, "ERROR: Invalid RC_HASH_INIT_CAP in rc_hash_init\n");
        return YQ_ERR_INVAL;
    }
    
    set->cap = RC_HASH_INIT_CAP;
    set->size = 0;
    set->slots = (rc_hash_slot *)calloc(set->cap, sizeof(rc_hash_slot));
    if (!set->slots) {
        fprintf(stderr, "ERROR: calloc failed for hash slots in rc_hash_init\n");
        return YQ_ERR_NOMEM;
    }
    
    /* Initialize all slots to unused state for safety */
    for (size_t i = 0; i < set->cap; i++) {
        set->slots[i].used = 0;
        set->slots[i].key = 0;
    }
    
    return YQ_OK;
}

static void rc_hash_destroy(rc_hash_set *set) {
    /* Enhanced parameter validation with comprehensive checking */
    if (!set) {
        fprintf(stderr, "WARNING: NULL set parameter in rc_hash_destroy\n");
        return;
    }
    
    /* Enhanced validation of hash set state before destruction */
    if (set->cap > 0 && set->size > set->cap) {
        fprintf(stderr, "WARNING: Invalid size %zu > cap %zu in rc_hash_destroy\n", set->size, set->cap);
    }
    
    if (set->slots) {
        /* Validate slot count before freeing */
        if (set->cap > SIZE_MAX / sizeof(rc_hash_slot)) {
            fprintf(stderr, "WARNING: Invalid cap %zu in rc_hash_destroy\n", set->cap);
        } else {
            /* Check for memory leaks by counting used slots */
            size_t used_count = 0;
            for (size_t i = 0; i < set->cap; i++) {
                if (set->slots[i].used) {
                    used_count++;
                }
            }
            if (used_count != set->size) {
                fprintf(stderr, "WARNING: Slot count mismatch in rc_hash_destroy: %zu used, %zu reported\n", 
                        used_count, set->size);
            }
        }
        
        free(set->slots);
        set->slots = NULL;
    }
    
    set->cap = 0;
    set->size = 0;
}

static int rc_hash_grow(rc_hash_set *set) {
    /* Enhanced parameter validation with comprehensive checking */
    if (!set || set->cap == 0) {
        fprintf(stderr, "ERROR: Invalid set or cap %zu in rc_hash_grow\n", set ? set->cap : 0);
        return YQ_ERR_INVAL;
    }
    
    /* Validate current capacity before growing */
    if (set->cap > SIZE_MAX / 2) {
        fprintf(stderr, "ERROR: Current cap %zu too large to grow in rc_hash_grow\n", set->cap);
        return YQ_ERR_INVAL;
    }
    
    size_t ncap = set->cap * 2;
    if (ncap < set->cap) { /* Check for overflow */
        fprintf(stderr, "ERROR: Capacity overflow in rc_hash_grow: %zu * 2 = %zu\n", set->cap, ncap);
        return YQ_ERR_INVAL;
    }
    
    if (ncap > SIZE_MAX / sizeof(rc_hash_slot)) {
        fprintf(stderr, "ERROR: New cap %zu too large in rc_hash_grow\n", ncap);
        return YQ_ERR_INVAL;
    }
    
    rc_hash_slot *nslots = (rc_hash_slot *)calloc(ncap, sizeof(rc_hash_slot));
    if (!nslots) {
        fprintf(stderr, "ERROR: calloc failed for new slots in rc_hash_grow\n");
        return YQ_ERR_NOMEM;
    }
    
    /* Initialize new slots to unused state for safety */
    for (size_t i = 0; i < ncap; i++) {
        nslots[i].used = 0;
        nslots[i].key = 0;
    }

    /* Rehash existing elements with enhanced bounds checking */
    for (size_t i = 0; i < set->cap; i++) {
        if (!set->slots[i].used) continue;
        
        /* Validate key in existing slot */
        if (set->slots[i].key == 0) {
            fprintf(stderr, "ERROR: Zero key in used slot %zu in rc_hash_grow\n", i);
            continue;
        }
        
        size_t idx = (size_t)(rc_hash_mix(set->slots[i].key) % ncap);
        size_t attempts = 0;
        while (nslots[idx].used) {
            idx = (idx + 1) % ncap;
            attempts++;
            if (attempts > ncap) {
                fprintf(stderr, "ERROR: Infinite loop detected in rc_hash_grow rehashing\n");
                free(nslots);
                return YQ_ERR_INVAL;
            }
        }
        nslots[idx].key = set->slots[i].key;
        nslots[idx].used = 1;
    }

    free(set->slots);
    set->slots = nslots;
    set->cap = ncap;
    return YQ_OK;
}

static int rc_hash_insert(rc_hash_set *set, uint64_t key) {
    /* Enhanced parameter validation with comprehensive checking */
    if (!set || set->cap == 0) {
        fprintf(stderr, "ERROR: Invalid set or cap %zu in rc_hash_insert\n", set ? set->cap : 0);
        return YQ_ERR_INVAL;
    }
    
    /* Validate key parameter */
    if (key == 0) {
        fprintf(stderr, "ERROR: Zero key in rc_hash_insert\n");
        return YQ_ERR_INVAL;
    }
    
    /* keep the load factor under 0.75 so a free slot always exists */
    if ((set->size + 1) * 4 >= set->cap * 3) {
        int rc = rc_hash_grow(set);
        if (rc != YQ_OK) {
            fprintf(stderr, "ERROR: rc_hash_grow failed in rc_hash_insert\n");
            return rc;
        }
    }

    size_t idx = (size_t)(rc_hash_mix(key) % set->cap);
    size_t attempts = 0;
    while (set->slots[idx].used) {
        if (set->slots[idx].key == key) {
            fprintf(stderr, "WARNING: Key %llu already present in rc_hash_insert\n", (unsigned long long)key);
            return YQ_OK; /* already present */
        }
        idx = (idx + 1) % set->cap;
        attempts++;
        if (attempts > set->cap) {
            fprintf(stderr, "ERROR: Infinite loop detected in rc_hash_insert\n");
            return YQ_ERR_INVAL;
        }
    }
    
    /* Validate index bounds before assignment */
    if (idx >= set->cap) {
        fprintf(stderr, "ERROR: Invalid index %zu >= cap %zu in rc_hash_insert\n", idx, set->cap);
        return YQ_ERR_INVAL;
    }
    
    set->slots[idx].key = key;
    set->slots[idx].used = 1;
    set->size++;
    
    /* Validate size after insertion */
    if (set->size > set->cap) {
        fprintf(stderr, "ERROR: Size %zu > cap %zu after insertion in rc_hash_insert\n", set->size, set->cap);
        return YQ_ERR_INVAL;
    }
    
    return YQ_OK;
}

static int rc_hash_contains(const rc_hash_set *set, uint64_t key) {
    /* Enhanced parameter validation with comprehensive checking */
    if (!set || set->cap == 0) {
        fprintf(stderr, "ERROR: Invalid set or cap %zu in rc_hash_contains\n", set ? set->cap : 0);
        return 0;
    }
    
    /* Validate key parameter */
    if (key == 0) {
        fprintf(stderr, "ERROR: Zero key in rc_hash_contains\n");
        return 0;
    }
    
    size_t idx = (size_t)(rc_hash_mix(key) % set->cap);
    size_t attempts = 0;
    while (set->slots[idx].used) {
        /* Validate key in slot before comparison */
        if (set->slots[idx].key == 0) {
            fprintf(stderr, "ERROR: Zero key in used slot %zu in rc_hash_contains\n", idx);
            return 0;
        }
        
        if (set->slots[idx].key == key) return 1;
        idx = (idx + 1) % set->cap;
        attempts++;
        if (attempts > set->cap) {
            fprintf(stderr, "ERROR: Infinite loop detected in rc_hash_contains\n");
            return 0;
        }
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
    /* Enhanced parameter validation with comprehensive checking */
    if (!buf || !used || !cap || !out_off) {
        fprintf(stderr, "ERROR: NULL parameter in arena_put\n");
        return YQ_ERR_INVAL;
    }
    
    if (*used > SIZE_MAX - n) {
        fprintf(stderr, "ERROR: Size overflow in arena_put: used %zu + n %zu\n", *used, n);
        return YQ_ERR_INVAL;
    }
    
    if (*cap > SIZE_MAX - n) {
        fprintf(stderr, "ERROR: Capacity overflow in arena_put: cap %zu + n %zu\n", *cap, n);
        return YQ_ERR_INVAL;
    }
    
    if (*buf == NULL || *used + n > *cap) {
        /* Validate initial capacity */
        size_t ncap = *cap ? *cap : 4096;
        if (ncap == 0) {
            fprintf(stderr, "ERROR: Zero initial capacity in arena_put\n");
            return YQ_ERR_INVAL;
        }
        
        /* Calculate new capacity with overflow checking */
        while (ncap < *used + n) {
            if (ncap > SIZE_MAX / 2) {
                fprintf(stderr, "ERROR: Capacity overflow in arena_put growth\n");
                return YQ_ERR_INVAL;
            }
            ncap *= 2;
        }
        
        if (ncap > SIZE_MAX) {
            fprintf(stderr, "ERROR: New capacity %zu too large in arena_put\n", ncap);
            return YQ_ERR_NOMEM;
        }
        
        uint8_t *nb = (uint8_t *)realloc(*buf, ncap);
        if (!nb) {
            fprintf(stderr, "ERROR: realloc failed for arena expansion in arena_put\n");
            return YQ_ERR_NOMEM;
        }
        *buf = nb;
        *cap = ncap;
    }
    
    if (n > 0) {
        if (!src) {
            fprintf(stderr, "ERROR: NULL src parameter with n %zu in arena_put\n", n);
            return YQ_ERR_INVAL;
        }
        
        if (*used + n > *cap) {
            fprintf(stderr, "ERROR: Bounds check failed in arena_put: used %zu + n %zu > cap %zu\n", 
                    *used, n, *cap);
            return YQ_ERR_INVAL;
        }
        
        memcpy(*buf + *used, src, n);
    }
    
    *out_off = *used;
    *used += n;
    
    /* Validate final state */
    if (*used > *cap) {
        fprintf(stderr, "ERROR: Used %zu > cap %zu after arena_put\n", *used, *cap);
        return YQ_ERR_INVAL;
    }
    
    return YQ_OK;
}

static int ctx_push_op(recover_ctx *ctx, uint64_t txn_id, int type,
                       const uint8_t *key, size_t klen,
                       const uint8_t *val, size_t vlen) {
    /* Enhanced parameter validation with comprehensive checking */
    if (!ctx) {
        fprintf(stderr, "ERROR: NULL ctx parameter in ctx_push_op\n");
        return YQ_ERR_INVAL;
    }
    
    if (type != 2 && type != 3) {
        fprintf(stderr, "ERROR: Invalid type %d in ctx_push_op\n", type);
        return YQ_ERR_INVAL;
    }
    
    if (!key || klen == 0 || klen > 1024) {
        fprintf(stderr, "ERROR: Invalid key parameter in ctx_push_op: klen %zu\n", klen);
        return YQ_ERR_INVAL;
    }
    
    if (type == 2 && (!val || vlen > 1024 * 1024)) {
        fprintf(stderr, "ERROR: Invalid val parameter in ctx_push_op: vlen %zu\n", vlen);
        return YQ_ERR_INVAL;
    }
    
    /* Validate transaction ID */
    if (txn_id == 0) {
        fprintf(stderr, "ERROR: Zero txn_id in ctx_push_op\n");
        return YQ_ERR_INVAL;
    }
    
    if (ctx->ops_count >= ctx->ops_cap) {
        /* Validate capacity before expansion */
        size_t ncap = ctx->ops_cap ? ctx->ops_cap * 2 : 128;
        if (ncap > SIZE_MAX / sizeof(rec_op)) {
            fprintf(stderr, "ERROR: Capacity overflow in ctx_push_op: %zu * sizeof(rec_op)\n", ncap);
            return YQ_ERR_NOMEM;
        }
        
        rec_op *na = (rec_op *)realloc(ctx->ops, ncap * sizeof(rec_op));
        if (!na) {
            fprintf(stderr, "ERROR: realloc failed for ops array in ctx_push_op\n");
            return YQ_ERR_NOMEM;
        }
        ctx->ops = na;
        ctx->ops_cap = ncap;
    }

    size_t key_off = 0;
    int rc = arena_put(&ctx->keys, &ctx->keys_used, &ctx->keys_cap, key, klen, &key_off);
    if (rc != YQ_OK) {
        fprintf(stderr, "ERROR: arena_put failed for keys in ctx_push_op\n");
        return rc;
    }

    size_t val_off = 0;
    if (type == 2) {
        rc = arena_put(&ctx->vals, &ctx->vals_used, &ctx->vals_cap, val, vlen, &val_off);
        if (rc != YQ_OK) {
            fprintf(stderr, "ERROR: arena_put failed for vals in ctx_push_op\n");
            return rc;
        }
    }

    /* Validate index bounds before assignment */
    if (ctx->ops_count >= ctx->ops_cap) {
        fprintf(stderr, "ERROR: Index %zu >= cap %zu in ctx_push_op\n", ctx->ops_count, ctx->ops_cap);
        return YQ_ERR_INVAL;
    }
    
    rec_op *op = &ctx->ops[ctx->ops_count];
    op->txn_id = txn_id;
    op->type = type;
    op->key_off = key_off;
    op->key_len = klen;
    op->val_off = val_off;
    op->val_len = (type == 2) ? vlen : 0;
    ctx->ops_count++;
    
    /* Validate final state */
    if (ctx->ops_count > ctx->ops_cap) {
        fprintf(stderr, "ERROR: ops_count %zu > ops_cap %zu after ctx_push_op\n", 
                ctx->ops_count, ctx->ops_cap);
        return YQ_ERR_INVAL;
    }
    
    return YQ_OK;
}

static int recovery_visitor(void *ctx_arg, uint64_t lsn, uint64_t txn_id, int rec_type,
    const uint8_t *payload, size_t paylen) {
    recover_ctx *ctx = (recover_ctx *)ctx_arg;
    (void)lsn;

    /* Enhanced parameter validation with comprehensive checking */
    if (!ctx_arg) {
        fprintf(stderr, "ERROR: NULL ctx_arg in recovery_visitor\n");
        return YQ_ERR_INVAL;
    }
    
    if (txn_id == 0) {
        fprintf(stderr, "ERROR: Zero txn_id in recovery_visitor\n");
        return YQ_ERR_INVAL;
    }
    
    if (rec_type != 2 && rec_type != 3 && rec_type != 4) {
        fprintf(stderr, "ERROR: Invalid rec_type %d in recovery_visitor\n", rec_type);
        return YQ_ERR_INVAL;
    }
    
    if (rec_type == 2 || rec_type == 3) {
        if (!payload || paylen == 0) {
            fprintf(stderr, "ERROR: NULL payload or zero paylen %zu in recovery_visitor\n", paylen);
            return YQ_ERR_CORRUPT;
        }
        
        if (paylen > 1024 * 1024 * 10) {
            fprintf(stderr, "ERROR: paylen %zu too large in recovery_visitor\n", paylen);
            return YQ_ERR_CORRUPT;
        }
    }

    if (rec_type == 4) {
        return rc_hash_insert(&ctx->committed, txn_id);
    }

    if (rec_type == 2) {
        size_t kpos = 0;
        uint64_t klen = 0;
        if (yq_varint_decode(payload, paylen, &klen, &kpos) != YQ_OK) {
            fprintf(stderr, "ERROR: varint_decode failed for key length in recovery_visitor\n");
            return YQ_ERR_CORRUPT;
        }
        
        if (klen > 1024 || klen == 0) {
            fprintf(stderr, "ERROR: Invalid klen %llu in recovery_visitor\n", (unsigned long long)klen);
            return YQ_ERR_CORRUPT;
        }
        
        if (kpos + klen > paylen) {
            fprintf(stderr, "ERROR: Key out of bounds in recovery_visitor: kpos %zu + klen %llu > paylen %zu\n", 
                    kpos, (unsigned long long)klen, paylen);
            return YQ_ERR_CORRUPT;
        }
        
        size_t vpos_rel = 0;
        uint64_t vlen = 0;
        if (yq_varint_decode(payload + kpos + klen, paylen - kpos - klen, &vlen, &vpos_rel) != YQ_OK) {
            fprintf(stderr, "ERROR: varint_decode failed for value length in recovery_visitor\n");
            return YQ_ERR_CORRUPT;
        }
        
        size_t vstart = kpos + klen + vpos_rel;
        if (vstart + vlen > paylen) {
            fprintf(stderr, "ERROR: Value out of bounds in recovery_visitor: vstart %zu + vlen %llu > paylen %zu\n", 
                    vstart, (unsigned long long)vlen, paylen);
            return YQ_ERR_CORRUPT;
        }
        
        if (vlen > 1024 * 1024) {
            fprintf(stderr, "ERROR: vlen %llu too large in recovery_visitor\n", (unsigned long long)vlen);
            return YQ_ERR_CORRUPT;
        }
        
        return ctx_push_op(ctx, txn_id, 2, payload + kpos, (size_t)klen,
                           payload + vstart, (size_t)vlen);
    }

    if (rec_type == 3) {
        size_t kpos = 0;
        uint64_t klen = 0;
        if (yq_varint_decode(payload, paylen, &klen, &kpos) != YQ_OK) {
            fprintf(stderr, "ERROR: varint_decode failed for key length in recovery_visitor\n");
            return YQ_ERR_CORRUPT;
        }
        
        if (klen > 1024 || klen == 0) {
            fprintf(stderr, "ERROR: Invalid klen %llu in recovery_visitor\n", (unsigned long long)klen);
            return YQ_ERR_CORRUPT;
        }
        
        if (kpos + klen > paylen) {
            fprintf(stderr, "ERROR: Key out of bounds in recovery_visitor: kpos %zu + klen %llu > paylen %zu\n", 
                    kpos, (unsigned long long)klen, paylen);
            return YQ_ERR_CORRUPT;
        }
        
        return ctx_push_op(ctx, txn_id, 3, payload + kpos, (size_t)klen, NULL, 0);
    }

    return YQ_OK;
}

int yq_recover(yq_wal *wal, yq_memtable *mt) {
    /* Enhanced parameter validation with comprehensive checking */
    if (!wal || !mt) {
        fprintf(stderr, "ERROR: NULL wal or mt parameter in yq_recover\n");
        return YQ_ERR_INVAL;
    }
    
    if (!wal->db || !mt->db) {
        fprintf(stderr, "ERROR: NULL db pointer in wal or mt in yq_recover\n");
        return YQ_ERR_INVAL;
    }
    
    if (yq_wal_size(wal) == 0) {
        fprintf(stderr, "WARNING: Empty WAL in yq_recover\n");
        return YQ_OK;
    }

    recover_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.mt = mt;

    int rc = rc_hash_init(&ctx.committed);
    if (rc != YQ_OK) {
        fprintf(stderr, "ERROR: rc_hash_init failed in yq_recover\n");
        return rc;
    }

    rc = yq_wal_scan(wal, 0, recovery_visitor, &ctx);

    if (rc == YQ_OK || rc == YQ_ERR_CORRUPT) {
        /* Validate operations count before processing */
        if (ctx.ops_count > SIZE_MAX / sizeof(rec_op)) {
            fprintf(stderr, "ERROR: ops_count %zu too large in yq_recover\n", ctx.ops_count);
            rc_hash_destroy(&ctx.committed);
            free(ctx.ops);
            free(ctx.keys);
            free(ctx.vals);
            return YQ_ERR_INVAL;
        }
        
        for (size_t i = 0; i < ctx.ops_count; i++) {
            /* Validate index bounds */
            if (i >= ctx.ops_count) {
                fprintf(stderr, "ERROR: Index %zu >= ops_count %zu in yq_recover\n", i, ctx.ops_count);
                break;
            }
            
            rec_op *op = &ctx.ops[i];
            if (!rc_hash_contains(&ctx.committed, op->txn_id)) continue;
            
            /* Validate key offset and length */
            if (op->key_off > ctx.keys_used || op->key_len > ctx.keys_used - op->key_off) {
                fprintf(stderr, "ERROR: Invalid key offset/len in yq_recover: off %zu, len %zu, used %zu\n", 
                        op->key_off, op->key_len, ctx.keys_used);
                continue;
            }
            
            yq_slice k;
            k.data = ctx.keys ? ctx.keys + op->key_off : NULL;
            k.size = op->key_len;
            
            if (op->type == 2) {
                /* Validate value offset and length for PUT operations */
                if (op->val_off > ctx.vals_used || op->val_len > ctx.vals_used - op->val_off) {
                    fprintf(stderr, "ERROR: Invalid val offset/len in yq_recover: off %zu, len %zu, used %zu\n", 
                            op->val_off, op->val_len, ctx.vals_used);
                    continue;
                }
                
                yq_slice v;
                v.data = ctx.vals ? ctx.vals + op->val_off : NULL;
                v.size = op->val_len;
                yq_memtable_put(mt, k, v);
            } else if (op->type == 3) {
                yq_memtable_del(mt, k);
            }
        }
        rc = YQ_OK;
    }

    rc_hash_destroy(&ctx.committed);
    free(ctx.ops);
    free(ctx.keys);
    free(ctx.vals);

    return rc;
}

int yq_recover_replay(yq_recover_ctx *ctx) {
    /* Enhanced parameter validation with comprehensive checking */
    if (!ctx) {
        fprintf(stderr, "ERROR: NULL ctx parameter in yq_recover_replay\n");
        return YQ_ERR_INVAL;
    }
    
    if (!ctx->mt) {
        fprintf(stderr, "ERROR: NULL ctx->mt parameter in yq_recover_replay\n");
        return YQ_ERR_INVAL;
    }
    
    /* Validate operations count before processing */
    if (ctx->ops_count > SIZE_MAX / sizeof(rec_op)) {
        fprintf(stderr, "ERROR: ops_count %zu too large in yq_recover_replay\n", ctx->ops_count);
        return YQ_ERR_INVAL;
    }
    
    /* Replay all committed operations */
    for (size_t i = 0; i < ctx->ops_count; i++) {
        /* Validate index bounds */
        if (i >= ctx->ops_count) {
            fprintf(stderr, "ERROR: Index %zu >= ops_count %zu in yq_recover_replay\n", i, ctx->ops_count);
            break;
        }
        
        rec_op *op = &ctx->ops[i];
        if (!rc_hash_contains(&ctx->committed, op->txn_id)) continue;
        
        /* Validate key offset and length */
        if (op->key_off > ctx->keys_used || op->key_len > ctx->keys_used - op->key_off) {
            fprintf(stderr, "ERROR: Invalid key offset/len in yq_recover_replay: off %zu, len %zu, used %zu\n", 
                    op->key_off, op->key_len, ctx->keys_used);
            continue;
        }
        
        yq_slice k;
        k.data = ctx->keys ? ctx->keys + op->key_off : NULL;
        k.size = op->key_len;
        
        if (op->type == 2) {
            /* Validate value offset and length for PUT operations */
            if (op->val_off > ctx->vals_used || op->val_len > ctx->vals_used - op->val_off) {
                fprintf(stderr, "ERROR: Invalid val offset/len in yq_recover_replay: off %zu, len %zu, used %zu\n", 
                        op->val_off, op->val_len, ctx->vals_used);
                continue;
            }
            
            yq_slice v;
            v.data = ctx->vals ? ctx->vals + op->val_off : NULL;
            v.size = op->val_len;
            yq_memtable_put(ctx->mt, k, v);
        } else if (op->type == 3) {
            yq_memtable_del(ctx->mt, k);
        }
    }
    
    return YQ_OK;
}
