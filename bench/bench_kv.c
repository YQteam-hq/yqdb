#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
static double now_sec(void) {
    LARGE_INTEGER f, c;
    static LARGE_INTEGER freq;
    static int init = 0;
    if (!init) { QueryPerformanceFrequency(&freq); init = 1; }
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
}
#else
#include <time.h>
static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
#endif

#define KEYLEN 16
#define VALLEN 100
#define BATCH  1000

static uint64_t rng_state = 0x9E3779B97F4A7C15ULL;

static uint64_t rng_next(void) {
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return x;
}

static void make_key(uint64_t id, unsigned char k[KEYLEN]) {
    memset(k, 'K', KEYLEN);
    for (int i = 0; i < 8; i++) {
        k[i] = (unsigned char)(id >> (56 - 8 * i));
    }
}

static void make_val(uint64_t id, unsigned char v[VALLEN]) {
    for (int i = 0; i < VALLEN; i++) {
        v[i] = (unsigned char)(id * 7u + (uint64_t)i);
    }
}

static void shuffle(uint64_t *a, uint64_t n) {
    for (uint64_t i = n; i > 1; i--) {
        uint64_t j = rng_next() % i;
        uint64_t t = a[i - 1];
        a[i - 1] = a[j];
        a[j] = t;
    }
}

#if defined(ENGINE_YQ)

#include "yq.h"
#define ENGINE_NAME "yq-db"

static yq_db  *g_db  = NULL;
static yq_txn *g_wtx = NULL;
static yq_txn *g_rtx = NULL;
static int     g_mode = 0;

static int eng_open(const char *path, int create) {
    yq_opts o;
    memset(&o, 0, sizeof(o));
    o.struct_size    = sizeof(o);
    o.flags          = create ? YQ_OPEN_CREATE : 0;
    o.sync_mode      = g_mode ? YQ_SYNC_OFF : YQ_SYNC_NORMAL;
    o.map_size       = 4ULL * 1024 * 1024 * 1024;
    o.memtable_bytes = 512ULL * 1024 * 1024;
    o.log_bytes      = 1024ULL * 1024 * 1024;
    return yq_open(path, &o, &g_db);
}

static void eng_close(void) {
    if (g_db) { yq_close(g_db); g_db = NULL; }
}

static int eng_wbegin(void)  { return yq_txn_begin(g_db, YQ_TXN_READWRITE, &g_wtx); }
static int eng_wcommit(void) { int rc = yq_txn_commit(g_wtx); g_wtx = NULL; return rc; }
static int eng_rbegin(void)  { return yq_txn_begin(g_db, YQ_TXN_READONLY, &g_rtx); }
static int eng_rend(void)    { int rc = yq_txn_commit(g_rtx); g_rtx = NULL; return rc; }

static int eng_put(const unsigned char *k, const unsigned char *v) {
    yq_slice ks = { k, KEYLEN };
    yq_slice vs = { v, VALLEN };
    return yq_put(g_wtx, ks, vs, YQ_PUT_UPSERT);
}

static int eng_del(const unsigned char *k) {
    yq_slice ks = { k, KEYLEN };
    return yq_del(g_wtx, ks);
}

static int eng_get(const unsigned char *k, int *found) {
    yq_slice ks = { k, KEYLEN };
    yq_slice out = { 0 };
    int rc = yq_get(g_rtx, ks, &out);
    if (rc == YQ_OK) { *found = 1; return 0; }
    if (rc == YQ_ERR_NOTFOUND) { *found = 0; return 0; }
    return rc;
}

static long long eng_scan(void) {
    yq_cur *c = NULL;
    long long cnt = 0;
    if (yq_cur_open(g_rtx, &c) != YQ_OK) return -1;
    for (int rc = yq_cur_first(c); rc == YQ_OK; rc = yq_cur_next(c)) cnt++;
    yq_cur_close(c);
    return cnt;
}

static long long eng_size(void) { return -1; }

#elif defined(ENGINE_SQLITE)

#include "sqlite3.h"
#define ENGINE_NAME "sqlite"

