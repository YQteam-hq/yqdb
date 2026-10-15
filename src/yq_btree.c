#include "yq_btree.h"
#include "yq_enc.h"
#include "yq_slice.h"
#include "yq_page.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define YQ_PAGE_TYPE_LEAF     1
#define YQ_PAGE_TYPE_INTERNAL 2
#define YQ_PAGE_TYPE_OVERFLOW 3
#define YQ_PAGE_TYPE_FREE     4

#define YQ_PAGE_HEADER_SIZE   24
#define YQ_PAGE_CRC_SIZE      4
#define YQ_PAGE_SLOT_SIZE     2
#define YQ_VARINT_MAX_SIZE    10

#define YQ_INLINE_MAX(ps)     ((ps) / 4)

/*
 * Page bookkeeping, corrected (the old arithmetic drifted).
 *
 * Layout: header [0,24), slot array [24, 24+2*nkeys), free gap, then the
 * cells packed upward, ending at page_size - 4 where the CRC lives. So:
 *
 *     lowest_cell_offset = 24 + 2*nkeys + free_bytes
 *     free_bytes         = size of the gap between slots and the lowest cell
 *
 * Inserting a `needed`-byte cell: it goes at lowest_cell_offset - needed and
 * costs `needed + 2` (the cell plus its slot). The previous formula placed
 * cells at free_bytes - needed, i.e. below the existing cell block, and lost
 * (24 + 2*nkeys) bytes per insert -- so pages filled ~10x too early.
 */
#define YQ_SLOTS_END(nkeys) ((uint16_t)(YQ_PAGE_HEADER_SIZE + (nkeys) * YQ_PAGE_SLOT_SIZE))
#define YQ_CELL_TOP(ps)     ((uint16_t)((ps) - YQ_PAGE_CRC_SIZE))

typedef struct {
    uint8_t page_type;
    uint8_t flags;
    uint16_t nkeys;
    uint64_t right_sibling;
    uint64_t left_sibling;
    uint16_t free_bytes;
    uint16_t header_size;
} yq_page_header;

struct yq_btree {
    uint32_t page_size;
    uint64_t root_page;
    int has_root;                /* root_page == 0 means "no tree yet" */
    uint64_t npages;             /* legacy (store-less) page count */
    yq_page_store *store;        /* P3: where page numbers come from */
    yq_memblk *arena;
    void *mmap_base;
    uint64_t mmap_size;
    void *page_provider_ctx;
    void *(*page_alloc)(void *ctx, int is_leaf);
    void (*page_free)(void *ctx, uint64_t page_no);
    void *file_ctx;
    void *(*file_alloc)(void *ctx, int is_leaf, uint64_t *out_page_no);
    void (*file_sync)(void *ctx);
};

struct yq_btree_cursor {
    yq_btree *bt;
    uint64_t leaf_page;
    uint16_t slot_idx;
    uint16_t slot_count;
    yq_slice cur_key;
    yq_slice cur_val;
    int valid;
};

static void read_page_header(const uint8_t *page, yq_page_header *hdr) {
    hdr->page_type   = page[0];
    hdr->flags       = page[1];
    hdr->nkeys       = *(uint16_t *)(page + 2);
    hdr->right_sibling = *(uint64_t *)(page + 4);
    hdr->left_sibling  = *(uint64_t *)(page + 12);
    hdr->free_bytes    = *(uint16_t *)(page + 20);
    hdr->header_size   = *(uint16_t *)(page + 22);
}

static void write_page_header(uint8_t *page, const yq_page_header *hdr) {
    page[0] = hdr->page_type;
    page[1] = hdr->flags;
    *(uint16_t *)(page + 2) = hdr->nkeys;
    *(uint64_t *)(page + 4) = hdr->right_sibling;
    *(uint64_t *)(page + 12) = hdr->left_sibling;
    *(uint16_t *)(page + 20) = hdr->free_bytes;
    *(uint16_t *)(page + 22) = hdr->header_size;
}

static void write_page_crc(uint8_t *page, uint32_t page_size) {
    uint32_t crc = yq_crc32c(page, page_size - YQ_PAGE_CRC_SIZE);
    *(uint32_t *)(page + page_size - YQ_PAGE_CRC_SIZE) = crc;
}

static int check_page_crc(const uint8_t *page, uint32_t page_size) {
    uint32_t stored = *(uint32_t *)(page + page_size - YQ_PAGE_CRC_SIZE);
    uint32_t computed = yq_crc32c(page, page_size - YQ_PAGE_CRC_SIZE);
    return stored == computed;
}

static uint16_t get_slot(const uint8_t *page, uint16_t idx) {
    return *(uint16_t *)(page + YQ_PAGE_HEADER_SIZE + idx * YQ_PAGE_SLOT_SIZE);
}

static void set_slot(uint8_t *page, uint16_t idx, uint16_t offset) {
    *(uint16_t *)(page + YQ_PAGE_HEADER_SIZE + idx * YQ_PAGE_SLOT_SIZE) = offset;
}

static uint8_t *get_page_data(yq_btree *bt, uint64_t page_no) {
    if (bt->store) {
        return (uint8_t *)yq_page_store_get(bt->store, page_no);
    }
    if (bt->mmap_base) {
        return (uint8_t *)bt->mmap_base + page_no * bt->page_size;
    }
    return (uint8_t *)bt->arena ?
           yq_memblk_alloc(bt->arena, bt->page_size) : NULL;
}

/*
 * Byte length of the cell in `slot_idx`, measured from the cell's own start.
 *
 * The previous version subtracted `offset` on the way out even though `pos`
 * was already counted from the cell start, so any cell sitting more than a few
 * hundred bytes into the page reported a negative length -- which the split
 * paths turn into YQ_ERR_CORRUPT on the very first leaf split. Latent until
 * now because nothing ever built a tree: the tests only hand-assembled pages
 * (yq_test_seek.c), and the old insert path split by copying fixed-size
 * regions rather than by cell size.
 */
static int cell_size(const uint8_t *page, uint16_t slot_idx, uint32_t page_size) {
    uint16_t offset = get_slot(page, slot_idx);
    uint8_t *cell = (uint8_t *)page + offset;
    size_t n = 0;
    uint64_t key_len;
    if (yq_varint_decode(cell, page_size - offset, &key_len, &n) != 0) return -1;
    size_t pos = n + key_len;
    yq_page_header hdr;
    read_page_header(page, &hdr);
    if (hdr.page_type == YQ_PAGE_TYPE_LEAF) {
        uint64_t val_len;
        if (yq_varint_decode(cell + pos, page_size - offset - pos, &val_len, &n) != 0) return -1;
        pos += n;
        if (val_len <= YQ_INLINE_MAX(page_size)) {
            return (int)(pos + val_len);
        } else {
            pos += 8;
            uint64_t total_len;
            if (yq_varint_decode(cell + pos, page_size - offset - pos, &total_len, &n) != 0) return -1;
            return (int)(pos + n);
        }
    } else {
        return (int)(pos + 8);
    }
}

static int compare_key(const uint8_t *key1, size_t len1, const uint8_t *key2, size_t len2) {
    return yq_slice_compare_raw(key1, len1, key2, len2);
}

/* Encode an internal cell: varint(key_len) key child_page. */
static void write_internal_cell(uint8_t *cell,
                                const uint8_t *key_len_buf, size_t key_len_enc,
                                const void *key_data, size_t key_size,
                                uint64_t child) {
    size_t pos = 0;
    memcpy(cell + pos, key_len_buf, key_len_enc);
    pos += key_len_enc;
    memcpy(cell + pos, key_data, key_size);
    pos += key_size;
    *(uint64_t *)(cell + pos) = child;
}

