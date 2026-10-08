/*
 * yq_test_seek.c - coverage + equivalence test for the ordered cursor seek
 * primitives added by P2 (yq_btree_cursor_seek / yq_btree_cursor_seek_le).
 *
 * Two fixtures are built by hand:
 *
 *   1. single leaf - one leaf page used directly as the tree root.
 *      seek / seek_le are validated against a linear reference built from
 *      cursor_first + cursor_next, which is exact at leaf level.
 *
 *   2. two-level tree - an internal root over three leaves linked through
 *      right_sibling. This covers the two paths the leaf fixture cannot
 *      reach: internal-page descent (find_slot on an internal page, then the
 *      cell child) and the cross-leaf right_sibling advance in
 *      yq_btree_cursor_seek.
 *      The root separator is deliberately loose - above every key in the
 *      fixture - so the descent always picks the same cell child and a probe
 *      above that leaf's maximum has to advance along right_sibling. Probes
 *      across the whole range therefore exercise one advance, two advances
 *      and advance-to-exhaustion.
 *      The linear reference is not usable here: find_leftmost_leaf /
 *      find_rightmost_leaf are leaf-only in this baseline (they pick the
 *      wrong child pointer on internal pages) and P2 is add-only per its
 *      scope, so the expectations are computed from the key set directly.
 *
 * Not covered, and not coverable within P2's scope: the
 * "slot >= nkeys -> rightmost child at page + page_size - 8" branch. The
 * engine keeps that child pointer in the last 8 bytes of the page while the
 * 4-byte CRC trailer occupies the last 4, so only the low half of the pointer
 * survives a CRC write and the value read back is garbage. The tree fixture
 * prints what it reads back to make this visible, and routes every probe
 * through a cell child so the branch is never entered.
 *
 * get_page_data() only resolves mmap-backed pages or an arena, so the tree is
 * backed by a zeroed buffer standing in for an mmap region and the pages are
 * laid out by hand (header, slot array, cells and CRC).
 *
 * Usage: yq_test_seek [num_keys] [page_size]
 * Exit:  0 = pass, 1 = mismatch, 2 = setup failure, 3 = invalid parameters
 */
#include "yq_btree.h"
#include "yq_enc.h"
#include "yq_slice.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define PAGE_HDR_SIZE   24
#define PAGE_CRC_SIZE   4
#define SLOT_SIZE       2
#define MAX_PROBES      512

#define PAGE_TYPE_LEAF     1
#define PAGE_TYPE_INTERNAL 2

/* Keys are 4 bytes and values 1 byte, so a leaf cell is varint(4) + key +
 * varint(1) + value = 7 bytes and an internal cell is varint(4) + key +
 * 8-byte child = 13 bytes; each also costs one 2-byte slot. */
#define LEAF_KEY_COST     (1 + 4 + 1 + 1 + SLOT_SIZE)
#define INTERNAL_KEY_COST (1 + 4 + 8 + SLOT_SIZE)

static uint32_t leaf_capacity(uint32_t ps) {
    if (ps <= PAGE_HDR_SIZE + PAGE_CRC_SIZE) return 0;
    return (ps - PAGE_HDR_SIZE - PAGE_CRC_SIZE) / LEAF_KEY_COST;
}

static uint32_t internal_capacity(uint32_t ps) {
    if (ps <= PAGE_HDR_SIZE + 8) return 0;
    return (ps - PAGE_HDR_SIZE - 8) / INTERNAL_KEY_COST;
}

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
 * map + page_no * page_size: 24-byte header, slot array growing up from the
 * header, cells packed downward from the CRC trailer, and a CRC32C over
 * everything but the trailer.
 */
static void build_leaf(uint8_t *map, uint32_t ps, uint64_t page_no,
                       const uint32_t *keys, int n,
                       uint64_t right_sibling, uint64_t left_sibling) {
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

    page[0] = PAGE_TYPE_LEAF;
    page[1] = 1;
    write_u16(page + 2, (uint16_t)n);
    write_u64(page + 4, right_sibling);
    write_u64(page + 12, left_sibling);
    write_u16(page + 20, (uint16_t)(off - (PAGE_HDR_SIZE + n * SLOT_SIZE)));
    write_u16(page + 22, PAGE_HDR_SIZE);
    write_u32(page + ps - PAGE_CRC_SIZE, yq_crc32c(page, ps - PAGE_CRC_SIZE));
}

