/*
 * P4 — recovery replay acceleration test.
 *
 * Covers the two behaviours that the P4 change rewrites:
 *   - yq_wal_scan: 64 KiB block reads, records spanning block boundaries,
 *     from_lsn filtering and the "stop at the first bad record" CRC rule;
 *   - yq_recover: only committed transactions are replayed, a trailing
 *     BEGIN+PUT without COMMIT stays invisible.
 *
 * It only uses the public/internal headers of the library (no helper modules).
 */

#include "yq.h"
#include "yq_wal.h"
#include "yq_recover.h"
#include "yq_memtable.h"
#include "yq_slice.h"
#include "yq_vfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DB_PATH  "yq_p4_scan_test"
#define LOG_PATH DB_PATH ".log"

static int g_failures = 0;

static void check(int cond, const char *what) {
    if (!cond) {
        printf("  FAIL: %s\n", what);
        g_failures++;
    }
}

static void cleanup_files(void) {
    remove(LOG_PATH);
    remove(DB_PATH);
    remove(DB_PATH ".shm");
    remove(DB_PATH ".lock");
}

static void sset(yq_slice *s, const char *str) {
    s->data = str;
    s->size = strlen(str);
}

/* ── scan contract: record count, lsn order and from_lsn filter ───────── */

typedef struct {
    uint64_t last_lsn;
    int count;
    int ordered;
} scan_probe;

static int probe_visit(void *ctx, uint64_t lsn, uint64_t txn_id, int rec_type,
                       const uint8_t *payload, size_t paylen) {
    scan_probe *p = (scan_probe *)ctx;
    (void)txn_id; (void)rec_type; (void)payload; (void)paylen;

    if (p->count > 0 && lsn <= p->last_lsn) p->ordered = 0;
    p->last_lsn = lsn;
    p->count++;
    return YQ_OK;
}

static void test_scan_from_lsn(void) {
    printf("test_scan_from_lsn\n");
    cleanup_files();

    yq_wal *wal = NULL;
    check(yq_wal_open(&wal, DB_PATH, 4096) == YQ_OK, "open wal");
    if (!wal) return;

    yq_slice k, v;
    sset(&k, "k1"); sset(&v, "v1");
    for (uint64_t t = 1; t <= 3; t++) {
        yq_wal_append_begin(wal, t);
        yq_wal_append_put(wal, t, k, v);
        yq_wal_append_commit(wal, t);
    }
    check(yq_wal_flush(wal) == YQ_OK, "flush wal");
    check(yq_wal_close(wal) == YQ_OK, "close wal");

    check(yq_wal_open(&wal, DB_PATH, 4096) == YQ_OK, "reopen wal");
    if (!wal) return;

    scan_probe all = { 0, 0, 1 };
    check(yq_wal_scan(wal, 0, probe_visit, &all) == YQ_OK, "scan from 0");
    check(all.count == 9, "from 0 sees all 9 records");
    check(all.ordered == 1, "lsns are strictly increasing");

    scan_probe part = { 0, 0, 1 };
    check(yq_wal_scan(wal, 7, probe_visit, &part) == YQ_OK, "scan from 7");
    check(part.count == 3, "from 7 sees the last three records");

    yq_wal_close(wal);
    cleanup_files();
}

/* ── replay semantics: committed only, tail BEGIN+PUT invisible ───────── */

static void test_replay_committed_only(void) {
    printf("test_replay_committed_only\n");
    cleanup_files();

    yq_wal *wal = NULL;
    yq_wal_open(&wal, DB_PATH, 4096);
    if (!wal) { check(0, "open wal"); return; }

    yq_slice k, v;
    sset(&k, "alpha"); sset(&v, "1");
    yq_wal_append_begin(wal, 1);
    yq_wal_append_put(wal, 1, k, v);
    yq_wal_append_commit(wal, 1);

    sset(&k, "beta"); sset(&v, "2");
    yq_wal_append_begin(wal, 2);
    yq_wal_append_put(wal, 2, k, v);
    yq_wal_append_commit(wal, 2);

    sset(&k, "alpha");
    yq_wal_append_begin(wal, 3);
    yq_wal_append_del(wal, 3, k);
    yq_wal_append_commit(wal, 3);

    /* trailing transaction without COMMIT must not be applied */
    sset(&k, "gamma"); sset(&v, "3");
    yq_wal_append_begin(wal, 4);
    yq_wal_append_put(wal, 4, k, v);

    yq_wal_flush(wal);
    yq_wal_close(wal);

    yq_wal_open(&wal, DB_PATH, 4096);
    if (!wal) { check(0, "reopen wal"); return; }
    yq_memtable *mt = yq_memtable_create(1u << 20);
    check(mt != NULL, "create memtable");
    check(yq_recover(wal, mt) == YQ_OK, "recover");
    yq_wal_close(wal);

    yq_slice out;
    sset(&k, "alpha");
    check(yq_memtable_get(mt, k, &out) == YQ_ERR_NOTFOUND, "alpha removed by committed txn 3");

    sset(&k, "beta");
    check(yq_memtable_get(mt, k, &out) == YQ_OK && out.size == 1 &&
          memcmp(out.data, "2", 1) == 0, "beta replayed with value 2");

    sset(&k, "gamma");
    check(yq_memtable_get(mt, k, &out) == YQ_ERR_NOTFOUND, "uncommitted gamma is invisible");

    yq_memtable_destroy(mt);
    cleanup_files();
}