static int find_slot(const uint8_t *page, const uint8_t *key, size_t key_len, uint16_t nkeys, uint32_t page_size) {
    int lo = 0, hi = (int)nkeys - 1;
    if (nkeys == 0) return 0;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint16_t offset = get_slot(page, mid);
        uint8_t *cell = (uint8_t *)page + offset;
        size_t pos = 0;
        uint64_t klen;
        if (yq_varint_decode(cell, page_size - offset, &klen, &pos) != 0) return -1;
        if (pos + klen > (size_t)(page_size - offset)) return -1;
        int cmp = compare_key(cell + pos, klen, key, key_len);
        if (cmp == 0) return mid;
        if (cmp < 0) lo = mid + 1;
        else hi = mid - 1;
    }
    return lo;
}

/*
 * Resolve the child page to descend into from an internal page when a search has
 * landed at `slot` (0..nkeys). When slot == nkeys the search ran past every key
 * and the rightmost child pointer -- stored in the page tail -- is followed;
 * otherwise the child is the 8-byte field trailing the key in cell[slot].
 *
 * This block was duplicated verbatim in every internal-page descent (insert,
 * lookup, delete and cursor seek). The copies must stay byte-for-byte identical
 * or the traversals diverge, so they are consolidated here. Returns YQ_OK, or
 * YQ_ERR_CORRUPT on a malformed/overflowing cell.
 */
/*
 * Rightmost child of an internal page.
 *
 * FORMAT.md 4.2 puts it immediately after the slot array, NOT in the page
 * tail: the tail belongs to the cell block and to page_crc32c at
 * [page_size-4, page_size), so the old tail placement overlapped the CRC by
 * four bytes -- the stored child number came back with half its bytes
 * overwritten by the checksum, and the descent then followed a garbage page
 * number. The field therefore moves one slot to the right on every insert,
 * which is why page_insert_cell() relocates it.
 */
static int read_rightmost_child(const uint8_t *page, uint32_t ps,
                                int nkeys, uint64_t *out) {
    uint16_t off = YQ_SLOTS_END(nkeys);
    if ((uint32_t)off + 8 > ps) return YQ_ERR_CORRUPT;
    memcpy(out, page + off, 8);
    return YQ_OK;
}

/*
 * Resolve the child page to descend into from an internal page when a search
 * has landed at `slot` (0..nkeys). When slot >= nkeys the search ran past
 * every separator and the rightmost child is followed; otherwise the child is
 * the 8-byte field trailing the key in cell[slot].
 *
 * This block was duplicated verbatim in every internal-page descent (insert,
 * lookup, delete, cursor seek); they all come through here now, which is what
 * makes "one placement rule" testable at all.
 */
static int read_child_ptr(const uint8_t *page, uint32_t ps,
                          int slot, int nkeys, uint64_t *child) {
    if (slot >= nkeys) return read_rightmost_child(page, ps, nkeys, child);

    uint16_t offset = get_slot(page, (uint16_t)slot);
    const uint8_t *cell = page + offset;
    size_t n = 0;
    uint64_t klen;
    size_t avail = ps - offset;
    if (yq_varint_decode(cell, avail, &klen, &n) != YQ_OK || n + klen + 8 > avail) {
        return YQ_ERR_CORRUPT;
    }
    memcpy(child, cell + n + klen, 8);
    return YQ_OK;
}

/*
 * Index of the first cell whose separator is strictly greater than `key`.
 *
 * find_slot() (lower bound) is what a LEAF wants: the slot the key itself
 * belongs in. An internal page needs the opposite bias, because cell[i] owns
 * the keys strictly below its separator (FORMAT.md 4.2) while a key EQUAL to
 * a separator lives in the next subtree -- the leaves keep a copy of every
 * separator (copy-up), so descending left on an exact hit loses the key. That
 * is exactly what the old descent did: it routed key00000023 (the separator
 * promoted when the first root was created) into the left leaf, where it does
 * not exist, and the lookup reported NOTFOUND for a key that was in the tree.
 */
static int find_slot_upper(const uint8_t *page, const uint8_t *key, size_t key_len,
                           uint16_t nkeys, uint32_t page_size) {
    int lo = 0, hi = (int)nkeys - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint16_t offset = get_slot(page, (uint16_t)mid);
        uint8_t *cell = (uint8_t *)page + offset;
        size_t pos = 0;
        uint64_t klen;
        if (yq_varint_decode(cell, page_size - offset, &klen, &pos) != YQ_OK) return -1;
        if (pos + klen > (size_t)(page_size - offset)) return -1;
        if (compare_key(cell + pos, klen, key, key_len) > 0) hi = mid - 1;
        else lo = mid + 1;
    }
    return lo;
}

/*
 * The child page an internal page routes `key` to.
 *
 * Insert, lookup, delete and cursor seek each used to spell this out on their
 * own, and they did not agree: two of them read the rightmost child out of the
 * page tail (FORMAT.md 4.2 puts it after the slot array) and all of them sent
 * an exact separator hit left. One implementation, one rule.
 */
static int descend_child(const uint8_t *page, uint32_t ps, uint16_t nkeys,
                         const uint8_t *key, size_t key_len, uint64_t *child) {
    int slot = find_slot_upper(page, key, key_len, nkeys, ps);
    if (slot < 0) return YQ_ERR_CORRUPT;
    return read_child_ptr(page, ps, slot, (int)nkeys, child);
}

static int read_key_from_slot(const uint8_t *page, uint16_t slot_idx, uint8_t *key_buf, size_t *key_len, uint32_t page_size) {
    uint16_t offset = get_slot(page, slot_idx);
    uint8_t *cell = (uint8_t *)page + offset;
    size_t n = 0;
    if (yq_varint_decode(cell, page_size - offset, key_len, &n) != 0) return -1;
    if (*key_len > 1024) return -1;
    if (n + *key_len > (size_t)(page_size - offset)) return -1;
    memcpy(key_buf, cell + n, *key_len);
    return 0;
}

/*
 * Allocate a page and hand back its number (P3). With a page store bound
 * this is the only way a page is created -- the store owns the numbering
 * and the free list. Without one the tree cannot grow at all: the legacy
 * arena path handed out buffers with no page number behind them.
 */
static int btree_alloc_page(yq_btree *bt, uint64_t *out_page_no) {
    if (!bt->store) return YQ_ERR_NOTSUP;
    int rc = yq_page_store_alloc(bt->store, out_page_no);
    if (rc != YQ_OK) return rc;
    if (!yq_page_store_get(bt->store, *out_page_no)) return YQ_ERR_PANIC;
    return YQ_OK;
}
/*
 * Rewrite `page` with its first `count` cells packed contiguously from the top
 * of the cell area, and recompute free_bytes to match.
 *
 * A split leaves the surviving half with a HOLE, not with free space: cells
 * were allocated over time, so the ones that remain straddle the gap where the
 * moved cells used to sit. free_bytes can only describe the contiguous run
 * between the slot array and the lowest cell, so measuring it after a split
 * reports a few dozen bytes on a page that is really half empty. That is not a
 * cosmetic problem: the page splits again almost immediately, the space is
 * burned page after page (1200 keys wanted 614 pages instead of ~50), and
 * eventually a page that reached nkeys == 0 tries to split, where
 * `nkeys - split_pos - 1` underflows into a 65535-cell move and the build turns
 * into garbage.
 *
 * Call this on the half that kept cells. An internal page's rightmost child
 * moves with the slot array, so it is read from its old position and written
 * back to the new one.
 */
