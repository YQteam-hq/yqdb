#include "yq.h"
#include "yq_vfs.h"
#include "yq_btree.h"
#include "yq_wal.h"
#include "yq_memtable.h"
#include "yq_mvcc.h"
#include "yq_slice.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <errno.h>

extern int yq_recover(yq_wal *wal, yq_memtable *mt);

static _Thread_local int g_last_io_err = 0;

struct yq_db {
    yq_opts opts;
    yq_file *db_file;
    yq_file *wal_file;
    yq_file *shm_file;
    yq_file *lock_file;
    yq_mvcc *mvcc;
    yq_wal *wal;
    yq_btree *btree;
    yq_memtable *memtable;
    void *mmap_base;
    size_t mmap_len;
    int write_enabled;
    int closed;
};

typedef struct pending_op {
    uint8_t *key;
    size_t key_len;
    uint8_t *val;
    size_t val_len;
    int is_del;
} pending_op;

struct yq_txn {
    yq_db *db;
    uint32_t flags;
    uint64_t snapshot_txn;
    uint64_t snapshot_root;
    int slot_idx;
    int state;
    int first_write;
    pending_op *pending;
    size_t pending_count;
    size_t pending_cap;
};

struct yq_cur {
    yq_txn *txn;
    yq_btree_cursor *bt_cur;
    yq_memtable_iter *mt_iter;
    int state;
    int at_end;
};

#define YQ_TXN_STATE_ACTIVE    0
#define YQ_TXN_STATE_COMMITTED 1
#define YQ_TXN_STATE_ABORTED   2

int yq_version(int *major, int *minor, int *patch) {
    if (major) *major = 1;
    if (minor) *minor = 0;
    if (patch) *patch = 0;
    return YQ_OK;
}

static const char *g_err_msgs[] = {
    "success",
    "generic error",
    "out of memory",
    "I/O error",
    "data corruption",
    "incompatible version",
    "key not found",
    "key exists",
    "resource busy",
    "read-only",
    "invalid argument",
    "value too big",
    "transaction closed",
    "transaction broken",
    "invalid cursor",
    "no space left",
    "mmap region full",
    "reader slots full",
    "not supported",
    "operation timed out",
    "internal panic"
};

const char *yq_strerror(int rc) {
    if (rc < 0 || rc > 20) return "unknown error";
    return g_err_msgs[rc];
}

int yq_last_io_error(void) {
    return g_last_io_err;
}

static void set_io_err(int err) {
    if (err != 0) g_last_io_err = err;
}

/* Bitwise OR of every YQ_OPEN_* flag defined in yq.h. */
#define YQ_OPEN_KNOWN_FLAGS 0x0000001Fu

/*
 * Enforce the contract documented on yq_opts before any file is touched.
 * Everything rejected here is stated as a hard requirement in yq.h, and all of
 * it used to be accepted silently: a non power-of-two page_size corrupts page
 * arithmetic, a too-small map_size leaves the meta pages outside the mapping,
 * and a non-zero reserved[] breaks forward compatibility (those words are
 * reserved so a future version can give them meaning).
 */
static int validate_opts(const yq_opts *opts) {
    if (opts->flags & ~YQ_OPEN_KNOWN_FLAGS) return YQ_ERR_INVAL;

    if (opts->page_size != 0) {
        if (opts->page_size < 4096 || opts->page_size > 65536) return YQ_ERR_INVAL;
        if ((opts->page_size & (opts->page_size - 1)) != 0) return YQ_ERR_INVAL;
    }

    if (opts->sync_mode > YQ_SYNC_FULL) return YQ_ERR_INVAL;
    if (opts->max_readers > 65535u) return YQ_ERR_INVAL;

    /* The two meta pages live at the start of the mapping. */
    if (opts->map_size != 0) {
        uint64_t min_map = 2ull * (opts->page_size ? (uint64_t)opts->page_size : 4096ull);
        if (opts->map_size < min_map) return YQ_ERR_INVAL;
    }

    for (size_t i = 0; i < sizeof(opts->reserved) / sizeof(opts->reserved[0]); i++) {
        if (opts->reserved[i] != 0) return YQ_ERR_INVAL;
    }

    return YQ_OK;
}