/*
 * Build an internal page with `n` separator cells plus the rightmost child,
 * using the same layout the descent rule in yq_btree_lookup /
 * yq_btree_cursor_seek reads: cell = varint(klen) + key + 8-byte child, and
 * the child for "greater than every separator" stored at page + ps - 8.
 * Cells are packed downward from ps - 8 so they never overlap that pointer.
 * The rightmost child must be written before the CRC, since the CRC covers it.
 */
static void build_internal(uint8_t *map, uint32_t ps, uint64_t page_no,
                           const uint32_t *seps, const uint64_t *children, int n,
                           uint64_t rightmost) {
    uint8_t *page = map + page_no * ps;
    memset(page, 0, ps);

    uint16_t off = (uint16_t)(ps - 8);
    for (int i = 0; i < n; i++) {
        size_t kl = 0;
        uint8_t kb[10];
        yq_varint_encode(4, kb, &kl);

        uint16_t csz = (uint16_t)(kl + 4 + 8);
        off = (uint16_t)(off - csz);
        uint8_t *cell = page + off;
        memcpy(cell, kb, kl);
        key4(cell + kl, seps[i]);
        write_u64(cell + kl + 4, children[i]);

        write_u16(page + PAGE_HDR_SIZE + i * SLOT_SIZE, off);
    }
    write_u64(page + ps - 8, rightmost);

    page[0] = PAGE_TYPE_INTERNAL;
    page[1] = 0;
    write_u16(page + 2, (uint16_t)n);
    write_u64(page + 4, 0);
    write_u64(page + 12, 0);
    write_u16(page + 20, (uint16_t)(off - (PAGE_HDR_SIZE + n * SLOT_SIZE)));
    write_u16(page + 22, PAGE_HDR_SIZE);
    write_u32(page + ps - PAGE_CRC_SIZE, yq_crc32c(page, ps - PAGE_CRC_SIZE));
}