static int page_repack(yq_btree *bt, uint8_t *page, uint16_t count) {
    yq_page_header hdr;
    read_page_header(page, &hdr);
    uint32_t ps = bt->page_size;
    int internal = (hdr.page_type == YQ_PAGE_TYPE_INTERNAL);

    uint64_t rightmost = 0;
    if (internal) {
        int rc = read_rightmost_child(page, ps, (int)hdr.nkeys, &rightmost);
        if (rc != YQ_OK) return rc;
    }

    size_t total = 0;
    uint16_t *sizes = (uint16_t *)malloc((count ? count : 1) * sizeof(uint16_t));
    if (!sizes) return YQ_ERR_NOMEM;
    for (uint16_t i = 0; i < count; i++) {
        int csz = cell_size(page, i, ps);
        if (csz < 0) { free(sizes); return YQ_ERR_CORRUPT; }
        sizes[i] = (uint16_t)csz;
        total += (size_t)csz;
    }

    uint8_t *tmp = (uint8_t *)malloc(total ? total : 1);
    if (!tmp) { free(sizes); return YQ_ERR_NOMEM; }
    size_t at = 0;
    for (uint16_t i = 0; i < count; i++) {
        memcpy(tmp + at, page + get_slot(page, i), sizes[i]);
        at += sizes[i];
    }

    uint16_t cursor = YQ_CELL_TOP(ps);
    at = total;
    for (uint16_t i = count; i-- > 0; ) {
        at -= sizes[i];
        cursor = (uint16_t)(cursor - sizes[i]);
        memcpy(page + cursor, tmp + at, sizes[i]);
        set_slot(page, i, cursor);
    }
    free(tmp);
    free(sizes);

    hdr.nkeys = count;
    hdr.free_bytes = (uint16_t)(cursor - (YQ_SLOTS_END(count) + (internal ? 8 : 0)));
    if (internal) memcpy(page + YQ_SLOTS_END(count), &rightmost, 8);
    write_page_header(page, &hdr);
    write_page_crc(page, ps);
    return YQ_OK;
}

/* Result of an insert that did not fit the page it landed on. */
typedef struct {
    int      split;          /* 1 when the page split */
    uint64_t right_page;     /* the new right sibling */
    uint8_t  median[1024];   /* separator to promote into the parent */
    size_t   median_len;
} yq_split_info;

static void split_info_init(yq_split_info *out) {
    out->split = 0;
    out->right_page = 0;
    out->median_len = 0;
}

/*
 * Encode a leaf cell: varint(key_len) key varint(val_len) value. Values are
 * capped by the caller, so the biggest cell is inline_max + 1024 + slack,
 * which needs a heap buffer on the larger page sizes -- same fallback shape
 * as yq_wal_append_put().
 */
static uint8_t *encode_leaf_cell(yq_slice key, yq_slice val,
                                 size_t *out_len, uint8_t *stack, size_t stack_cap) {
    size_t klen_enc = 0, vlen_enc = 0;
    uint8_t kbuf[10], vbuf[10];
    yq_varint_encode(key.size, kbuf, &klen_enc);
    yq_varint_encode(val.size, vbuf, &vlen_enc);

    size_t total = klen_enc + key.size + vlen_enc + val.size;
    uint8_t *cell = (total <= stack_cap) ? stack : (uint8_t *)malloc(total);
    if (!cell) return NULL;

    memcpy(cell, kbuf, klen_enc);
    memcpy(cell + klen_enc, key.data, key.size);
    memcpy(cell + klen_enc + key.size, vbuf, vlen_enc);
    memcpy(cell + klen_enc + key.size + vlen_enc, val.data, val.size);
    *out_len = total;
    return cell;
}

/*
 * Place `cell_len` bytes at slot position `slot`, packed just below the
 * current lowest cell. The caller must have checked that the gap is big
 * enough (free_bytes >= cell_len + 2). Identical mechanics for leaf and
 * internal pages -- only the cell encoding differs.
 */
/*
 * Place `cell_len` bytes at slot position `slot`, packed just below the
 * current lowest cell. The caller must have verified that the gap is big
 * enough (free_bytes >= cell_len + 2).
 *
 * Internal pages keep their rightmost child immediately after the slot array
 * (FORMAT.md 4.2), so an insert shifts that field one slot to the right:
 * relocate it here and let its 8 bytes count against the gap. The mechanics
 * of leaf and internal cells are otherwise identical -- only the encoding
 * differs, and both come from the same two writers.
 */
static void page_insert_cell(yq_btree *bt, uint8_t *page, uint16_t nkeys,
                             int slot, const uint8_t *cell, uint16_t cell_len) {
    yq_page_header hdr;
    read_page_header(page, &hdr);

    int internal = (hdr.page_type == YQ_PAGE_TYPE_INTERNAL);
    uint64_t rightmost = 0;
    if (internal) memcpy(&rightmost, page + YQ_SLOTS_END(nkeys), 8);

    uint16_t off = (uint16_t)(YQ_SLOTS_END(nkeys) + (internal ? 8 : 0) +
                              hdr.free_bytes - cell_len);
    for (uint16_t i = nkeys; i > (uint16_t)slot; i--) {
        set_slot(page, i, get_slot(page, i - 1));
    }
    set_slot(page, (uint16_t)slot, off);
    memcpy(page + off, cell, cell_len);

    if (internal) memcpy(page + YQ_SLOTS_END(nkeys + 1), &rightmost, 8);

    hdr.nkeys = (uint16_t)(nkeys + 1);
    hdr.free_bytes = (uint16_t)(hdr.free_bytes - cell_len - YQ_PAGE_SLOT_SIZE);
    write_page_header(page, &hdr);
    write_page_crc(page, bt->page_size);
}

/*
 * Insert into a leaf, splitting when the gap is too small.
 *
 * On a split the page keeps the lower half, a new page takes the upper half,
 * and the first key of the upper half is promoted as the separator -- the
 * standard B+Tree arrangement. The separator is COPIED UP: the right half
 * keeps it as its first key and the parent gets a copy, so every key the
 * tree holds is reachable from a leaf-chain scan. Sibling links keep that
 */