static int apply_defaults(yq_opts *opts) {
    if (opts->page_size == 0) opts->page_size = 4096;
    else if (opts->page_size < 4096 || opts->page_size > 65536) return YQ_ERR_INVAL;
    if (opts->sync_mode == YQ_SYNC_DEFAULT) opts->sync_mode = YQ_SYNC_NORMAL;
    if (opts->map_size == 0) opts->map_size = 1024ULL * 1024 * 1024;
    if (opts->memtable_bytes == 0) opts->memtable_bytes = 64ULL * 1024 * 1024;
    if (opts->log_bytes == 0) opts->log_bytes = 256ULL * 1024 * 1024;
    if (opts->max_readers == 0) opts->max_readers = 126;
    return YQ_OK;
}

static void free_db(yq_db *db) {
    if (!db) return;
    if (db->mmap_base && db->mmap_len > 0) {
        yq_file_munmap(db->mmap_base, db->mmap_len);
    }
    if (db->memtable) yq_memtable_destroy(db->memtable);
    if (db->btree) {
    }
    if (db->wal) yq_wal_close(db->wal);
    if (db->mvcc) yq_mvcc_close(db->mvcc);
    if (db->lock_file) yq_file_close(db->lock_file);
    if (db->shm_file) yq_file_close(db->shm_file);
    if (db->wal_file) yq_file_close(db->wal_file);
    if (db->db_file) yq_file_close(db->db_file);
    free(db);
}

int yq_open(const char *path, const yq_opts *opts, yq_db **out) {
    *out = NULL;
    if (!opts || opts->struct_size != sizeof(yq_opts)) return YQ_ERR_INVAL;
    if (!path || path[0] == '\0') return YQ_ERR_INVAL;

    int rc = validate_opts(opts);
    if (rc != YQ_OK) return rc;

    yq_opts def = *opts;
    rc = apply_defaults(&def);
    if (rc != YQ_OK) return rc;

    yq_db *db = calloc(1, sizeof(yq_db));
    if (!db) return YQ_ERR_NOMEM;

    db->opts = def;
    db->write_enabled = !(def.flags & YQ_OPEN_READONLY);

    char db_path_buf[1024];
    size_t plen = strlen(path);
    if (plen >= sizeof(db_path_buf)) { free(db); return YQ_ERR_INVAL; }
    memcpy(db_path_buf, path, plen + 1);

    int create_file = (def.flags & YQ_OPEN_CREATE) ? 1 : 0;

    db->db_file = yq_file_open(db_path_buf, create_file, db->write_enabled ? 1 : 0);
    if (!db->db_file) { set_io_err(errno); free_db(db); return YQ_ERR_IO; }

    uint64_t fsize = yq_file_size(db->db_file);

    /*
     * YQ_OPEN_EXCL: fail if the database already exists. Checked before any
     * auxiliary file is created so a rejected open leaves nothing behind, and
     * before recovery runs so an existing database is never modified.
     */
    if ((def.flags & YQ_OPEN_EXCL) && fsize > 0) {
        free_db(db);
        return YQ_ERR_EXISTS;
    }

    char shm_path[1024];
    snprintf(shm_path, sizeof(shm_path), "%s.shm", db_path_buf);
    db->shm_file = yq_file_open(shm_path, 1, 1);
    if (!db->shm_file) { set_io_err(errno); free_db(db); return YQ_ERR_IO; }

    char lock_path[1024];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", db_path_buf);
    db->lock_file = yq_file_open(lock_path, 1, 1);
    if (!db->lock_file) { set_io_err(errno); free_db(db); return YQ_ERR_IO; }

    rc = yq_mvcc_open(&db->mvcc, db->db_file, db->shm_file, db->lock_file, def.max_readers);
    if (rc != YQ_OK) { free_db(db); return rc; }

    uint64_t txn_id = 0, root_page = 0, free_head = 0, npages = 0, ckpt_lsn = 0;
    int has_existing = (fsize > 0) ? 1 : 0;
    if (has_existing) {
        rc = yq_mvcc_meta_read(db->mvcc, &txn_id, &root_page, &free_head, &npages, &ckpt_lsn);
        if (rc != YQ_OK) { free_db(db); return rc; }
        yq_mvcc_set_base_txn(db->mvcc, txn_id);
    }

    if (db->write_enabled && fsize == 0 && (def.flags & YQ_OPEN_CREATE)) {
        uint64_t init_size = (uint64_t)def.page_size * 2;
        if (yq_file_truncate(db->db_file, init_size) != YQ_OK) { free_db(db); return YQ_ERR_IO; }
        fsize = init_size;
        has_existing = 1;
        npages = 2;
        txn_id = 1;
        root_page = 0;

        rc = yq_mvcc_meta_pwrite_full(db->mvcc, txn_id, root_page, free_head, npages, ckpt_lsn);
        if (rc != YQ_OK) { free_db(db); return rc; }
        yq_file_sync(db->db_file);
    }

    db->mmap_len = 0;
    if (fsize > 0) {
        size_t map_len = (size_t)(fsize < (uint64_t)def.map_size ? fsize : (uint64_t)def.map_size);
        db->mmap_base = yq_file_mmap(db->db_file, 0, map_len);
        db->mmap_len = map_len;
    }

    rc = yq_wal_open(&db->wal, db_path_buf, def.page_size);
    if (rc != YQ_OK) { free_db(db); return rc; }

    if (has_existing && db->mmap_base) {
        rc = yq_btree_open(&db->btree, db->mmap_base, fsize, def.page_size);
        if (rc != YQ_OK) { free_db(db); return rc; }
        yq_btree_set_npages(db->btree, npages);
        yq_btree_set_root(db->btree, root_page);
    } else {
        void *arena = db->mmap_base;
        if (!arena) arena = malloc(def.map_size);
        if (!arena) { free_db(db); return YQ_ERR_NOMEM; }
        db->btree = yq_btree_create(arena, def.page_size);
        if (!db->btree) { if (!db->mmap_base) free(arena); free_db(db); return YQ_ERR_NOMEM; }
    }

    db->memtable = yq_memtable_create((size_t)def.memtable_bytes);
    if (!db->memtable) { free_db(db); return YQ_ERR_NOMEM; }

    if (db->write_enabled) {
        rc = yq_recover(db->wal, db->memtable);
        if (rc != YQ_OK) { free_db(db); return rc; }
    }

    *out = db;
    return YQ_OK;
}