static sqlite3      *g_db  = NULL;
static sqlite3_stmt *st_put = NULL, *st_get = NULL, *st_del = NULL, *st_scan = NULL;
static int           g_mode = 0;

static int eng_open(const char *path, int create) {
    (void)create;
    if (sqlite3_open(path, &g_db) != SQLITE_OK) return -1;
    sqlite3_exec(g_db, "PRAGMA journal_mode=WAL;", 0, 0, 0);
    sqlite3_exec(g_db, g_mode ? "PRAGMA synchronous=OFF;" : "PRAGMA synchronous=NORMAL;", 0, 0, 0);
    sqlite3_exec(g_db, "PRAGMA cache_size=-65536;", 0, 0, 0);
    sqlite3_exec(g_db, "CREATE TABLE IF NOT EXISTS kv(k BLOB PRIMARY KEY, v BLOB) WITHOUT ROWID;", 0, 0, 0);
    sqlite3_prepare_v2(g_db, "INSERT OR REPLACE INTO kv(k,v) VALUES(?,?);", -1, &st_put, 0);
    sqlite3_prepare_v2(g_db, "SELECT v FROM kv WHERE k=?;", -1, &st_get, 0);
    sqlite3_prepare_v2(g_db, "DELETE FROM kv WHERE k=?;", -1, &st_del, 0);
    sqlite3_prepare_v2(g_db, "SELECT k,v FROM kv;", -1, &st_scan, 0);
    return 0;
}

static void eng_close(void) {
    sqlite3_finalize(st_put); sqlite3_finalize(st_get);
    sqlite3_finalize(st_del); sqlite3_finalize(st_scan);
    if (g_db) { sqlite3_close(g_db); g_db = NULL; }
}

static int eng_wbegin(void)  { return sqlite3_exec(g_db, "BEGIN;", 0, 0, 0) == SQLITE_OK ? 0 : -1; }
static int eng_wcommit(void) { return sqlite3_exec(g_db, "COMMIT;", 0, 0, 0) == SQLITE_OK ? 0 : -1; }
static int eng_rbegin(void)  { return sqlite3_exec(g_db, "BEGIN;", 0, 0, 0) == SQLITE_OK ? 0 : -1; }
static int eng_rend(void)    { return sqlite3_exec(g_db, "COMMIT;", 0, 0, 0) == SQLITE_OK ? 0 : -1; }

static int eng_put(const unsigned char *k, const unsigned char *v) {
    sqlite3_bind_blob(st_put, 1, k, KEYLEN, SQLITE_STATIC);
    sqlite3_bind_blob(st_put, 2, v, VALLEN, SQLITE_STATIC);
    int rc = sqlite3_step(st_put);
    sqlite3_reset(st_put);
    return rc == SQLITE_DONE ? 0 : -1;
}

static int eng_del(const unsigned char *k) {
    sqlite3_bind_blob(st_del, 1, k, KEYLEN, SQLITE_STATIC);
    int rc = sqlite3_step(st_del);
    sqlite3_reset(st_del);
    return rc == SQLITE_DONE ? 0 : -1;
}

static int eng_get(const unsigned char *k, int *found) {
    sqlite3_bind_blob(st_get, 1, k, KEYLEN, SQLITE_STATIC);
    int rc = sqlite3_step(st_get);
    *found = (rc == SQLITE_ROW) ? 1 : 0;
    sqlite3_reset(st_get);
    return 0;
}

static long long eng_scan(void) {
    long long cnt = 0;
    sqlite3_reset(st_scan);
    while (sqlite3_step(st_scan) == SQLITE_ROW) cnt++;
    sqlite3_reset(st_scan);
    return cnt;
}

static long long eng_size(void) {
    sqlite3_stmt *s = NULL;
    long long pages = 0, psz = 0;
    if (sqlite3_prepare_v2(g_db, "PRAGMA page_count;", -1, &s, 0) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW)
        pages = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    s = NULL;
    if (sqlite3_prepare_v2(g_db, "PRAGMA page_size;", -1, &s, 0) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW)
        psz = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return pages * psz;
}