static int leaf_insert(yq_btree *bt, uint64_t page_no, uint8_t *page,
                       yq_slice key, yq_slice val, yq_split_info *out) {
    /* page_no is the sibling bookkeeping this function needs (the new right
     * half's left link), and what stage S3 will need to copy the page
     * before editing it. */
    split_info_init(out);


    uint32_t ps = bt->page_size;
    if (val.size > YQ_INLINE_MAX(ps)) {
        /*
         * Overflow cells need a page chain that nothing allocates yet (P3
         * stage 4). Refuse loudly: the previous code wrote a cell whose
         * overflow pointer was 0, so a lookup later returned YQ_OK with a
         * NULL value -- silent data loss instead of a clear error.
         */
        return YQ_ERR_TOOBIG;
    }

    yq_page_header hdr;
    read_page_header(page, &hdr);
    uint16_t nkeys = hdr.nkeys;

    int slot = find_slot(page, (const uint8_t *)key.data, key.size, nkeys, ps);
    if (slot < 0) return YQ_ERR_CORRUPT;
    if (slot < (int)nkeys) {
        uint16_t off = get_slot(page, (uint16_t)slot);
        const uint8_t *cell = page + off;
        size_t n = 0;
        uint64_t klen = 0;
        if (yq_varint_decode(cell, ps - off, &klen, &n) != YQ_OK) return YQ_ERR_CORRUPT;
        if ((size_t)klen == key.size && memcmp(cell + n, key.data, key.size) == 0) {
            /*
             * Duplicate key. The tree's contract is unique keys (checkpoint
             * feeds it a deduplicated sorted run), and silently storing a
             * second copy would make lookups return whichever copy the
             * binary search lands on.
             */
            return YQ_ERR_EXISTS;
        }
    }

    uint8_t stack_cell[2048];
    size_t cell_len = 0;
    uint8_t *cell = encode_leaf_cell(key, val, &cell_len, stack_cell, sizeof(stack_cell));
    if (!cell) return YQ_ERR_NOMEM;

    int rc = YQ_OK;
    if (hdr.free_bytes >= (uint16_t)(cell_len + YQ_PAGE_SLOT_SIZE)) {
        page_insert_cell(bt, page, nkeys, slot, cell, (uint16_t)cell_len);
        goto done;
    }

    /* --- split --- */
    if (nkeys == 0) {
        /*
         * An empty leaf that cannot hold one cell: the value (or key) is
         * larger than this page can ever hold. Splitting here would move
         * zero cells and leave both halves at nkeys == 0.
         */
        rc = YQ_ERR_TOOBIG;
        goto done;
    }
    uint16_t split_pos = nkeys / 2;
    uint8_t median[1024];
    size_t median_len = 0;
    if (nkeys > 0 && read_key_from_slot(page, split_pos, median, &median_len, ps) != 0) {
        rc = YQ_ERR_CORRUPT;
        goto done;
    }

    uint64_t right_no = 0;
    rc = btree_alloc_page(bt, &right_no);
    if (rc != YQ_OK) goto done;
    uint8_t *right = get_page_data(bt, right_no);
    if (!right) { rc = YQ_ERR_PANIC; goto done; }

    yq_page_header rhdr;
    memset(&rhdr, 0, sizeof(rhdr));
    rhdr.page_type = YQ_PAGE_TYPE_LEAF;
    rhdr.header_size = YQ_PAGE_HEADER_SIZE;
    rhdr.right_sibling = hdr.right_sibling;
    rhdr.left_sibling = page_no;

    uint16_t moved = (uint16_t)(nkeys - split_pos);
    uint16_t cursor = YQ_CELL_TOP(ps);
    for (uint16_t i = 0; i < moved; i++) {
        int csz = cell_size(page, split_pos + i, ps);
        if (csz < 0) { rc = YQ_ERR_CORRUPT; goto done; }
        uint16_t old_off = get_slot(page, split_pos + i);
        cursor = (uint16_t)(cursor - csz);
        memcpy(right + cursor, page + old_off, (size_t)csz);
        set_slot(right, i, cursor);
    }
    rhdr.nkeys = moved;
    rhdr.nkeys = moved;
    rhdr.free_bytes = (uint16_t)(cursor - YQ_SLOTS_END(moved));
    write_page_header(right, &rhdr);
    write_page_crc(right, ps);

    /*
     * The page that used to be this one's right neighbour now sits after the
     * new right page, so its left link has to be repointed. Skipping this
     * leaves a broken chain: forward scans follow right_sibling and look fine,
     * while cursor_prev walks left_sibling and silently steps over an entire
     * leaf -- which is how a backward scan over 2000 keys failed at the exact
     * key that followed the first split of a non-rightmost leaf.
     */
    if (hdr.right_sibling != 0) {
        uint8_t *after = get_page_data(bt, hdr.right_sibling);
        if (after) {
            yq_page_header ahdr;
            read_page_header(after, &ahdr);
            ahdr.left_sibling = right_no;
            write_page_header(after, &ahdr);
            write_page_crc(after, ps);
        }
    }

    /*
     * The left half keeps cells [0, split_pos), and they are no longer
     * contiguous -- the cells that moved out sat between them. Repack so
     * free_bytes describes the whole remaining gap instead of just the bytes
     * below the lowest cell.
     */
    hdr.nkeys = split_pos;
    hdr.right_sibling = right_no;
    write_page_header(page, &hdr);
    rc = page_repack(bt, page, split_pos);
    if (rc != YQ_OK) goto done;

    /*
     * Which half takes the new key. Both halves have roughly half a page of
     * gap, so the insert below cannot split again -- unless the cell is one
     * of the documented worst cases (1 KiB key + inline_max value on a 4 KiB
     * page). That corner returns YQ_ERR_TOOBIG rather than splitting a
     * one-cell page, which is what a naive re-split would do; stage 4's
     * overflow path removes the corner entirely.
     */
    int goes_left = (compare_key((const uint8_t *)key.data, key.size, median, median_len) < 0);
    uint8_t *target = goes_left ? page : right;
    uint16_t target_nkeys = goes_left ? split_pos : moved;
    yq_page_header thdr;
    read_page_header(target, &thdr);
    if (thdr.free_bytes < (uint16_t)(cell_len + YQ_PAGE_SLOT_SIZE)) {
        rc = YQ_ERR_TOOBIG;
        goto done;
    }
    int tslot = find_slot(target, (const uint8_t *)key.data, key.size, target_nkeys, ps);
    if (tslot < 0) { rc = YQ_ERR_CORRUPT; goto done; }
    page_insert_cell(bt, target, target_nkeys, tslot, cell, (uint16_t)cell_len);

    out->split = 1;
    out->right_page = right_no;
    memcpy(out->median, median, median_len);
    out->median_len = median_len;

done:
    if (cell != stack_cell) free(cell);
    return rc;
}

/*
 * Record a child split in an internal page.
 *
 * Layout (FORMAT.md 4.2): cell_i = (key_i, child_i) where child_i holds the
 * keys in [key_{i-1}, key_i), and the page's rightmost child holds everything
 * from key_{n-1} up. So when a child page splits into a left half (keys below
 * the separator) and a right half (keys from the separator on, separator
 * included -- leaves keep a copy of every separator), the parent needs TWO
 * edits, not one:
 *
 *   1. a new cell (separator, left_half) at the separator's lower-bound slot;
 *   2. the entry that used to name the left half -- cell[slot], or the
 *      rightmost field when the split page WAS the rightmost child -- repointed
 *      at the right half.
 *
 * Doing only (1) was the bug that kept a stale root alive: the root ended up
 * with rightmost pointing at the left half of the last leaf split, so every key
 * from that separator on looked up a page that no longer held it, and the tail
 * of the tree silently stopped being reachable.
 *
 * The repoint happens before the insert so that whichever piece of a parent
 * split carries the entry (left half, right half, or promoted to the
 * grandparent) carries the corrected pointer with it.
 */