/* Reference: first key >= probe, via linear traversal from the first leaf. */
static int ref_ge_linear(yq_btree *bt, uint32_t probe, uint32_t *out) {
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

/* Reference: last key <= probe, via linear traversal from the first leaf. */
static int ref_le_linear(yq_btree *bt, uint32_t probe, uint32_t *out) {
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

/* Contract: smallest key >= probe. */
static int exp_ge(const uint32_t *keys, int n, uint32_t probe, uint32_t *out) {
    int found = 0;
    uint32_t best = 0;
    for (int i = 0; i < n; i++) {
        if (keys[i] >= probe && (!found || keys[i] < best)) { best = keys[i]; found = 1; }
    }
    if (found) *out = best;
    return found;
}

/* Contract: largest key <= probe. */
static int exp_le(const uint32_t *keys, int n, uint32_t probe, uint32_t *out) {
    int found = 0;
    uint32_t best = 0;
    for (int i = 0; i < n; i++) {
        if (keys[i] <= probe && (!found || keys[i] > best)) { best = keys[i]; found = 1; }
    }
    if (found) *out = best;
    return found;
}

static int open_tree(uint32_t ps, uint8_t **out_map, uint64_t *out_size, yq_btree **out_bt) {
    const uint64_t pages = 64;
    uint64_t map_size = pages * ps;
    uint8_t *map = (uint8_t *)calloc(1, (size_t)map_size);
    if (!map) { printf("alloc map failed\n"); return 2; }
    yq_btree *bt = NULL;
    if (yq_btree_open(&bt, map, map_size, ps) != YQ_OK) {
        printf("btree_open failed\n");
        free(map);
        return 2;
    }
    *out_map = map; *out_size = map_size; *out_bt = bt;
    return 0;
}

/* Fixture 1: one leaf as the root; compare against the linear reference. */
static int case_single_leaf(int n_keys, uint32_t ps) {
    uint32_t cap = leaf_capacity(ps);
    if ((uint32_t)n_keys > cap) {
        printf("ERROR: num_keys=%d exceeds single-leaf capacity=%u for page_size=%u\n",
               n_keys, cap, ps);
        return 3;
    }
    if (cap == 0) { printf("ERROR: page_size=%u too small for a leaf page\n", ps); return 3; }

    uint8_t *map = NULL; uint64_t map_size = 0; yq_btree *bt = NULL;
    if (open_tree(ps, &map, &map_size, &bt) != 0) return 2;

    uint32_t *keys = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)n_keys);
    for (int i = 0; i < n_keys; i++) keys[i] = (uint32_t)(i * 3);

    build_leaf(map, ps, 1, keys, n_keys, 0, 0);
    yq_btree_set_root(bt, 1);

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
    printf("leaf:   n=%d page_size=%u traversed=%d/%d sorted=%d unique=%d\n",
           n_keys, ps, n_seen, n_keys, sorted, unique);
    if (n_seen > 0) printf("        min=%u max=%u cap=%u\n", seen[0], seen[n_seen - 1], cap);

    static uint32_t probes[MAX_PROBES];
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
        int exp_found = ref_ge_linear(bt, probe, &exp);
        if (new_found != exp_found || (new_found && got != exp)) {
            if (fail_ge < 10)
                printf("        [seek mismatch] probe=%u new(rc=%d,found=%d,key=%u) ref(found=%d,key=%u)\n",
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
        exp_found = ref_le_linear(bt, probe, &exp);
        if (new_found != exp_found || (new_found && got != exp)) {
            if (fail_le < 10)
                printf("        [seek_le mismatch] probe=%u new(rc=%d,found=%d,key=%u) ref(found=%d,key=%u)\n",
                       probe, rc_new, new_found, got, exp_found, exp);
            fail_le++;
        }
    }
    yq_btree_cursor_close(s);

    printf("leaf:   probes=%d seek_mismatch=%d seek_le_mismatch=%d\n", n_probes, fail_ge, fail_le);
    int bad = (fail_ge || fail_le || !sorted || !unique || n_seen != n_keys);
    free(seen); free(keys); free(map);
    return bad ? 1 : 0;
}

/*
 * Fixture 2: internal root over three leaves linked by right_sibling.
 *
 *   root(1)  internal, nkeys=1: sep 1000 -> child 2
 *   leaf(2)  {10,20,30}  right->3  left->0
 *   leaf(3)  {40,50,60}  right->4  left->2
 *   leaf(4)  {70,80,90}  right->0  left->3
 *
 * The separator (1000) sits above every key in the fixture, so find_slot on
 * the root always selects slot 0 and the descent always lands on leaf 2. That
 * is the loose-separator case the advance exists for: a probe above 30 falls
 * off leaf 2 and must walk right to leaf 3, above 60 it walks on to leaf 4,
 * and above 90 it exhausts the chain.
 */
static int case_multi_leaf(uint32_t ps) {
    static const uint32_t l1[] = {10, 20, 30};
    static const uint32_t l2[] = {40, 50, 60};
    static const uint32_t l3[] = {70, 80, 90};
    uint32_t all[9];
    for (int i = 0; i < 3; i++) { all[i] = l1[i]; all[3 + i] = l2[i]; all[6 + i] = l3[i]; }

    if (internal_capacity(ps) < 1 || leaf_capacity(ps) < 3) {
        printf("ERROR: page_size=%u too small for the tree fixture\n", ps);
        return 3;
    }

    uint8_t *map = NULL; uint64_t map_size = 0; yq_btree *bt = NULL;
    if (open_tree(ps, &map, &map_size, &bt) != 0) return 2;

    build_leaf(map, ps, 2, l1, 3, 3, 0);
    build_leaf(map, ps, 3, l2, 3, 4, 2);
    build_leaf(map, ps, 4, l3, 3, 0, 3);

    uint32_t seps[1] = {1000};
    uint64_t kids[1] = {2};
    build_internal(map, ps, 1, seps, kids, 1, 3);
    yq_btree_set_root(bt, 1);

    /* What the "greater than every separator" branch would read back. */
    uint64_t rm_read = 0;
    memcpy(&rm_read, map + 1 * ps + ps - 8, sizeof(rm_read));
    printf("tree:   rightmost-child slot reads back 0x%llX (wrote 3): low half is the "
           "pointer, high half is the CRC trailer\n", (unsigned long long)rm_read);

    yq_btree_cursor *s = NULL;
    yq_btree_cursor_open(bt, &s);

    int fail_ge = 0, fail_le = 0, le_skipped = 0;
    int routed_cell = 0, advance1 = 0, advance2 = 0;
    for (uint32_t probe = 0; probe <= 100; probe++) {
        uint8_t pb[4];
        key4(pb, probe);
        yq_slice pk; pk.data = pb; pk.size = 4;

        routed_cell++;               /* separator is above every probe */
        if (probe > 30) advance1++;  /* falls off leaf 2 */
        if (probe > 60) advance2++;  /* falls off leaf 2 and leaf 3 */

        int rc_new = yq_btree_cursor_seek(s, pk);
        uint32_t got = 0xFFFFFFFFu;
        int new_found = (rc_new == YQ_OK && yq_btree_cursor_valid(s));
        if (new_found) {
            yq_slice k;
            yq_btree_cursor_key(s, &k);
            got = slice_to_u32(&k);
        }
        uint32_t exp = 0xFFFFFFFFu;
        int exp_found = exp_ge(all, 9, probe, &exp);
        if (new_found != exp_found || (new_found && got != exp)) {
            if (fail_ge < 10)
                printf("        [tree seek mismatch] probe=%u new(rc=%d,found=%d,key=%u) exp(found=%d,key=%u)\n",
                       probe, rc_new, new_found, got, exp_found, exp);
            fail_ge++;
        }

        /*
         * seek_le only where its fallback is not involved. For a probe above
         * every key, seek_le delegates to cursor_last, whose descent
         * (find_rightmost_leaf) is leaf-only in this baseline and out of P2's
         * additive scope, so that endpoint cannot be asserted here.
         */
        if (probe > 90) { le_skipped++; continue; }

        rc_new = yq_btree_cursor_seek_le(s, pk);
        got = 0xFFFFFFFFu;
        new_found = (rc_new == YQ_OK && yq_btree_cursor_valid(s));
        if (new_found) {
            yq_slice k;
            yq_btree_cursor_key(s, &k);
            got = slice_to_u32(&k);
        }
        exp = 0xFFFFFFFFu;
        exp_found = exp_le(all, 9, probe, &exp);
        if (new_found != exp_found || (new_found && got != exp)) {
            if (fail_le < 10)
                printf("        [tree seek_le mismatch] probe=%u new(rc=%d,found=%d,key=%u) exp(found=%d,key=%u)\n",
                       probe, rc_new, new_found, got, exp_found, exp);
            fail_le++;
        }
    }
    yq_btree_cursor_close(s);

    printf("tree:   probes=101 descend_via_cell=%d advance_1_leaf=%d advance_2_leaves=%d "
           "seek_mismatch=%d seek_le_mismatch=%d (seek_le skipped=%d)\n",
           routed_cell, advance1, advance2, fail_ge, fail_le, le_skipped);

    /* Guard against the fixture silently losing its coverage. */
    if (routed_cell != 101 || advance1 == 0 || advance2 == 0) {
        printf("        ERROR: fixture no longer exercises the intended paths\n");
        fail_ge++;
    }

    free(map);
    return (fail_ge || fail_le) ? 1 : 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int n_keys = (argc > 1) ? atoi(argv[1]) : 32;
    uint32_t page_size = (argc > 2) ? (uint32_t)atoi(argv[2]) : 4096u;
    if (n_keys < 1) n_keys = 1;

    int r1 = case_single_leaf(n_keys, page_size);
    if (r1 == 3) return 3;
    int r2 = case_multi_leaf(page_size);
    if (r2 == 3) return 3;

    int ok = (r1 == 0 && r2 == 0);
    printf("%s\n", ok ? "RESULT: PASS" : "RESULT: FAIL");
    return ok ? 0 : 1;
}