int yq_close(yq_db *db) {
    if (!db) return YQ_OK;
    if (db->closed) { free_db(db); return YQ_OK; }
    db->closed = 1;
    if (db->write_enabled) yq_sync(db);
    free_db(db);
    return YQ_OK;
}

static pending_op *pending_find(yq_txn *txn, yq_slice key, size_t *idx_out) {
    for (size_t i = txn->pending_count; i > 0; i--) {
        pending_op *op = &txn->pending[i - 1];
        if (op->key_len == key.size && memcmp(op->key, key.data, key.size) == 0) {
            if (idx_out) *idx_out = i - 1;
            return op;
        }
    }
    if (idx_out) *idx_out = (size_t)-1;
    return NULL;
}

static int pending_push(yq_txn *txn, yq_slice key, yq_slice val, int is_del) {
    if (txn->pending_count >= txn->pending_cap) {
        size_t nc = txn->pending_cap ? txn->pending_cap * 2 : 64;
        pending_op *na = realloc(txn->pending, nc * sizeof(pending_op));
        if (!na) return YQ_ERR_NOMEM;
        txn->pending = na;
        txn->pending_cap = nc;
    }
    pending_op *op = &txn->pending[txn->pending_count];
    op->key = malloc(key.size ? key.size : 1);
    if (!op->key) return YQ_ERR_NOMEM;
    memcpy(op->key, key.data, key.size);
    op->key_len = key.size;
    op->val = NULL;
    op->val_len = 0;
    op->is_del = is_del;
    if (!is_del) {
        op->val = malloc(val.size ? val.size : 1);
        if (!op->val) { free(op->key); return YQ_ERR_NOMEM; }
        memcpy(op->val, val.data, val.size);
        op->val_len = val.size;
    }
    txn->pending_count++;
    return YQ_OK;
}

static void pending_free(yq_txn *txn) {
    if (!txn->pending) return;
    for (size_t i = 0; i < txn->pending_count; i++) {
        free(txn->pending[i].key);
        free(txn->pending[i].val);
    }
    free(txn->pending);
    txn->pending = NULL;
    txn->pending_count = 0;
    txn->pending_cap = 0;
}