/* ── records spanning many 64 KiB read blocks ─────────────────────────── */

static void test_cross_block_records(void) {
    printf("test_cross_block_records\n");
    cleanup_files();

    enum { N = 4000 };
    char kbuf[32];
    char vbuf[200];
    memset(vbuf, 'x', sizeof(vbuf) - 1);
    vbuf[sizeof(vbuf) - 1] = '\0';

    yq_wal *wal = NULL;
    yq_wal_open(&wal, DB_PATH, 4096);
    if (!wal) { check(0, "open wal"); return; }

    for (int i = 0; i < N; i++) {
        snprintf(kbuf, sizeof(kbuf), "key%06d", i);
        yq_slice k, v;
        sset(&k, kbuf);
        sset(&v, vbuf);
        yq_wal_append_begin(wal, (uint64_t)i + 1);
        yq_wal_append_put(wal, (uint64_t)i + 1, k, v);
        yq_wal_append_commit(wal, (uint64_t)i + 1);
    }
    yq_wal_flush(wal);
    uint64_t total = yq_wal_size(wal);
    yq_wal_close(wal);
    check(total > 3u * 64u * 1024u, "wal spans more than three 64 KiB blocks");

    yq_wal_open(&wal, DB_PATH, 4096);
    if (!wal) { check(0, "reopen wal"); return; }
    yq_memtable *mt = yq_memtable_create(1u << 24);
    check(mt != NULL, "create memtable");
    check(yq_recover(wal, mt) == YQ_OK, "recover large wal");
    yq_wal_close(wal);

    int missing = 0;
    for (int i = 0; i < N; i++) {
        yq_slice k, out;
        snprintf(kbuf, sizeof(kbuf), "key%06d", i);
        sset(&k, kbuf);
        if (yq_memtable_get(mt, k, &out) != YQ_OK) missing++;
    }
    check(missing == 0, "every committed key replayed across block boundaries");

    yq_memtable_destroy(mt);
    cleanup_files();
}

/* ── CRC truncation: a corrupt tail record stops the scan ─────────────── */

static void test_crc_truncation(void) {
    printf("test_crc_truncation\n");
    cleanup_files();

    yq_wal *wal = NULL;
    yq_wal_open(&wal, DB_PATH, 4096);
    if (!wal) { check(0, "open wal"); return; }

    yq_slice k, v;
    sset(&k, "good"); sset(&v, "ok");
    yq_wal_append_begin(wal, 1);
    yq_wal_append_put(wal, 1, k, v);
    yq_wal_append_commit(wal, 1);
    yq_wal_flush(wal);
    yq_wal_close(wal);

    /* append a full record header whose CRC does not match */
    yq_file *f = yq_file_open(LOG_PATH, 1, 1);
    check(f != NULL, "open log for corruption");
    if (f) {
        uint64_t size = yq_file_size(f);
        uint8_t junk[29];
        memset(junk, 0xAB, sizeof(junk));
        check(yq_file_pwrite(f, junk, sizeof(junk), size) == YQ_OK, "write corrupt record");
        yq_file_close(f);
    }

    yq_wal_open(&wal, DB_PATH, 4096);
    if (!wal) { check(0, "reopen wal"); return; }
    yq_memtable *mt = yq_memtable_create(1u << 20);
    check(mt != NULL, "create memtable");
    check(yq_recover(wal, mt) == YQ_OK, "corrupt tail returns YQ_OK");
    yq_wal_close(wal);

    yq_slice out;
    sset(&k, "good");
    check(yq_memtable_get(mt, k, &out) == YQ_OK, "record before corrupt tail is applied");

    yq_memtable_destroy(mt);
    cleanup_files();
}

int main(void) {
    printf("running P4 tests\n");

    test_scan_from_lsn();
    test_replay_committed_only();
    test_cross_block_records();
    test_crc_truncation();

    if (g_failures == 0) {
        printf("P4 tests passed\n");
        return 0;
    }
    printf("P4 tests failed: %d check(s)\n", g_failures);
    return 1;
}