static int internal_insert(yq_btree *bt, uint64_t page_no, uint8_t *page,
                           yq_slice key, uint64_t left_child, uint64_t right_child,
                           yq_split_info *out) {
    /* Carried for the COW stage (P3 S3): a page about to be modified must
     * first be copied to a fresh page number, and that copy is keyed by this
     * number. Nothing in this stage needs it yet. */
    split_info_init(out);
    (void)page_no;   /* see the comment above */

    uint32_t ps = bt->page_size;
    yq_page_header hdr;
    read_page_header(page, &hdr);
    uint16_t nkeys = hdr.nkeys;

    int slot = find_slot(page, (const uint8_t *)key.data, key.size, nkeys, ps);
    if (slot < 0) return YQ_ERR_CORRUPT;

    size_t klen_enc = 0;
    uint8_t kbuf[10];
    yq_varint_encode(key.size, kbuf, &klen_enc);
    uint16_t cell_len = (uint16_t)(klen_enc + key.size + 8);

    /* Repoint first; the insert below re-stamps the CRC either way. */
    if (slot < (int)nkeys) {
        uint16_t off = get_slot(page, (uint16_t)slot);
        uint8_t *target = page + off;
        size_t n = 0;
        uint64_t klen = 0;
        if (yq_varint_decode(target, ps - off, &klen, &n) != YQ_OK) return YQ_ERR_CORRUPT;
        memcpy(target + n + klen, &right_child, 8);
    } else {
        memcpy(page + YQ_SLOTS_END(nkeys), &right_child, 8);
    }

    uint8_t stack_cell[1100];
    uint8_t *cell = (cell_len <= sizeof(stack_cell)) ? stack_cell
                                                    : (uint8_t *)malloc(cell_len);
    if (!cell) return YQ_ERR_NOMEM;
    write_internal_cell(cell, kbuf, klen_enc, key.data, key.size, left_child);

    int rc = YQ_OK;
    if (hdr.free_bytes >= (uint16_t)(cell_len + YQ_PAGE_SLOT_SIZE)) {
        page_insert_cell(bt, page, nkeys, slot, cell, cell_len);
        goto done;
    }

    /* --- split --- */
    if (nkeys <= 1) {
        /*
         * One cell and no room for another: splitting would leave two
         * pages with no separators at all, and the next insert into either
         * would divide (nkeys - split_pos - 1) by zero. That means the page
         * is too small for the keys being stored -- say so instead.
         */
        rc = YQ_ERR_TOOBIG;
        goto done;
    }
    uint16_t split_pos = (uint16_t)(nkeys / 2);
    uint8_t median[1024];
    size_t median_len = 0;
    uint16_t median_off = get_slot(page, split_pos);
    if (read_key_from_slot(page, split_pos, median, &median_len, ps) != 0) {
        rc = YQ_ERR_CORRUPT;
        goto done;
    }
    uint64_t median_child = 0;
    if (median_off + median_len + 8 <= ps) {
        size_t enc = 0;
        uint64_t klen = 0;
        /* The cell is varint(klen) key child; the child sits right after. */
        if (yq_varint_decode(page + median_off, ps - median_off, &klen, &enc) == YQ_OK) {
            memcpy(&median_child, page + median_off + enc + klen, 8);
        }
    }
    uint64_t old_rightmost = 0;
    if (read_rightmost_child(page, ps, (int)nkeys, &old_rightmost) != YQ_OK) {
        rc = YQ_ERR_CORRUPT;
        goto done;
    }

    uint64_t right_no = 0;
    rc = btree_alloc_page(bt, &right_no);
    if (rc != YQ_OK) goto done;
    uint8_t *right = get_page_data(bt, right_no);
    if (!right) { rc = YQ_ERR_PANIC; goto done; }

    yq_page_header rhdr;
    memset(&rhdr, 0, sizeof(rhdr));
    rhdr.page_type = YQ_PAGE_TYPE_INTERNAL;
    rhdr.header_size = YQ_PAGE_HEADER_SIZE;
    rhdr.right_sibling = 0;
    rhdr.left_sibling = 0;

    uint16_t moved = (uint16_t)(nkeys - split_pos - 1);   /* the median cell is promoted */
    uint16_t cursor = YQ_CELL_TOP(ps);
    for (uint16_t i = 0; i < moved; i++) {
        int csz = cell_size(page, split_pos + 1 + i, ps);
        if (csz < 0) { rc = YQ_ERR_CORRUPT; goto done; }
        uint16_t old_off = get_slot(page, split_pos + 1 + i);
        cursor = (uint16_t)(cursor - csz);
        memcpy(right + cursor, page + old_off, (size_t)csz);
        set_slot(right, i, cursor);
    }
    rhdr.nkeys = moved;
    rhdr.free_bytes = (uint16_t)(cursor - (YQ_SLOTS_END(moved) + 8));
    memcpy(right + YQ_SLOTS_END(moved), &old_rightmost, 8);
    write_page_header(right, &rhdr);
    write_page_crc(right, ps);

       /*
     * Left keeps cells [0, split_pos) and inherits the median's child as its
     * rightmost child, which is written where the new slot array puts it so
     * page_repack() can carry it across to its final position.
     */
    hdr.nkeys = split_pos;
    memcpy(page + YQ_SLOTS_END(split_pos), &median_child, 8);
    write_page_header(page, &hdr);
    rc = page_repack(bt, page, split_pos);
    if (rc != YQ_OK) goto done;

    int goes_left = (compare_key((const uint8_t *)key.data, key.size, median, median_len) < 0);
    uint8_t *target = goes_left ? page : right;
    uint16_t target_nkeys = goes_left ? split_pos : moved;
    yq_page_header thdr;
    read_page_header(target, &thdr);
    if (thdr.free_bytes < (uint16_t)(cell_len + YQ_PAGE_SLOT_SIZE)) {
        rc = YQ_ERR_TOOBIG;
        goto done;
    }
    int tslot = find_slot(target, (const uint8_t *)key.data, key.size, target_nkeys, ps);
    if (tslot < 0) { rc = YQ_ERR_CORRUPT; goto done; }
    page_insert_cell(bt, target, target_nkeys, tslot, cell, cell_len);

    out->split = 1;
    out->right_page = right_no;
    memcpy(out->median, median, median_len);
    out->median_len = median_len;

done:
    if (cell != stack_cell) free(cell);
    return rc;
}

/*
 * Insert a unique key. Creates the root leaf on first use, descends recording
 * the path, then propagates any split upward: the separator goes into the
 * parent, and a parent that splits in turn promotes its own separator. If the
 * split reaches the top, a new root is created over the two halves.
 */