static int pending_apply(yq_txn *txn) {
    yq_db *db = txn->db;
    int rc = YQ_OK;
    for (size_t i = 0; i < txn->pending_count; i++) {
        pending_op *op = &txn->pending[i];
        yq_slice k;
        yq_slice_set(&k, op->key, op->key_len);
        if (op->is_del) {
            yq_memtable_del(db->memtable, k);
        } else {
            yq_slice v;
            yq_slice_set(&v, op->val, op->val_len);
            int r = yq_memtable_put(db->memtable, k, v);
            if (r != YQ_OK && rc == YQ_OK) rc = r;
        }
    }
    return rc;
}

int yq_txn_begin(yq_db *db, uint32_t flags, yq_txn **out) {
    if (!db || !out) return YQ_ERR_INVAL;
    *out = NULL;
    if (db->closed) return YQ_ERR_CORRUPT;

    yq_txn *txn = calloc(1, sizeof(yq_txn));
    if (!txn) return YQ_ERR_NOMEM;

    txn->db = db;
    txn->flags = flags;
    txn->state = YQ_TXN_STATE_ACTIVE;
    txn->slot_idx = -1;
    txn->first_write = (flags & YQ_TXN_READWRITE) ? 1 : 0;

    if (flags & YQ_TXN_READONLY) {
        uint64_t txn_id = 0, root = 0;
        yq_mvcc_meta_read(db->mvcc, &txn_id, &root, NULL, NULL, NULL);
        int rc = yq_mvcc_acquire_snapshot(db->mvcc, txn_id, root, &txn->snapshot_txn, &txn->snapshot_root, &txn->slot_idx);
        if (rc != YQ_OK) { free(txn); return rc; }
    } else if (flags & YQ_TXN_READWRITE) {
        int got = 0;
        int rc = yq_mvcc_elect_writer(db->mvcc, (int)db->opts.lock_timeout_ms, &got);
        if (rc != YQ_OK) { free(txn); return rc; }
        if (!got) { free(txn); return YQ_ERR_BUSY; }
        uint64_t txn_id = 0, root = 0;
        yq_mvcc_meta_read(db->mvcc, &txn_id, &root, NULL, NULL, NULL);
        rc = yq_mvcc_acquire_snapshot(db->mvcc, txn_id, root, &txn->snapshot_txn, &txn->snapshot_root, &txn->slot_idx);
        if (rc != YQ_OK) { yq_mvcc_release_writer(db->mvcc); free(txn); return rc; }
    }

    *out = txn;
    return YQ_OK;
}

int yq_txn_commit(yq_txn *txn) {
    if (!txn) return YQ_ERR_INVAL;
    if (txn->state != YQ_TXN_STATE_ACTIVE) return YQ_ERR_TXN_CLOSED;

    yq_db *db = txn->db;

    if (txn->flags & YQ_TXN_READWRITE) {
        int rc = yq_wal_append_commit(db->wal, txn->snapshot_txn);
        if (rc != YQ_OK) {
            txn->state = YQ_TXN_STATE_ABORTED;
            pending_free(txn);
            yq_mvcc_release_writer(db->mvcc);
            if (txn->slot_idx >= 0) yq_mvcc_release_snapshot(db->mvcc, txn->slot_idx);
            free(txn);
            return rc;
        }
        yq_wal_flush(db->wal);
        if (db->opts.sync_mode == YQ_SYNC_FULL && db->db_file) yq_file_sync(db->db_file);

        int rc_apply = pending_apply(txn);
        pending_free(txn);
        if (rc_apply != YQ_OK) {
            txn->state = YQ_TXN_STATE_ABORTED;
            yq_mvcc_release_writer(db->mvcc);
            if (txn->slot_idx >= 0) yq_mvcc_release_snapshot(db->mvcc, txn->slot_idx);
            free(txn);
            return rc_apply;
        }

        uint64_t new_txn_id = 0;
        int rc2 = yq_mvcc_increment_txn_id(db->mvcc, &new_txn_id);
        if (rc2 != YQ_OK) { txn->state = YQ_TXN_STATE_ABORTED; yq_mvcc_release_writer(db->mvcc); if (txn->slot_idx >= 0) yq_mvcc_release_snapshot(db->mvcc, txn->slot_idx); free(txn); return rc2; }

        uint64_t cur_txn = 0, cur_root = 0, cur_free = 0, cur_npages = 0, cur_ckpt = 0;
        yq_mvcc_meta_read(db->mvcc, &cur_txn, &cur_root, &cur_free, &cur_npages, &cur_ckpt);

        uint64_t root_page = cur_root;
        if (db->btree) {
            yq_btree_get_root(db->btree, &root_page);
            cur_npages = yq_btree_npages(db->btree);
        }

        rc2 = yq_mvcc_meta_pwrite_full(db->mvcc, new_txn_id, root_page, cur_free, cur_npages, cur_ckpt);

        yq_mvcc_release_writer(db->mvcc);
    }

    txn->state = YQ_TXN_STATE_COMMITTED;
    if (txn->slot_idx >= 0) yq_mvcc_release_snapshot(db->mvcc, txn->slot_idx);
    free(txn);
    return YQ_OK;
}