#elif defined(ENGINE_LMDB)

#include "lmdb.h"
#define ENGINE_NAME "lmdb"

static MDB_env *g_env = NULL;
static MDB_dbi  g_dbi;
static MDB_txn *g_txn = NULL;
static int      g_mode = 0;

static int eng_open(const char *path, int create) {
    (void)create;
    if (mdb_env_create(&g_env) != MDB_SUCCESS) return -1;
    mdb_env_set_mapsize(g_env, 256ULL * 1024 * 1024);
    mdb_env_set_maxdbs(g_env, 8);
    unsigned int flags = g_mode ? MDB_NOSYNC : 0;
    if (mdb_env_open(g_env, path, flags, 0664) != MDB_SUCCESS) return -1;
    MDB_txn *t = NULL;
    if (mdb_txn_begin(g_env, NULL, 0, &t) != MDB_SUCCESS) return -1;
    mdb_dbi_open(t, NULL, MDB_CREATE, &g_dbi);
    mdb_txn_commit(t);
    return 0;
}

static void eng_close(void) {
    if (g_env) { mdb_env_close(g_env); g_env = NULL; }
}

static int eng_wbegin(void)  { return mdb_txn_begin(g_env, NULL, 0, &g_txn) == MDB_SUCCESS ? 0 : -1; }
static int eng_wcommit(void) { int rc = mdb_txn_commit(g_txn); g_txn = NULL; return rc == MDB_SUCCESS ? 0 : -1; }
static int eng_rbegin(void)  { return mdb_txn_begin(g_env, NULL, MDB_RDONLY, &g_txn) == MDB_SUCCESS ? 0 : -1; }
static int eng_rend(void)    { mdb_txn_commit(g_txn); g_txn = NULL; return 0; }

static int eng_put(const unsigned char *k, const unsigned char *v) {
    MDB_val mk = { KEYLEN, (void *)k };
    MDB_val mv = { VALLEN, (void *)v };
    return mdb_put(g_txn, g_dbi, &mk, &mv, 0) == MDB_SUCCESS ? 0 : -1;
}

static int eng_del(const unsigned char *k) {
    MDB_val mk = { KEYLEN, (void *)k };
    return mdb_del(g_txn, g_dbi, &mk, NULL) == MDB_SUCCESS ? 0 : -1;
}

static int eng_get(const unsigned char *k, int *found) {
    MDB_val mk = { KEYLEN, (void *)k };
    MDB_val mv;
    int rc = mdb_get(g_txn, g_dbi, &mk, &mv);
    *found = (rc == MDB_SUCCESS) ? 1 : 0;
    return 0;
}

static long long eng_scan(void) {
    MDB_cursor *cur = NULL;
    MDB_val mk, mv;
    long long cnt = 0;
    if (mdb_cursor_open(g_txn, g_dbi, &cur) != MDB_SUCCESS) return -1;
    if (mdb_cursor_get(cur, &mk, &mv, MDB_FIRST) == MDB_SUCCESS) {
        do { cnt++; } while (mdb_cursor_get(cur, &mk, &mv, MDB_NEXT) == MDB_SUCCESS);
    }
    mdb_cursor_close(cur);
    return cnt;
}

static long long eng_size(void) {
    MDB_stat st;
    MDB_envinfo info;
    if (mdb_env_stat(g_env, &st) != MDB_SUCCESS) return -1;
    if (mdb_env_info(g_env, &info) != MDB_SUCCESS) return -1;
    return (long long)(info.me_last_pgno + 1) * (long long)st.ms_psize;
}

#else
#error "define ENGINE_YQ, ENGINE_SQLITE or ENGINE_LMDB"
#endif

static const char *g_mode_name = "normal";
static const char *g_order_name = "rand";