int yq_btree_insert(yq_btree *bt, yq_slice key, yq_slice val) {
    if (!bt) return YQ_ERR_INVAL;
    if (!bt->store) return YQ_ERR_NOTSUP;     /* P3: page numbers need a store */
    if (!key.data || key.size == 0 || key.size > 1024) return YQ_ERR_INVAL;

    if (!bt->has_root) {
        uint64_t root_no = 0;
        int rc = btree_alloc_page(bt, &root_no);
        if (rc != YQ_OK) return rc;
        uint8_t *root = get_page_data(bt, root_no);
        if (!root) return YQ_ERR_PANIC;

        yq_page_header hdr;
        memset(&hdr, 0, sizeof(hdr));
        hdr.page_type = YQ_PAGE_TYPE_LEAF;
        hdr.flags = 1;                      /* leaf that is also the root */
        hdr.header_size = YQ_PAGE_HEADER_SIZE;
        hdr.free_bytes = (uint16_t)(bt->page_size - YQ_PAGE_CRC_SIZE - YQ_PAGE_HEADER_SIZE);
        write_page_header(root, &hdr);
        write_page_crc(root, bt->page_size);

        bt->root_page = root_no;
        bt->has_root = 1;
    }

    uint64_t path[64];
    int path_len = 0;
    uint64_t cur = bt->root_page;

    yq_split_info info;
    uint64_t sep_left = 0, sep_right = 0;
    yq_slice sep_key;
    uint8_t sep_buf[1024];

    while (1) {
        if (path_len >= (int)(sizeof(path) / sizeof(path[0]))) return YQ_ERR_PANIC;
        uint8_t *page = get_page_data(bt, cur);
        if (!page) return YQ_ERR_PANIC;

        yq_page_header hdr;
        read_page_header(page, &hdr);

        if (hdr.page_type == YQ_PAGE_TYPE_LEAF) {
            int rc = leaf_insert(bt, cur, page, key, val, &info);
            if (rc != YQ_OK) return rc;
            if (!info.split) return YQ_OK;
            sep_left = cur;
            sep_right = info.right_page;
            memcpy(sep_buf, info.median, info.median_len);
            sep_key.data = sep_buf;
            sep_key.size = info.median_len;
            break;
        }

        path[path_len++] = cur;
        uint64_t child = 0;
        if (descend_child(page, bt->page_size, hdr.nkeys,
                          (const uint8_t *)key.data, key.size, &child) != YQ_OK) {
            return YQ_ERR_CORRUPT;
        }
        cur = child;
    }

    /* Propagate: each level inserts (separator, right_page) into its parent. */
    while (1) {
        if (path_len == 0) {
            /*
             * The split reached the top: publish a new root over the two
             * halves. Its single cell is packed at the bottom of the page
             * like every other cell -- writing it at YQ_SLOTS_END(0) lands
             * it on top of the slot array, where the following set_slot()
             * overwrites the cell's own key-length byte and the root becomes
             * garbage that only shows up one insert later as a wild page
             * number in the descent.
             */
            uint64_t new_root_no = 0;
            int rc = btree_alloc_page(bt, &new_root_no);
            if (rc != YQ_OK) return rc;
            uint8_t *nr = get_page_data(bt, new_root_no);
            if (!nr) return YQ_ERR_PANIC;

            uint8_t enc_buf[10];
            size_t enc = 0;
            yq_varint_encode(sep_key.size, enc_buf, &enc);
            size_t cell_len = enc + sep_key.size + 8;
            uint16_t cell_off = (uint16_t)(YQ_CELL_TOP(bt->page_size) - cell_len);

            yq_page_header hdr;
            memset(&hdr, 0, sizeof(hdr));
            hdr.page_type = YQ_PAGE_TYPE_INTERNAL;
            hdr.header_size = YQ_PAGE_HEADER_SIZE;
            hdr.nkeys = 1;
            hdr.free_bytes = (uint16_t)(cell_off - (YQ_SLOTS_END(1) + 8));
            write_page_header(nr, &hdr);

            uint8_t *cell = nr + cell_off;
            memcpy(cell, enc_buf, enc);
            memcpy(cell + enc, sep_key.data, sep_key.size);
            memcpy(cell + enc + sep_key.size, &sep_left, 8);
            set_slot(nr, 0, cell_off);
            memcpy(nr + YQ_SLOTS_END(1), &sep_right, 8);
            write_page_crc(nr, bt->page_size);

            bt->root_page = new_root_no;
            return YQ_OK;
        }
        uint64_t parent_no = path[--path_len];
        uint8_t *parent = get_page_data(bt, parent_no);
        if (!parent) return YQ_ERR_PANIC;

        int rc = internal_insert(bt, parent_no, parent, sep_key,
                                 sep_left, sep_right, &info);
        if (rc != YQ_OK) return rc;
        if (!info.split) return YQ_OK;

        sep_left = parent_no;
        sep_right = info.right_page;
        memcpy(sep_buf, info.median, info.median_len);
        sep_key.data = sep_buf;
        sep_key.size = info.median_len;
    }
}
yq_btree *yq_btree_create(yq_memblk *arena, uint32_t page_size) {
    yq_btree *bt = (yq_btree *)malloc(sizeof(yq_btree));
    if (!bt) return NULL;
    memset(bt, 0, sizeof(yq_btree));
    bt->page_size = page_size > 0 ? page_size : 4096;
    bt->arena = arena;
    bt->root_page = 0;
    bt->npages = 2;
    return bt;
}

/*
 * Release a B+Tree handle. The backing arena is NOT freed here: it is either
 * the caller's mmap window or a block owned by yq_db, and ownership stays
 * with whoever allocated it.
 */
void yq_btree_destroy(yq_btree *bt) {
    free(bt);
}

int yq_btree_lookup(yq_btree *bt, yq_slice key, yq_slice *out) {
    if (!yq_btree_has_root(bt)) return YQ_ERR_NOTFOUND;

    uint64_t cur_page = bt->root_page;
    uint32_t ps = bt->page_size;

    while (1) {
        uint8_t *page = get_page_data(bt, cur_page);
        if (!page) return YQ_ERR_IO;
        if (!check_page_crc(page, ps)) return YQ_ERR_CORRUPT;

        yq_page_header hdr;
        read_page_header(page, &hdr);

        if (hdr.page_type == YQ_PAGE_TYPE_LEAF) {
            int slot = find_slot(page, (const uint8_t *)key.data, key.size, hdr.nkeys, ps);
            if (slot >= (int)hdr.nkeys) return YQ_ERR_NOTFOUND;

            uint16_t offset = get_slot(page, (uint16_t)slot);
            uint8_t *cell = page + offset;
            size_t n = 0;
            uint64_t klen;
            if (yq_varint_decode(cell, ps - offset, &klen, &n) != 0) return YQ_ERR_CORRUPT;
            if (n + klen > (size_t)(ps - offset)) return YQ_ERR_CORRUPT;
            if (klen != key.size || memcmp(cell + n, key.data, klen) != 0) return YQ_ERR_NOTFOUND;

            size_t pos = n + klen;
            uint64_t val_len;
            if (yq_varint_decode(cell + pos, ps - offset - pos, &val_len, &n) != 0) return YQ_ERR_CORRUPT;
            pos += n;

            if (val_len <= YQ_INLINE_MAX(ps)) {
                out->data = cell + pos;
                out->size = (size_t)val_len;
            } else {
                /*
                 * Overflow values are not storable yet (P3 stage 4): the insert
                 * path refuses anything above inline_max, so a cell flagged
                 * overflow here means a damaged page or a newer writer.
                 * Report corruption instead of returning YQ_OK with a NULL
                 * value, which is how the draft silently lost data.
                 */
                (void)pos;
                return YQ_ERR_CORRUPT;;
            }
            return YQ_OK;
        }

        uint64_t child = 0;
        if (descend_child(page, ps, hdr.nkeys,
                          (const uint8_t *)key.data, key.size, &child) != YQ_OK) {
            return YQ_ERR_CORRUPT;
        }
        cur_page = child;
    }
}

int yq_btree_delete(yq_btree *bt, yq_slice key) {
    if (!yq_btree_has_root(bt)) return YQ_OK;

    uint64_t cur_page = bt->root_page;
    uint32_t ps = bt->page_size;

    while (1) {
        uint8_t *page = get_page_data(bt, cur_page);
        if (!page) return YQ_ERR_IO;
        if (!check_page_crc(page, ps)) return YQ_ERR_CORRUPT;

        yq_page_header hdr;
        read_page_header(page, &hdr);

        if (hdr.page_type == YQ_PAGE_TYPE_LEAF) {
            int slot = find_slot(page, (const uint8_t *)key.data, key.size, hdr.nkeys, ps);
            if (slot >= (int)hdr.nkeys) return YQ_OK;

            uint16_t offset = get_slot(page, (uint16_t)slot);
            int csz = cell_size(page, (uint16_t)slot, ps);
            if (csz < 0) return YQ_ERR_CORRUPT;

            /*
             * Only the cell at the bottom of the gap grows free_bytes when it
             * goes away. Removing a cell from the middle leaves a hole inside
             * the cell block, so claim nothing there rather than hand out space
             * that is still occupied.
             */
            uint16_t lowest = (uint16_t)(YQ_SLOTS_END(hdr.nkeys) + hdr.free_bytes);
            uint16_t reclaimed = (offset == lowest)
                               ? (uint16_t)(csz + YQ_PAGE_SLOT_SIZE) : 0;

            for (uint16_t i = (uint16_t)slot; i < hdr.nkeys - 1; i++) {
                set_slot(page, i, get_slot(page, i + 1));
            }

            hdr.nkeys--;
            hdr.free_bytes = (uint16_t)(hdr.free_bytes + reclaimed);
            write_page_header(page, &hdr);
            write_page_crc(page, ps);
            return YQ_OK;
        }

        uint64_t child = 0;
        if (descend_child(page, ps, hdr.nkeys,
                          (const uint8_t *)key.data, key.size, &child) != YQ_OK) {
            return YQ_ERR_CORRUPT;
        }
        cur_page = child;
    }
}