int yq_txn_abort(yq_txn *txn) {
    if (!txn) return YQ_OK;
    if (txn->state != YQ_TXN_STATE_ACTIVE) return YQ_ERR_TXN_CLOSED;
    yq_db *db = txn->db;
    if (txn->flags & YQ_TXN_READWRITE) {
        yq_wal_append_abort(db->wal, txn->snapshot_txn);
        yq_mvcc_release_writer(db->mvcc);
    }
    pending_free(txn);
    txn->state = YQ_TXN_STATE_ABORTED;
    if (txn->slot_idx >= 0) yq_mvcc_release_snapshot(db->mvcc, txn->slot_idx);
    free(txn);
    return YQ_OK;
}

int yq_put(yq_txn *txn, yq_slice key, yq_slice val, uint32_t mode) {
    if (!txn) return YQ_ERR_INVAL;
    if (txn->state != YQ_TXN_STATE_ACTIVE) return YQ_ERR_TXN_CLOSED;
    if (txn->flags & YQ_TXN_READONLY) return YQ_ERR_READONLY;
    if (key.size == 0 || key.size > 1024) return YQ_ERR_TOOBIG;
    if (val.size > (1ULL * 1024 * 1024 * 1024)) return YQ_ERR_TOOBIG;

    yq_db *db = txn->db;

    if (mode == YQ_PUT_NOOVERWRITE) {
        pending_op *p = pending_find(txn, key, NULL);
        if (p) {
            if (!p->is_del) return YQ_ERR_EXISTS;
        } else {
            yq_slice ex = {0};
            if (yq_memtable_get(db->memtable, key, &ex) == YQ_OK) return YQ_ERR_EXISTS;
            if (db->btree && yq_btree_lookup(db->btree, key, &ex) == YQ_OK) return YQ_ERR_EXISTS;
        }
    }

    if (txn->first_write) {
        int r = yq_wal_append_begin(db->wal, txn->snapshot_txn);
        if (r != YQ_OK) return r;
        txn->first_write = 0;
    }

    int rc = yq_wal_append_put(db->wal, txn->snapshot_txn, key, val);
    if (rc != YQ_OK) return rc;

    return pending_push(txn, key, val, 0);
}

int yq_del(yq_txn *txn, yq_slice key) {
    if (!txn) return YQ_ERR_INVAL;
    if (txn->state != YQ_TXN_STATE_ACTIVE) return YQ_ERR_TXN_CLOSED;
    if (txn->flags & YQ_TXN_READONLY) return YQ_ERR_READONLY;
    if (key.size == 0 || key.size > 1024) return YQ_ERR_INVAL;

    yq_db *db = txn->db;
    yq_slice empty;
    yq_slice_set(&empty, NULL, 0);

    if (txn->first_write) {
        int r = yq_wal_append_begin(db->wal, txn->snapshot_txn);
        if (r != YQ_OK) return r;
        txn->first_write = 0;
    }

    int rc = yq_wal_append_del(db->wal, txn->snapshot_txn, key);
    if (rc != YQ_OK) return rc;

    return pending_push(txn, key, empty, 1);
}

