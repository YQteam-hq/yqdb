/*
 * yq_test_seek.c - equivalence test for the ordered cursor seek primitives.
 *
 * Validates yq_btree_cursor_seek() / yq_btree_cursor_seek_le() against the
 * reference behavior obtained by linear traversal (cursor_first + cursor_next):
 * seek must land on the first key >= target, seek_le on the last key <= target,
 * and a miss must invalidate the cursor exactly like the reference.
 *
 * The B+Tree backend resolves pages through get_page_data(), which only knows
 * about mmap-backed storage or an arena. This test backs the tree with a plain
 * zeroed buffer standing in for an mmap region and hand-builds a single leaf
 * page inside it (header, slot array, cells and CRC), then points the tree at
 * that page via yq_btree_set_root().
 *
 * Scope note: insert()/split() cannot currently materialize a multi-leaf tree
 * through a storage-backed provider (insert() resets root_page to 0 on every
 * call when the root is unset, so page identity is never stable, and the split
 * path emits pages that the descent rule cannot route). Therefore internal-page
 * descent and right-sibling advance beyond one leaf are not exercised here; see
 * the PR description for that limitation. The leaf-level binary search and the
 * seek/seek_le endpoint semantics are covered.
 *
 * Usage: yq_test_seek [num_keys] [page_size]
 */
#include "yq_btree.h"
#include "yq_enc.h"
#include "yq_slice.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MAX_KEYS      4096
#define PAGE_HDR_SIZE 24
#define PAGE_CRC_SIZE 4
#define SLOT_SIZE     2

static void write_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void write_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void write_u64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static void key4(uint8_t *out, uint32_t v) {
    out[0] = (uint8_t)(v >> 24);
    out[1] = (uint8_t)(v >> 16);
    out[2] = (uint8_t)(v >> 8);
    out[3] = (uint8_t)v;
}