static void report(const char *phase, uint64_t n, double sec) {
    double ops = sec > 0 ? (double)n / sec : 0.0;
    printf("RESULT,%s,%s,%s,%s,%llu,%.6f,%.1f\n",
           ENGINE_NAME, g_mode_name, g_order_name, phase,
           (unsigned long long)n, sec, ops);
    fflush(stdout);
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "bench.dat";
    uint64_t N = argc > 2 ? strtoull(argv[2], 0, 10) : 100000;
    const char *mode = argc > 3 ? argv[3] : "normal";
    const char *order = argc > 4 ? argv[4] : "rand";
    g_mode = (strcmp(mode, "nosync") == 0) ? 1 : 0;
    g_mode_name = mode;
    g_order_name = order;

    uint64_t *ids  = (uint64_t *)malloc(N * sizeof(uint64_t));
    uint64_t *rnd  = (uint64_t *)malloc(N * sizeof(uint64_t));
    uint64_t *miss = (uint64_t *)malloc(N * sizeof(uint64_t));
    for (uint64_t i = 0; i < N; i++) { ids[i] = i; rnd[i] = i; miss[i] = N + i; }
    rng_state = 0x123456789ABCDEFULL;
    shuffle(rnd, N); shuffle(miss, N);
    if (strcmp(order, "seq") != 0) shuffle(ids, N);

    unsigned char k[KEYLEN], v[VALLEN];
    double t0, t1;

    printf("ENGINE,%s,%s,%s,%llu\n", ENGINE_NAME, mode, order, (unsigned long long)N);
    fflush(stdout);

    if (eng_open(path, 1) != 0) { printf("OPEN_FAIL,%s\n", ENGINE_NAME); return 2; }

    t0 = now_sec();
    for (uint64_t i = 0; i < N; i++) {
        if (i % BATCH == 0) eng_wbegin();
        make_key(ids[i], k);
        make_val(ids[i], v);
        eng_put(k, v);
        if (i % BATCH == BATCH - 1 || i == N - 1) eng_wcommit();
    }
    t1 = now_sec();
    report("insert", N, t1 - t0);

    long long esz = eng_size();
    if (esz >= 0) { printf("SIZE,%s,%s,%s,%lld\n", ENGINE_NAME, mode, order, esz); fflush(stdout); }

    eng_rbegin();
    t0 = now_sec();
    long long hits = 0;
    for (uint64_t i = 0; i < N; i++) { int f = 0; make_key(rnd[i], k); eng_get(k, &f); hits += f; }
    t1 = now_sec();
    eng_rend();
    report("read_hit", (uint64_t)hits, t1 - t0);

    eng_rbegin();
    t0 = now_sec();
    long long msc = 0;
    for (uint64_t i = 0; i < N; i++) { int f = 0; make_key(miss[i], k); eng_get(k, &f); msc += f; }
    t1 = now_sec();
    eng_rend();
    report("read_miss", N, t1 - t0);

    eng_rbegin();
    t0 = now_sec();
    long long sc = eng_scan();
    t1 = now_sec();
    eng_rend();
    report("scan", (uint64_t)sc, t1 - t0);

    t0 = now_sec();
    for (uint64_t i = 0; i < N; i++) {
        if (i % BATCH == 0) eng_wbegin();
        make_key(rnd[i], k);
        make_val(rnd[i] + 1, v);
        eng_put(k, v);
        if (i % BATCH == BATCH - 1 || i == N - 1) eng_wcommit();
    }
    t1 = now_sec();
    report("update", N, t1 - t0);

    t0 = now_sec();
    for (uint64_t i = 0; i < N; i++) {
        if (i % BATCH == 0) eng_wbegin();
        make_key(rnd[i], k);
        eng_del(k);
        if (i % BATCH == BATCH - 1 || i == N - 1) eng_wcommit();
    }
    t1 = now_sec();
    report("delete", N, t1 - t0);

    eng_close();
    t0 = now_sec();
    if (eng_open(path, 0) == 0) {
        eng_rbegin();
        int f = 0;
        make_key(0, k);
        eng_get(k, &f);
        eng_rend();
    }
    t1 = now_sec();
    report("reopen", 1, t1 - t0);
    eng_close();

    free(ids); free(rnd); free(miss);
    return 0;
}