int yq_get(yq_txn *txn, yq_slice key, yq_slice *out) {
    if (!txn || !out) return YQ_ERR_INVAL;
    if (txn->state != YQ_TXN_STATE_ACTIVE) return YQ_ERR_TXN_CLOSED;
    out->data = NULL;
    out->size = 0;

    yq_db *db = txn->db;

    pending_op *p = pending_find(txn, key, NULL);
    if (p) {
        if (p->is_del) return YQ_ERR_NOTFOUND;
        out->data = p->val;
        out->size = p->val_len;
        return YQ_OK;
    }

    int rc = yq_memtable_get(db->memtable, key, out);
    if (rc == YQ_OK) return YQ_OK;

    if (db->btree) {
        rc = yq_btree_lookup(db->btree, key, out);
        if (rc == YQ_OK) return YQ_OK;
    }

    return YQ_ERR_NOTFOUND;
}

int yq_cur_open(yq_txn *txn, yq_cur **out) {
    if (!txn || !out) return YQ_ERR_INVAL;
    if (txn->state != YQ_TXN_STATE_ACTIVE) return YQ_ERR_TXN_CLOSED;

    yq_cur *c = calloc(1, sizeof(yq_cur));
    if (!c) return YQ_ERR_NOMEM;

    c->txn = txn;
    c->state = 0;
    c->at_end = 0;

    yq_db *db = txn->db;

    if (db->btree) {
        int rc = yq_btree_cursor_open(db->btree, &c->bt_cur);
        if (rc != YQ_OK) { free(c); return rc; }
    }

    int rc = yq_memtable_iter_open(db->memtable, &c->mt_iter);
    if (rc != YQ_OK) { if (c->bt_cur) yq_btree_cursor_close(c->bt_cur); free(c); return rc; }

    *out = c;
    return YQ_OK;
}

int yq_cur_first(yq_cur *c) {
    if (!c) return YQ_ERR_INVAL;
    c->at_end = 0; c->state = 0;
    if (c->mt_iter) {
        int rc = yq_memtable_iter_first(c->mt_iter);
        if (rc == YQ_OK && yq_memtable_iter_valid(c->mt_iter)) { c->state = 1; return YQ_OK; }
    }
    if (c->bt_cur) {
        int rc = yq_btree_cursor_first(c->bt_cur);
        if (rc == YQ_OK && yq_btree_cursor_valid(c->bt_cur)) { c->state = 2; return YQ_OK; }
    }
    c->at_end = 1; return YQ_ERR_NOTFOUND;
}

int yq_cur_last(yq_cur *c) {
    if (!c) return YQ_ERR_INVAL;
    c->at_end = 0; c->state = 0;
    if (c->bt_cur) {
        int rc = yq_btree_cursor_last(c->bt_cur);
        if (rc == YQ_OK && yq_btree_cursor_valid(c->bt_cur)) { c->state = 2; return YQ_OK; }
    }
    if (c->mt_iter) {
        int rc = yq_memtable_iter_last(c->mt_iter);
        if (rc == YQ_OK && yq_memtable_iter_valid(c->mt_iter)) { c->state = 1; return YQ_OK; }
    }
    c->at_end = 1; return YQ_ERR_NOTFOUND;
}

int yq_cur_next(yq_cur *c) {
    if (!c) return YQ_ERR_INVAL;
    if (c->at_end) return YQ_ERR_NOTFOUND;
    if (c->state == 1) {
        if (c->mt_iter) {
            int rc = yq_memtable_iter_next(c->mt_iter);
            if (rc == YQ_OK && yq_memtable_iter_valid(c->mt_iter)) { c->state = 1; return YQ_OK; }
        }
        if (c->bt_cur) {
            int rc = yq_btree_cursor_first(c->bt_cur);
            if (rc == YQ_OK && yq_btree_cursor_valid(c->bt_cur)) { c->state = 2; return YQ_OK; }
        }
        c->at_end = 1; c->state = 0; return YQ_ERR_NOTFOUND;
    }
    if (c->state == 2) {
        if (c->bt_cur) {
            int rc = yq_btree_cursor_next(c->bt_cur);
            if (rc == YQ_OK && yq_btree_cursor_valid(c->bt_cur)) { c->state = 2; return YQ_OK; }
        }
        c->at_end = 1; c->state = 0; return YQ_ERR_NOTFOUND;
    }
    return YQ_ERR_CURSOR;
}