int yq_btree_open(yq_btree **out, void *mmap_base, uint64_t file_size, uint32_t page_size) {
    yq_btree *bt = (yq_btree *)malloc(sizeof(yq_btree));
    if (!bt) return YQ_ERR_NOMEM;
    memset(bt, 0, sizeof(yq_btree));
    bt->page_size = page_size;
    bt->mmap_base = mmap_base;
    bt->mmap_size = file_size;
    bt->npages = file_size / page_size;
    bt->root_page = 0;
    *out = bt;
    return YQ_OK;
}

void yq_btree_close(yq_btree *bt) {
    free(bt);
}

int yq_btree_get_root(yq_btree *bt, uint64_t *root_page) {
    *root_page = bt->root_page;
    return YQ_OK;
}

int yq_btree_set_root(yq_btree *bt, uint64_t root_page) {
    bt->root_page = root_page;
    bt->has_root = root_page != 0;
    return YQ_OK;
}

int yq_btree_cursor_open(yq_btree *bt, yq_btree_cursor **c) {
    yq_btree_cursor *cursor = (yq_btree_cursor *)malloc(sizeof(yq_btree_cursor));
    if (!cursor) return YQ_ERR_NOMEM;
    memset(cursor, 0, sizeof(yq_btree_cursor));
    cursor->bt = bt;
    cursor->valid = 0;
    *c = cursor;
    return YQ_OK;
}

void yq_btree_cursor_close(yq_btree_cursor *c) {
    free(c);
}

/*
 * Descend to the leftmost/rightmost leaf under `page_no`.
 *
 * The leftmost subtree hangs off cell 0 when the page has separators, and off
 * the rightmost child when it does not (nkeys == 0); the rightmost subtree is
 * always the rightmost child. Both used to ignore the separator array -- one
 * followed the page tail unconditionally, the other followed the LAST cell's
 * child -- which only happened to be right for a single-cell root, the shape
 * the hand-built test fixture had. A real two-level tree (P3) needs the
 * actual rule, so it comes from read_child_ptr().
 */
static int find_leftmost_leaf(yq_btree *bt, uint64_t page_no,
                              uint64_t *leaf_page, uint8_t **leaf_data) {
    uint32_t ps = bt->page_size;
    uint64_t cur = page_no;
    while (1) {
        uint8_t *page = get_page_data(bt, cur);
        if (!page) return YQ_ERR_IO;
        if (!check_page_crc(page, ps)) return YQ_ERR_CORRUPT;

        yq_page_header hdr;
        read_page_header(page, &hdr);
        if (hdr.page_type == YQ_PAGE_TYPE_LEAF) {
            *leaf_page = cur;
            *leaf_data = page;
            return YQ_OK;
        }
        uint64_t child = 0;
        int rc = read_child_ptr(page, ps, 0, (int)hdr.nkeys, &child);
        if (rc != YQ_OK) return rc;
        if (child == 0) return YQ_ERR_CORRUPT;
        cur = child;
    }
}

static int find_rightmost_leaf(yq_btree *bt, uint64_t page_no,
                               uint64_t *leaf_page, uint8_t **leaf_data) {
    uint32_t ps = bt->page_size;
    uint64_t cur = page_no;
    while (1) {
        uint8_t *page = get_page_data(bt, cur);
        if (!page) return YQ_ERR_IO;
        if (!check_page_crc(page, ps)) return YQ_ERR_CORRUPT;

        yq_page_header hdr;
        read_page_header(page, &hdr);
        if (hdr.page_type == YQ_PAGE_TYPE_LEAF) {
            *leaf_page = cur;
            *leaf_data = page;
            return YQ_OK;
        }
        uint64_t child = 0;
        int rc = read_rightmost_child(page, ps, (int)hdr.nkeys, &child);
        if (rc != YQ_OK) return rc;
        if (child == 0) return YQ_ERR_CORRUPT;
        cur = child;
    }
}

int yq_btree_cursor_first(yq_btree_cursor *c) {
    yq_btree *bt = c->bt;
    if (!yq_btree_has_root(bt)) {
        c->valid = 0;
        return YQ_ERR_NOTFOUND;
    }
    uint64_t leaf_page;
    uint8_t *leaf_data;
    int ret = find_leftmost_leaf(bt, bt->root_page, &leaf_page, &leaf_data);
    if (ret != YQ_OK) return ret;
    yq_page_header hdr;
    read_page_header(leaf_data, &hdr);
    if (hdr.nkeys == 0) {
        c->valid = 0;
        return YQ_ERR_NOTFOUND;
    }
    c->leaf_page = leaf_page;
    c->slot_idx = 0;
    c->slot_count = hdr.nkeys;
    c->valid = 1;
    return YQ_OK;
}

int yq_btree_cursor_last(yq_btree_cursor *c) {
    yq_btree *bt = c->bt;
    if (!yq_btree_has_root(bt)) {
        c->valid = 0;
        return YQ_ERR_NOTFOUND;
    }
    uint64_t leaf_page;
    uint8_t *leaf_data;
    int ret = find_rightmost_leaf(bt, bt->root_page, &leaf_page, &leaf_data);
    if (ret != YQ_OK) return ret;
    yq_page_header hdr;
    read_page_header(leaf_data, &hdr);
    if (hdr.nkeys == 0) {
        c->valid = 0;
        return YQ_ERR_NOTFOUND;
    }
    c->leaf_page = leaf_page;
    c->slot_idx = hdr.nkeys - 1;
    c->slot_count = hdr.nkeys;
    c->valid = 1;
    return YQ_OK;
}

int yq_btree_cursor_next(yq_btree_cursor *c) {
    if (!c->valid) return YQ_ERR_CURSOR;
    yq_btree *bt = c->bt;
    uint8_t *page = get_page_data(bt, c->leaf_page);
    if (!page) return YQ_ERR_IO;
    yq_page_header hdr;
    read_page_header(page, &hdr);

    c->slot_idx++;
    if (c->slot_idx < hdr.nkeys) {
        return YQ_OK;
    }

    if (hdr.right_sibling == 0) {
        c->valid = 0;
        return YQ_ERR_NOTFOUND;
    }

    c->leaf_page = hdr.right_sibling;
    uint8_t *next_page = get_page_data(bt, c->leaf_page);
    if (!next_page) return YQ_ERR_IO;
    read_page_header(next_page, &hdr);
    c->slot_idx = 0;
    c->slot_count = hdr.nkeys;
    if (hdr.nkeys == 0) {
        c->valid = 0;
        return YQ_ERR_NOTFOUND;
    }
    return YQ_OK;
}