static uint32_t slice_to_u32(const yq_slice *s) {
    const uint8_t *p = (const uint8_t *)s->data;
    if (!p || s->size != 4) return 0xFFFFFFFFu;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/*
 * Build a leaf page holding `n` strictly ascending 4-byte keys at
 * map + page_no * page_size, with the same layout the engine expects:
 * 24-byte header, slot array growing up from the header, cells packed
 * downward from the CRC trailer, and a CRC32C over everything but the trailer.
 */
static void build_leaf(uint8_t *map, uint32_t ps, uint64_t page_no,
                       const uint32_t *keys, int n) {
    uint8_t *page = map + page_no * ps;
    memset(page, 0, ps);

    uint16_t off = (uint16_t)(ps - PAGE_CRC_SIZE);
    for (int i = 0; i < n; i++) {
        size_t kl = 0, vl = 0;
        uint8_t kb[10], vb[10];
        yq_varint_encode(4, kb, &kl);
        yq_varint_encode(1, vb, &vl);

        uint16_t csz = (uint16_t)(kl + 4 + vl + 1);
        off = (uint16_t)(off - csz);
        uint8_t *cell = page + off;
        size_t p = 0;
        memcpy(cell + p, kb, kl); p += kl;
        key4(cell + p, keys[i]); p += 4;
        memcpy(cell + p, vb, vl); p += vl;
        cell[p] = (uint8_t)'v';

        write_u16(page + PAGE_HDR_SIZE + i * SLOT_SIZE, off);
    }

    page[0] = 1;  /* YQ_PAGE_TYPE_LEAF */
    page[1] = 1;  /* flags */
    write_u16(page + 2, (uint16_t)n);
    write_u64(page + 4, 0);   /* right_sibling */
    write_u64(page + 12, 0);  /* left_sibling */
    write_u16(page + 20, (uint16_t)(off - (PAGE_HDR_SIZE + n * SLOT_SIZE)));
    write_u16(page + 22, PAGE_HDR_SIZE);
    write_u32(page + ps - PAGE_CRC_SIZE, yq_crc32c(page, ps - PAGE_CRC_SIZE));
}

/* Reference: first key >= probe, found via linear traversal. */
static int ref_ge(yq_btree *bt, uint32_t probe, uint32_t *out) {
    yq_btree_cursor *c = NULL;
    if (yq_btree_cursor_open(bt, &c) != YQ_OK) return -1;
    int found = 0;
    int rc = yq_btree_cursor_first(c);
    while (rc == YQ_OK && yq_btree_cursor_valid(c)) {
        yq_slice k;
        if (yq_btree_cursor_key(c, &k) != YQ_OK) break;
        uint32_t v = slice_to_u32(&k);
        if (v >= probe) { *out = v; found = 1; break; }
        rc = yq_btree_cursor_next(c);
    }
    yq_btree_cursor_close(c);
    return found;
}

/* Reference: last key <= probe, found via linear traversal. */
static int ref_le(yq_btree *bt, uint32_t probe, uint32_t *out) {
    yq_btree_cursor *c = NULL;
    if (yq_btree_cursor_open(bt, &c) != YQ_OK) return -1;
    int found = 0;
    int rc = yq_btree_cursor_first(c);
    while (rc == YQ_OK && yq_btree_cursor_valid(c)) {
        yq_slice k;
        if (yq_btree_cursor_key(c, &k) != YQ_OK) break;
        uint32_t v = slice_to_u32(&k);
        if (v <= probe) { *out = v; found = 1; }
        else break;
        rc = yq_btree_cursor_next(c);
    }
    yq_btree_cursor_close(c);
    return found;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int n_keys = (argc > 1) ? atoi(argv[1]) : 32;
    uint32_t page_size = (argc > 2) ? (uint32_t)atoi(argv[2]) : 4096u;
    if (n_keys < 1) n_keys = 1;
    if (n_keys > MAX_KEYS) n_keys = MAX_KEYS;

    const uint64_t pages = 64;
    uint64_t map_size = pages * page_size;
    uint8_t *map = (uint8_t *)calloc(1, (size_t)map_size);
    if (!map) { printf("alloc map failed\n"); return 2; }

    yq_btree *bt = NULL;
    if (yq_btree_open(&bt, map, map_size, page_size) != YQ_OK) {
        printf("btree_open failed\n");
        free(map);
        return 2;
    }

    uint32_t *keys = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)n_keys);
    for (int i = 0; i < n_keys; i++) keys[i] = (uint32_t)(i * 3);

    build_leaf(map, page_size, 1, keys, n_keys);
    yq_btree_set_root(bt, 1);

    /* Collect traversal order to confirm ordering and content. */
    uint32_t *seen = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)n_keys);
    int n_seen = 0;
    yq_btree_cursor *c = NULL;
    yq_btree_cursor_open(bt, &c);
    int rc = yq_btree_cursor_first(c);
    while (rc == YQ_OK && yq_btree_cursor_valid(c) && n_seen < n_keys) {
        yq_slice k;
        if (yq_btree_cursor_key(c, &k) != YQ_OK) break;
        seen[n_seen++] = slice_to_u32(&k);
        rc = yq_btree_cursor_next(c);
    }
    yq_btree_cursor_close(c);

    int sorted = 1, unique = 1;
    for (int i = 1; i < n_seen; i++) {
        if (seen[i] < seen[i - 1]) sorted = 0;
        if (seen[i] == seen[i - 1]) unique = 0;
    }
    printf("traversed=%d/%d sorted=%d unique=%d\n", n_seen, n_keys, sorted, unique);
    if (n_seen > 0) printf("min=%u max=%u\n", seen[0], seen[n_seen - 1]);

    /* Build probes: exact keys, gaps, and both ends (below min / above max). */
    static uint32_t probes[512];
    int n_probes = 0;
    probes[n_probes++] = 0;
    probes[n_probes++] = 1;
    probes[n_probes++] = 2;
    probes[n_probes++] = 3;
    uint32_t hi = (n_seen > 0) ? seen[n_seen - 1] : 0;
    probes[n_probes++] = hi;
    probes[n_probes++] = hi + 1;
    probes[n_probes++] = hi + 2;
    probes[n_probes++] = 100000;
    for (int i = 0; i < 200; i++) probes[n_probes++] = (uint32_t)(i * 41 + 7);
    for (int i = 0; i < 200; i++) probes[n_probes++] = (uint32_t)(i * 45);

    yq_btree_cursor *s = NULL;
    yq_btree_cursor_open(bt, &s);

    int fail_ge = 0, fail_le = 0;
    for (int i = 0; i < n_probes; i++) {
        uint32_t probe = probes[i];
        uint8_t pb[4];
        key4(pb, probe);
        yq_slice pk; pk.data = pb; pk.size = 4;

        int rc_new = yq_btree_cursor_seek(s, pk);
        uint32_t got = 0xFFFFFFFFu;
        int new_found = (rc_new == YQ_OK && yq_btree_cursor_valid(s));
        if (new_found) {
            yq_slice k;
            yq_btree_cursor_key(s, &k);
            got = slice_to_u32(&k);
        }
        uint32_t exp = 0xFFFFFFFFu;
        int exp_found = ref_ge(bt, probe, &exp);
        if (new_found != exp_found || (new_found && got != exp)) {
            if (fail_ge < 10)
                printf("  [seek mismatch] probe=%u new(rc=%d,found=%d,key=%u) ref(found=%d,key=%u)\n",
                       probe, rc_new, new_found, got, exp_found, exp);
            fail_ge++;
        }

        rc_new = yq_btree_cursor_seek_le(s, pk);
        got = 0xFFFFFFFFu;
        new_found = (rc_new == YQ_OK && yq_btree_cursor_valid(s));
        if (new_found) {
            yq_slice k;
            yq_btree_cursor_key(s, &k);
            got = slice_to_u32(&k);
        }
        exp = 0xFFFFFFFFu;
        exp_found = ref_le(bt, probe, &exp);
        if (new_found != exp_found || (new_found && got != exp)) {
            if (fail_le < 10)
                printf("  [seek_le mismatch] probe=%u new(rc=%d,found=%d,key=%u) ref(found=%d,key=%u)\n",
                       probe, rc_new, new_found, got, exp_found, exp);
            fail_le++;
        }
    }
    yq_btree_cursor_close(s);

    printf("probes=%d seek_mismatch=%d seek_le_mismatch=%d\n", n_probes, fail_ge, fail_le);
    int ok = (fail_ge == 0 && fail_le == 0 && sorted && unique && n_seen == n_keys);
    printf("%s\n", ok ? "RESULT: PASS" : "RESULT: FAIL");

    free(seen);
    free(keys);
    free(map);
    return ok ? 0 : 1;
}