int yq_cur_prev(yq_cur *c) {
    if (!c) return YQ_ERR_INVAL;
    if (c->at_end) return yq_cur_last(c);
    if (c->state == 2) {
        if (c->bt_cur) {
            int rc = yq_btree_cursor_prev(c->bt_cur);
            if (rc == YQ_OK && yq_btree_cursor_valid(c->bt_cur)) { c->state = 2; return YQ_OK; }
        }
        if (c->mt_iter) {
            int rc = yq_memtable_iter_last(c->mt_iter);
            if (rc == YQ_OK && yq_memtable_iter_valid(c->mt_iter)) { c->state = 1; return YQ_OK; }
        }
        c->at_end = 1; c->state = 0; return YQ_ERR_NOTFOUND;
    }
    if (c->state == 1) {
        if (c->mt_iter) {
            int rc = yq_memtable_iter_prev(c->mt_iter);
            if (rc == YQ_OK && yq_memtable_iter_valid(c->mt_iter)) { c->state = 1; return YQ_OK; }
        }
        c->at_end = 1; c->state = 0; return YQ_ERR_NOTFOUND;
    }
    return YQ_ERR_CURSOR;
}

int yq_cur_seek(yq_cur *c, yq_slice key) {
    if (!c) return YQ_ERR_INVAL;

    /* Position the tree cursor at the first key >= target (binary descent). */
    int tree_found = 0;
    if (c->bt_cur) {
        int rc = yq_btree_cursor_seek(c->bt_cur, key);
        if (rc == YQ_OK) tree_found = 1;
        else if (rc != YQ_ERR_NOTFOUND) return rc;
    }

    /* Position the memtable iterator at the first key >= target. */
    int mt_found = 0;
    if (c->mt_iter) {
        int rc = yq_memtable_iter_first(c->mt_iter);
        while (rc == YQ_OK) {
            yq_slice k;
            if (yq_memtable_iter_key(c->mt_iter, &k) != YQ_OK) break;
            if (yq_slice_compare(&k, &key) >= 0) { mt_found = 1; break; }
            rc = yq_memtable_iter_next(c->mt_iter);
        }
    }

    /* Merge order visits the memtable first, then the tree; pick the first
     * iterator that is positioned at a key >= target. */
    if (mt_found) { c->state = 1; c->at_end = 0; return YQ_OK; }
    if (tree_found) { c->state = 2; c->at_end = 0; return YQ_OK; }
    c->state = 0; c->at_end = 1;
    return YQ_ERR_NOTFOUND;
}

int yq_cur_seek_exact(yq_cur *c, yq_slice key) {
    if (!c) return YQ_ERR_INVAL;

    /* Position the tree cursor at the first key >= target (binary descent). */
    int tree_found = 0;
    if (c->bt_cur) {
        int rc = yq_btree_cursor_seek(c->bt_cur, key);
        if (rc == YQ_OK) tree_found = 1;
        else if (rc != YQ_ERR_NOTFOUND) return rc;
    }

    /* Position the memtable iterator at the first key >= target. */
    int mt_found = 0;
    if (c->mt_iter) {
        int rc = yq_memtable_iter_first(c->mt_iter);
        while (rc == YQ_OK) {
            yq_slice k;
            if (yq_memtable_iter_key(c->mt_iter, &k) != YQ_OK) break;
            if (yq_slice_compare(&k, &key) >= 0) { mt_found = 1; break; }
            rc = yq_memtable_iter_next(c->mt_iter);
        }
    }

    /* Success requires an exact match on the iterator chosen by merge order
     * (memtable first, then tree); anything else is a miss. */
    if (mt_found) {
        yq_slice k;
        if (yq_memtable_iter_key(c->mt_iter, &k) == YQ_OK && yq_slice_compare(&k, &key) == 0) {
            c->state = 1; c->at_end = 0; return YQ_OK;
        }
        c->state = 0; c->at_end = 1; return YQ_ERR_NOTFOUND;
    }
    if (tree_found) {
        yq_slice k;
        if (yq_btree_cursor_key(c->bt_cur, &k) == YQ_OK && yq_slice_compare(&k, &key) == 0) {
            c->state = 2; c->at_end = 0; return YQ_OK;
        }
    }
    c->state = 0; c->at_end = 1;
    return YQ_ERR_NOTFOUND;
}

