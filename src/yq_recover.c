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
    /*
     * Guard *used + n against size_t overflow. n is bounded by a single WAL
     * record payload (<= 1 GiB by yq_put), but *used accumulates the whole
     * replayed log, so on a 32-bit build the sum can wrap and let the growth
     * loop below settle on a far-too-small capacity, after which the memcpy
     * would write out of bounds.
     */
    if (n > SIZE_MAX - *used) return YQ_ERR_NOMEM;
    if (*buf == NULL || *used + n > *cap) {
        size_t ncap = *cap ? *cap : 4096;
        while (ncap < *used + n) {
            /* Guard ncap *= 2 against wrapping to 0 on a 32-bit platform. */
            if (ncap > SIZE_MAX / 2) return YQ_ERR_NOMEM;
            ncap *= 2;
        }
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

/*
 * P2: index build by sort, not by replay.
 *
 * The previous phase-2 loop replayed every committed operation into the
 * memtable one record at a time. That is O(n) inserts into an ordered
 * structure -- O(n^2) in the worst case (random key order in the log) --
 * and it dominated reopen for anything but trivial logs.
 *
 * Replay semantics only ever look at the FINAL state of each key, so the
 * per-record application order can be replaced by: collect the committed
 * operations, sort once by (key, log order), keep the last operation per
 * key, and hand the deduplicated run to yq_memtable_bulk_load(), which
 * builds the chunk layout directly. The observable result is identical --
 * last write wins per key, a trailing DEL hides earlier PUTs -- but the
 * cost is one sort (O(n log n)) plus a linear bulk build.
 *
 * Bytes are referenced in place from the scan arenas; the bulk load copies
 * them into the memtable arena, after which both areas are released by the
 * caller.
 */
typedef struct rec_candidate {
    const uint8_t *key;
    const uint8_t *val;               /* NULL for DEL */
    size_t key_len;
    size_t val_len;
    int is_del;
    size_t seq;                       /* position in the scan order */
} rec_candidate;

/* Byte-string order: memcmp over the common prefix, then shorter first.
 * Matches yq_slice_compare_raw(). */
static int key_order_cmp(const uint8_t *a, size_t alen,
                         const uint8_t *b, size_t blen) {
    size_t min = alen < blen ? alen : blen;
    if (min) {
        int c = memcmp(a, b, min);
        if (c) return c < 0 ? -1 : 1;
    }
    if (alen != blen) return alen < blen ? -1 : 1;
    return 0;
}

static int candidate_cmp(const void *pa, const void *pb) {
    const rec_candidate *a = (const rec_candidate *)pa;
    const rec_candidate *b = (const rec_candidate *)pb;

    int c = key_order_cmp(a->key, a->key_len, b->key, b->key_len);
    if (c) return c;
    /* Same key: log order decides, so the LAST element of a run is the
     * operation that must survive. Totally ordered -> qsort stays honest. */
    if (a->seq != b->seq) return a->seq < b->seq ? -1 : 1;
    return 0;
}

static int recover_build_index(yq_memtable *mt, recover_ctx *ctx) {
    int rc = YQ_OK;
    rec_candidate *cand = NULL;
    yq_bulk_entry *bulk = NULL;

    /* 1. Collect the committed operations in scan (log) order. */
    size_t ncommit = 0;
    for (size_t i = 0; i < ctx->ops_count; i++) {
        if (rc_hash_contains(&ctx->committed, ctx->ops[i].txn_id)) ncommit++;
    }
    if (ncommit == 0) return YQ_OK;

    cand = (rec_candidate *)malloc(ncommit * sizeof(rec_candidate));
    if (!cand) return YQ_ERR_NOMEM;

    size_t w = 0;
    for (size_t i = 0; i < ctx->ops_count; i++) {
        const rec_op *op = &ctx->ops[i];
        if (!rc_hash_contains(&ctx->committed, op->txn_id)) continue;

        rec_candidate *c = &cand[w];
        c->key = ctx->keys ? ctx->keys + op->key_off : NULL;
        c->key_len = op->key_len;
        c->is_del = (op->type == 3);
        c->val = c->is_del ? NULL
                           : (ctx->vals ? ctx->vals + op->val_off : NULL);
        c->val_len = c->is_del ? 0 : op->val_len;
        c->seq = w++;
    }

    /* 2. One sort by (key, log order). */
    qsort(cand, ncommit, sizeof(rec_candidate), candidate_cmp);

    /*
     * 3. Keep the last operation per key: runs of equal keys are contiguous
     *    after the sort, and the run's final element carries the highest
     *    sequence number (the latest write in log order).
     */
    bulk = (yq_bulk_entry *)malloc(ncommit * sizeof(yq_bulk_entry));
    if (!bulk) { free(cand); return YQ_ERR_NOMEM; }

    size_t nb = 0;
    for (size_t i = 0; i < ncommit; ) {
        size_t j = i;
        while (j + 1 < ncommit &&
               key_order_cmp(cand[j + 1].key, cand[j + 1].key_len,
                             cand[i].key, cand[i].key_len) == 0) {
            j++;
        }
        const rec_candidate *win = &cand[j];   /* last write for this key */
        bulk[nb].key.data = win->key;
        bulk[nb].key.size = win->key_len;
        bulk[nb].val.data = win->is_del ? NULL : win->val;
        bulk[nb].val.size = win->is_del ? 0 : win->val_len;
        bulk[nb].tombstone = win->is_del;
        nb++;
        i = j + 1;
    }

    /* 4. Build the index in one pass. */
    rc = yq_memtable_bulk_load(mt, bulk, nb);

    free(bulk);
    free(cand);
    return rc;
}

int yq_recover(yq_wal *wal, yq_memtable *mt) {
    if (!wal || !mt) return YQ_ERR_INVAL;
    if (yq_wal_size(wal) == 0) return YQ_OK;

    recover_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.mt = mt;

    int rc = rc_hash_init(&ctx.committed);
    if (rc != YQ_OK) return rc;

    /*
     * Two changes make a live dataset larger than memtable_bytes replayable:
     *
     *  1. Grow the arena to fit at most the whole log. The replayed live set
     *     can never exceed the bytes that were appended, so the log size is
     *     a safe upper bound; without this the arena (2x memtable_bytes)
     *     would run out mid-replay and the database would stay unopenable.
     *  2. Lift the max_bytes admission check (see
     *     yq_memtable_set_limit_enforcement).
     *
     * Both matter: failing to open the database is data loss caused by a
     * memory threshold, which is strictly worse than exceeding it.
     */
    rc = yq_memtable_reserve(mt, (size_t)yq_wal_size(wal));
    if (rc != YQ_OK) return rc;
    yq_memtable_set_limit_enforcement(mt, 0);

    rc = yq_wal_scan(wal, 0, recovery_visitor, &ctx);

    if (rc == YQ_OK || rc == YQ_ERR_CORRUPT) {
        int replay_rc = recover_build_index(mt, &ctx);
        if (replay_rc != YQ_OK) {
            rc = replay_rc;
        } else if (rc == YQ_ERR_CORRUPT) {
            rc = YQ_OK;
        }
    }

    rc_hash_destroy(&ctx.committed);
    free(ctx.ops);
    free(ctx.keys);
    free(ctx.vals);

    /* Restore the write path's admission check before handing the handle over. */
    yq_memtable_set_limit_enforcement(mt, 1);

    return rc;
}

int yq_recover_replay(yq_recover_ctx *ctx) {
    (void)ctx;
    return YQ_OK;
}