int yq_btree_cursor_prev(yq_btree_cursor *c) {
    if (!c->valid) return YQ_ERR_CURSOR;
    yq_btree *bt = c->bt;
    uint8_t *page = get_page_data(bt, c->leaf_page);
    if (!page) return YQ_ERR_IO;
    yq_page_header hdr;
    read_page_header(page, &hdr);

    if (c->slot_idx > 0) {
        c->slot_idx--;
        return YQ_OK;
    }

    if (hdr.left_sibling == 0) {
        c->valid = 0;
        return YQ_ERR_NOTFOUND;
    }

    c->leaf_page = hdr.left_sibling;
    uint8_t *prev_page = get_page_data(bt, c->leaf_page);
    if (!prev_page) return YQ_ERR_IO;
    read_page_header(prev_page, &hdr);
    if (hdr.nkeys == 0) {
        c->valid = 0;
        return YQ_ERR_NOTFOUND;
    }
    c->slot_idx = hdr.nkeys - 1;
    c->slot_count = hdr.nkeys;
    return YQ_OK;
}

int yq_btree_cursor_key(yq_btree_cursor *c, yq_slice *out) {
    if (!c->valid) return YQ_ERR_CURSOR;
    yq_btree *bt = c->bt;
    uint8_t *page = get_page_data(bt, c->leaf_page);
    if (!page) return YQ_ERR_IO;

    uint16_t offset = get_slot(page, c->slot_idx);
    uint8_t *cell = page + offset;
    size_t n = 0;
    uint64_t klen;
    if (yq_varint_decode(cell, bt->page_size - offset, &klen, &n) != 0) return YQ_ERR_CORRUPT;
    if (n + klen > (size_t)(bt->page_size - offset)) return YQ_ERR_CORRUPT;
    out->data = cell + n;
    out->size = (size_t)klen;
    return YQ_OK;
}

int yq_btree_cursor_val(yq_btree_cursor *c, yq_slice *out) {
    if (!c->valid) return YQ_ERR_CURSOR;
    yq_btree *bt = c->bt;
    uint8_t *page = get_page_data(bt, c->leaf_page);
    if (!page) return YQ_ERR_IO;

    uint16_t offset = get_slot(page, c->slot_idx);
    uint8_t *cell = page + offset;
    size_t n = 0;
    uint64_t klen;
    if (yq_varint_decode(cell, bt->page_size - offset, &klen, &n) != 0) return YQ_ERR_CORRUPT;
    if (n + klen > (size_t)(bt->page_size - offset)) return YQ_ERR_CORRUPT;
    size_t pos = n + klen;
    uint64_t val_len;
    if (yq_varint_decode(cell + pos, bt->page_size - offset - pos, &val_len, &n) != 0) return YQ_ERR_CORRUPT;
    if (pos + n + val_len > (size_t)(bt->page_size - offset)) return YQ_ERR_CORRUPT;
    pos += n;
    out->data = cell + pos;
    out->size = (size_t)val_len;
    return YQ_OK;
}

/*
 * Position the cursor at the first key >= target ("lower bound").
 *
 * Descends from the root using find_slot (binary search on each page) and
 * lands on the leaf slot holding the first key >= target. If such a slot does
 * not exist in the current leaf (slot == nkeys), the target is greater than
 * every key of this leaf, so we advance along right_sibling and retry there;
 * with no right sibling the whole tree is exhausted and the cursor is set
 * invalid. On success the cursor state matches yq_btree_cursor_first:
 * valid=1, leaf_page/slot_idx/slot_count updated.
 */
int yq_btree_cursor_seek(yq_btree_cursor *c, yq_slice key) {
    yq_btree *bt = c->bt;
    if (!yq_btree_has_root(bt)) {
        c->valid = 0;
        return YQ_ERR_NOTFOUND;
    }

    uint64_t cur_page = bt->root_page;
    uint32_t ps = bt->page_size;

    while (1) {
        uint8_t *page = get_page_data(bt, cur_page);
        if (!page) return YQ_ERR_IO;
        if (!check_page_crc(page, ps)) return YQ_ERR_CORRUPT;

        yq_page_header hdr;
        read_page_header(page, &hdr);

        int slot = find_slot(page, (const uint8_t *)key.data, key.size, hdr.nkeys, ps);
        if (slot < 0) return YQ_ERR_CORRUPT;

        if (hdr.page_type == YQ_PAGE_TYPE_LEAF) {
            if (slot >= (int)hdr.nkeys) {
                if (hdr.right_sibling == 0) {
                    c->valid = 0;
                    return YQ_ERR_NOTFOUND;
                }
                cur_page = hdr.right_sibling;
                continue;
            }
            c->leaf_page = cur_page;
            c->slot_idx = (uint16_t)slot;
            c->slot_count = hdr.nkeys;
            c->valid = 1;
            return YQ_OK;
        }

        /* Internal page: the same routing rule lookup uses (see descend_child). */
        uint64_t child = 0;
        if (descend_child(page, ps, hdr.nkeys,
                          (const uint8_t *)key.data, key.size, &child) != YQ_OK) {
            return YQ_ERR_CORRUPT;
        }
        cur_page = child;
    }
}

/*
 * Position the cursor at the last key <= target ("floor").
 *
 * Starts from yq_btree_cursor_seek (first key >= target):
 *   - if the located key equals the target, that is the answer;
 *   - otherwise step back once to obtain the last key < target;
 *   - if seek found no key >= target (target greater than every key), fall
 *     back to yq_btree_cursor_last;
 *   - if seek landed on the very first key (all keys > target), stepping back
 *     leaves the tree and yields YQ_ERR_NOTFOUND, i.e. no key <= target.
 */
int yq_btree_cursor_seek_le(yq_btree_cursor *c, yq_slice key) {
    int rc = yq_btree_cursor_seek(c, key);
    if (rc == YQ_OK) {
        yq_slice k;
        if (yq_btree_cursor_key(c, &k) == YQ_OK &&
            compare_key((const uint8_t *)k.data, k.size,
                        (const uint8_t *)key.data, key.size) == 0) {
            return YQ_OK;
        }
        return yq_btree_cursor_prev(c);
    }
    if (rc == YQ_ERR_NOTFOUND) {
        return yq_btree_cursor_last(c);
    }
    return rc;
}

int yq_btree_cursor_valid(yq_btree_cursor *c) {
    return c->valid;
}

uint64_t yq_btree_npages(yq_btree *bt) {
    if (bt->store) return yq_page_store_npages(bt->store);
    return bt->npages;
}

void yq_btree_set_page_store(yq_btree *bt, yq_page_store *store) {
    bt->store = store;
    if (store) bt->npages = yq_page_store_npages(store);
}

yq_page_store *yq_btree_page_store(yq_btree *bt) {
    return bt->store;
}

int yq_btree_has_root(yq_btree *bt) {
    return bt->has_root && bt->root_page != 0;
}

void yq_btree_set_page_provider(yq_btree *bt, void *ctx,
    void *(*alloc)(void *ctx, int is_leaf),
    void (*free)(void *ctx, uint64_t page_no)) {
    bt->page_provider_ctx = ctx;
    bt->page_alloc = alloc;
    bt->page_free = free;
}

void yq_btree_set_file_provider(yq_btree *bt, void *ctx,
    void *(*alloc)(void *ctx, int is_leaf, uint64_t *out_page_no),
    void (*sync)(void *ctx)) {
    bt->file_ctx = ctx;
    bt->file_alloc = alloc;
    bt->file_sync = sync;
}

int yq_btree_set_npages(yq_btree *bt, uint64_t npages) {
    bt->npages = npages;
    if (bt->store) return yq_page_store_set_npages(bt->store, npages);
    return YQ_OK;
}

int yq_btree_flush_page(yq_btree *bt, uint64_t page_no, const void *page_data) {
    (void)bt;
    (void)page_no;
    (void)page_data;
    return YQ_OK;
}

void yq_btree_sync(yq_btree *bt) {
    if (bt->file_sync) {
        bt->file_sync(bt->file_ctx);
    }
}