int yq_cur_seek_le(yq_cur *c, yq_slice key) {
    if (!c) return YQ_ERR_INVAL;
    int rc = yq_cur_seek(c, key);
    if (rc == YQ_OK) {
        yq_slice k;
        /* Exact hit: the located key already satisfies `<= target`. */
        if (yq_cur_key(c, &k) == YQ_OK && yq_slice_compare(&k, &key) == 0) return YQ_OK;
        /* First key is > target, so step back to the last key < target. */
        return yq_cur_prev(c);
    }
    /* No key >= target means target is greater than every key: the answer is
     * the last (largest) key. */
    return yq_cur_last(c);
}

int yq_cur_valid(const yq_cur *c) {
    if (!c) return 0;
    return c->state && !c->at_end;
}

int yq_cur_key(const yq_cur *c, yq_slice *out) {
    if (!c || !out) return YQ_ERR_INVAL;
    if (!yq_cur_valid(c)) return YQ_ERR_CURSOR;
    if (c->state == 1 && c->mt_iter) return yq_memtable_iter_key(c->mt_iter, out);
    if (c->state == 2 && c->bt_cur) return yq_btree_cursor_key(c->bt_cur, out);
    return YQ_ERR_CURSOR;
}

int yq_cur_val(const yq_cur *c, yq_slice *out) {
    if (!c || !out) return YQ_ERR_INVAL;
    if (!yq_cur_valid(c)) return YQ_ERR_CURSOR;
    if (c->state == 1 && c->mt_iter) return yq_memtable_iter_val(c->mt_iter, out);
    if (c->state == 2 && c->bt_cur) return yq_btree_cursor_val(c->bt_cur, out);
    return YQ_ERR_CURSOR;
}

int yq_cur_close(yq_cur *c) {
    if (!c) return YQ_OK;
    if (c->mt_iter) yq_memtable_iter_close(c->mt_iter);
    if (c->bt_cur) yq_btree_cursor_close(c->bt_cur);
    free(c);
    return YQ_OK;
}

int yq_checkpoint(yq_db *db) {
    if (!db) return YQ_ERR_INVAL;
    if (!db->write_enabled) return YQ_ERR_CORRUPT;

    uint64_t wal_sz = yq_wal_size(db->wal);
    if (wal_sz == 0 && yq_memtable_size(db->memtable) == 0) return YQ_OK;

    uint64_t new_txn_id = 0;
    yq_mvcc_increment_txn_id(db->mvcc, &new_txn_id);

    uint64_t cur_txn = 0, cur_root = 0, cur_free = 0, cur_npages = 0, cur_ckpt = 0;
    yq_mvcc_meta_read(db->mvcc, &cur_txn, &cur_root, &cur_free, &cur_npages, &cur_ckpt);

    uint64_t root_page = cur_root;
    if (db->btree) {
        yq_btree_get_root(db->btree, &root_page);
        cur_npages = yq_btree_npages(db->btree);
    }

    yq_mvcc_meta_pwrite_full(db->mvcc, new_txn_id, root_page, cur_free, cur_npages, cur_ckpt);

    if (wal_sz > 0) yq_wal_truncate(db->wal, 0);
    if (db->db_file) yq_file_sync(db->db_file);

    return YQ_OK;
}

int yq_sync(yq_db *db) {
    if (!db) return YQ_ERR_INVAL;
    if (!db->write_enabled) return YQ_ERR_CORRUPT;
    yq_wal_flush(db->wal);
    if (db->db_file) yq_file_sync(db->db_file);
    return YQ_OK;
}

int yq_db_stat(yq_db *db, yq_stat *out) {
    if (!db || !out) return YQ_ERR_INVAL;
    if (out->struct_size != sizeof(yq_stat)) return YQ_ERR_INVAL;
    memset(out, 0, sizeof(yq_stat));
    out->struct_size = sizeof(yq_stat);
    out->format_version = 1;
    out->page_size = db->opts.page_size;
    out->max_readers = db->opts.max_readers;
    uint64_t txn_id = 0, root_page = 0, free_head = 0, npages = 0, ckpt_lsn = 0;
    yq_mvcc_meta_read(db->mvcc, &txn_id, &root_page, &free_head, &npages, &ckpt_lsn);
    out->txn_id = txn_id;
    out->npages = npages;
    out->free_pages = free_head;
    out->log_bytes = yq_wal_size(db->wal);
    return YQ_OK;
}
