#include "yq_btree.h"
#include "yq_enc.h"
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
#define YQ_BTREE_MAX_PATH_DEPTH 64

#define YQ_INLINE_MAX(ps)     ((ps) / 4)

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
    uint64_t npages;
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

static uint8_t *alloc_page(yq_btree *bt, int is_leaf) {
    if (!bt) return NULL;
    if (bt->page_alloc) {
        return (uint8_t *)bt->page_alloc(bt->page_provider_ctx, is_leaf);
    }
    if (bt->arena) {
        return (uint8_t *)yq_memblk_alloc(bt->arena, bt->page_size);
    }
    uint8_t *page = (uint8_t *)malloc(bt->page_size);
    if (page) memset(page, 0, bt->page_size);
    return page;
}

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
            return (int)(pos + val_len - offset);
        } else {
            pos += 8;
            uint64_t total_len;
            if (yq_varint_decode(cell + pos, page_size - offset - pos, &total_len, &n) != 0) return -1;
            return (int)(pos + n - offset);
        }
    } else {
        return (int)(pos + 8 - offset);
    }
}

static int compare_key(const uint8_t *key1, size_t len1, const uint8_t *key2, size_t len2) {
    size_t min_len = len1 < len2 ? len1 : len2;
    if (min_len > 0) {
        int cmp = memcmp(key1, key2, min_len);
        if (cmp != 0) return cmp < 0 ? -1 : 1;
    }
    if (len1 < len2) return -1;
    if (len1 > len2) return 1;
    return 0;
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
        int cmp = compare_key(cell + pos, klen, key, key_len);
        if (cmp == 0) return mid;
        if (cmp < 0) lo = mid + 1;
        else hi = mid - 1;
    }
    return lo;
}

static int read_key_from_slot(const uint8_t *page, uint16_t slot_idx, uint8_t *key_buf, size_t *key_len, uint32_t page_size) {
    if (!page || !key_buf || !key_len) return -1;
    if (slot_idx >= YQ_INLINE_MAX(page_size)) return -1;
    
    uint16_t offset = get_slot(page, slot_idx);
    if (offset >= page_size) return -1;
    
    uint8_t *cell = (uint8_t *)page + offset;
    size_t n = 0;
    if (yq_varint_decode(cell, page_size - offset, key_len, &n) != 0) return -1;
    if (*key_len > 1024 || *key_len > page_size - offset - n) return -1;
    
    memcpy(key_buf, cell + n, *key_len);
    return 0;
}

static int insert_into_leaf(yq_btree *bt, uint64_t page_no, uint8_t *page,
                            yq_slice key, yq_slice val, uint64_t *out_new_page, uint8_t **out_new_page_data) {
    yq_page_header hdr;
    read_page_header(page, &hdr);
    uint32_t ps = bt->page_size;
    uint16_t nkeys = hdr.nkeys;

    size_t key_len_enc = 0, val_len_enc = 0;
    uint8_t key_len_buf[10], val_len_buf[10];
    yq_varint_encode(key.size, key_len_buf, &key_len_enc);
    yq_varint_encode(val.size, val_len_buf, &val_len_enc);

    int is_overflow = val.size > YQ_INLINE_MAX(ps);
    size_t cell_total;
    if (is_overflow) {
        size_t total_enc = 0;
        uint8_t total_buf[10];
        yq_varint_encode(val.size, total_buf, &total_enc);
        cell_total = key_len_enc + key.size + 8 + total_enc;
    } else {
        cell_total = key_len_enc + key.size + val_len_enc + val.size;
    }

    uint16_t needed = (uint16_t)cell_total;

    if (hdr.free_bytes >= needed + YQ_PAGE_SLOT_SIZE) {
        uint16_t new_offset = hdr.free_bytes - needed;
        int slot = find_slot(page, (const uint8_t *)key.data, key.size, nkeys, ps);
        if (slot < 0) return YQ_ERR_CORRUPT;

        for (uint16_t i = nkeys; i > (uint16_t)slot; i--) {
            set_slot(page, i, get_slot(page, i - 1));
        }
        set_slot(page, (uint16_t)slot, new_offset);

        uint8_t *cell = page + new_offset;
        size_t pos = 0;
        memcpy(cell + pos, key_len_buf, key_len_enc);
        pos += key_len_enc;
        memcpy(cell + pos, key.data, key.size);
        pos += key.size;

        if (is_overflow) {
            *(uint64_t *)(cell + pos) = 0;
            pos += 8;
            size_t total_enc = 0;
            uint8_t total_buf[10];
            yq_varint_encode(val.size, total_buf, &total_enc);
            memcpy(cell + pos, total_buf, total_enc);
        } else {
            memcpy(cell + pos, val_len_buf, val_len_enc);
            pos += val_len_enc;
            memcpy(cell + pos, val.data, val.size);
        }

        hdr.nkeys++;
        hdr.free_bytes = new_offset - (YQ_PAGE_HEADER_SIZE + (nkeys + 1) * YQ_PAGE_SLOT_SIZE);
        write_page_header(page, &hdr);
        write_page_crc(page, ps);
        *out_new_page = page_no;
        *out_new_page_data = page;
        return YQ_OK;
    }

    uint16_t split_pos = nkeys / 2;
    uint8_t *new_page = alloc_page(bt, 1);
    if (!new_page) return YQ_ERR_NOMEM;

    yq_page_header new_hdr = {0};
    new_hdr.page_type = YQ_PAGE_TYPE_LEAF;
    new_hdr.flags = 0;
    new_hdr.nkeys = 0;
    new_hdr.right_sibling = hdr.right_sibling;
    new_hdr.left_sibling = page_no;
    new_hdr.header_size = YQ_PAGE_HEADER_SIZE;
    new_hdr.free_bytes = ps - YQ_PAGE_HEADER_SIZE;

    for (uint16_t i = 0; i < nkeys - split_pos; i++) {
        uint16_t old_offset = get_slot(page, split_pos + i);
        uint16_t new_offset = (uint16_t)(ps - YQ_PAGE_CRC_SIZE - (i * YQ_PAGE_SLOT_SIZE) - (cell_size(page, split_pos + i, ps)));
        set_slot(new_page, i, new_offset);
        uint8_t *src_cell = page + old_offset;
        uint8_t *dst_cell = new_page + new_offset;
        int csz = cell_size(page, split_pos + i, ps);
        memcpy(dst_cell, src_cell, csz);
        new_hdr.nkeys++;
    }

    uint8_t *insert_key_buf = (uint8_t *)malloc(key.size);
    if (!insert_key_buf) {
        free(new_page);
        return YQ_ERR_NOMEM;
    }
    memcpy(insert_key_buf, key.data, key.size);

    uint8_t split_key_buf[1024];
    size_t split_key_len = 0;
    if (read_key_from_slot(page, split_pos, split_key_buf, &split_key_len, ps) != 0) {
        free(insert_key_buf);
        free(new_page);
        return YQ_ERR_CORRUPT;
    }

    int cmp = compare_key(insert_key_buf, key.size, split_key_buf, split_key_len);

    if (cmp < 0) {
        uint16_t new_offset = (uint16_t)(ps - YQ_PAGE_CRC_SIZE - (nkeys - split_pos) * YQ_PAGE_SLOT_SIZE - needed);
        uint8_t *cell = new_page + new_offset;
        size_t pos = 0;
        memcpy(cell + pos, key_len_buf, key_len_enc);
        pos += key_len_enc;
        memcpy(cell + pos, key.data, key.size);
        pos += key.size;
        if (is_overflow) {
            *(uint64_t *)(cell + pos) = 0;
            pos += 8;
            size_t total_enc = 0;
            uint8_t total_buf[10];
            yq_varint_encode(val.size, total_buf, &total_enc);
            memcpy(cell + pos, total_buf, total_enc);
        } else {
            memcpy(cell + pos, val_len_buf, val_len_enc);
            pos += val_len_enc;
            memcpy(cell + pos, val.data, val.size);
        }
        for (uint16_t i = new_hdr.nkeys; i > 0; i--) {
            set_slot(new_page, i, get_slot(new_page, i - 1));
        }
        set_slot(new_page, 0, new_offset);
        new_hdr.nkeys++;
    } else {
        uint16_t new_offset = (uint16_t)(new_hdr.free_bytes - needed);
        uint8_t *cell = new_page + new_offset;
        size_t pos = 0;
        memcpy(cell + pos, key_len_buf, key_len_enc);
        pos += key_len_enc;
        memcpy(cell + pos, key.data, key.size);
        pos += key.size;
        if (is_overflow) {
            *(uint64_t *)(cell + pos) = 0;
            pos += 8;
            size_t total_enc = 0;
            uint8_t total_buf[10];
            yq_varint_encode(val.size, total_buf, &total_enc);
            memcpy(cell + pos, total_buf, total_enc);
        } else {
            memcpy(cell + pos, val_len_buf, val_len_enc);
            pos += val_len_enc;
            memcpy(cell + pos, val.data, val.size);
        }
        set_slot(new_page, new_hdr.nkeys, new_offset);
        new_hdr.nkeys++;
    }
    free(insert_key_buf);

    new_hdr.free_bytes = YQ_PAGE_HEADER_SIZE + new_hdr.nkeys * YQ_PAGE_SLOT_SIZE;
    write_page_header(new_page, &new_hdr);
    write_page_crc(new_page, ps);

    hdr.nkeys = split_pos;
    hdr.right_sibling = bt->npages;
    hdr.free_bytes = YQ_PAGE_HEADER_SIZE + split_pos * YQ_PAGE_SLOT_SIZE;
    write_page_header(page, &hdr);
    write_page_crc(page, ps);

    *out_new_page = bt->npages;
    *out_new_page_data = new_page;
    bt->npages++;
    return 2;
}

static int insert_into_internal(yq_btree *bt, uint64_t page_no, uint8_t *page,
                                yq_slice key, uint64_t left_child, uint64_t right_child,
                                uint64_t *out_new_page, uint8_t **out_new_page_data) {
    (void)left_child;
    yq_page_header hdr;
    read_page_header(page, &hdr);
    uint32_t ps = bt->page_size;
    uint16_t nkeys = hdr.nkeys;

    size_t key_len_enc = 0;
    uint8_t key_len_buf[10];
    yq_varint_encode(key.size, key_len_buf, &key_len_enc);
    size_t cell_total = key_len_enc + key.size + 8;

    if (hdr.free_bytes >= (uint16_t)cell_total + YQ_PAGE_SLOT_SIZE) {
        int slot = find_slot(page, (const uint8_t *)key.data, key.size, nkeys, ps);
        if (slot < 0) return YQ_ERR_CORRUPT;

        uint16_t new_offset = hdr.free_bytes - (uint16_t)cell_total;
        for (uint16_t i = nkeys; i > (uint16_t)slot; i--) {
            set_slot(page, i, get_slot(page, i - 1));
        }
        set_slot(page, (uint16_t)slot, new_offset);

        uint8_t *cell = page + new_offset;
        size_t pos = 0;
        memcpy(cell + pos, key_len_buf, key_len_enc);
        pos += key_len_enc;
        memcpy(cell + pos, key.data, key.size);
        pos += key.size;
        *(uint64_t *)(cell + pos) = right_child;

        hdr.nkeys++;
        hdr.free_bytes = new_offset - (YQ_PAGE_HEADER_SIZE + nkeys * YQ_PAGE_SLOT_SIZE);
        write_page_header(page, &hdr);
        write_page_crc(page, ps);
        *out_new_page = page_no;
        *out_new_page_data = page;
        return YQ_OK;
    }

    uint16_t split_pos = nkeys / 2;
    uint8_t *new_page = alloc_page(bt, 0);
    if (!new_page) return YQ_ERR_NOMEM;

    yq_page_header new_hdr = {0};
    new_hdr.page_type = YQ_PAGE_TYPE_INTERNAL;
    new_hdr.flags = 0;
    new_hdr.nkeys = 0;
    new_hdr.header_size = YQ_PAGE_HEADER_SIZE;
    new_hdr.free_bytes = ps - YQ_PAGE_HEADER_SIZE - 8;

    uint16_t new_slot_area_size = (nkeys - split_pos) * YQ_PAGE_SLOT_SIZE;
    for (uint16_t i = 0; i < nkeys - split_pos; i++) {
        uint16_t old_offset = get_slot(page, split_pos + i);
        uint16_t new_offset = (uint16_t)(ps - 8 - new_slot_area_size - (i * YQ_PAGE_SLOT_SIZE) - (cell_size(page, split_pos + i, ps)));
        set_slot(new_page, i, new_offset);
        uint8_t *src_cell = page + old_offset;
        uint8_t *dst_cell = new_page + new_offset;
        int csz = cell_size(page, split_pos + i, ps);
        memcpy(dst_cell, src_cell, csz);
        new_hdr.nkeys++;
    }

    uint8_t *rightmost = (uint8_t *)(page + ps - 8);
    *(uint64_t *)(new_page + ps - 8) = *(uint64_t *)rightmost;

    if (compare_key((const uint8_t *)key.data, key.size,
                    page + get_slot(page, split_pos) + 1,
                    *(uint16_t *)(page + get_slot(page, split_pos))) >= 0) {
        uint16_t new_offset = (uint16_t)(new_hdr.free_bytes - cell_total);
        uint8_t *cell = new_page + new_offset;
        size_t pos = 0;
        memcpy(cell + pos, key_len_buf, key_len_enc);
        pos += key_len_enc;
        memcpy(cell + pos, key.data, key.size);
        pos += key.size;
        *(uint64_t *)(cell + pos) = right_child;
        set_slot(new_page, new_hdr.nkeys, new_offset);
        new_hdr.nkeys++;
    } else {
        uint16_t new_offset = (uint16_t)(hdr.free_bytes - cell_total);
        uint8_t *cell = page + new_offset;
        size_t pos = 0;
        memcpy(cell + pos, key_len_buf, key_len_enc);
        pos += key_len_enc;
        memcpy(cell + pos, key.data, key.size);
        pos += key.size;
        *(uint64_t *)(cell + pos) = right_child;
        for (uint16_t i = hdr.nkeys; i > (uint16_t)split_pos; i--) {
            set_slot(page, i, get_slot(page, i - 1));
        }
        set_slot(page, (uint16_t)split_pos, new_offset);
        hdr.nkeys++;
    }

    new_hdr.free_bytes = YQ_PAGE_HEADER_SIZE + new_hdr.nkeys * YQ_PAGE_SLOT_SIZE + 8;
    write_page_header(new_page, &new_hdr);
    write_page_crc(new_page, ps);

    hdr.free_bytes = YQ_PAGE_HEADER_SIZE + hdr.nkeys * YQ_PAGE_SLOT_SIZE + 8;
    write_page_header(page, &hdr);
    write_page_crc(page, ps);

    *out_new_page = bt->npages;
    *out_new_page_data = new_page;
    bt->npages++;
    return 2;
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

int yq_btree_insert(yq_btree *bt, yq_slice key, yq_slice val) {
    if (bt->root_page == 0) {
        uint8_t *root = alloc_page(bt, 1);
        if (!root) return YQ_ERR_NOMEM;

        yq_page_header hdr = {0};
        hdr.page_type = YQ_PAGE_TYPE_LEAF;
        hdr.flags = 1;
        hdr.nkeys = 0;
        hdr.header_size = YQ_PAGE_HEADER_SIZE;
        hdr.free_bytes = bt->page_size - YQ_PAGE_HEADER_SIZE;
        write_page_header(root, &hdr);
        write_page_crc(root, bt->page_size);
        bt->root_page = 0;
        bt->npages = 2;
    }

    uint64_t path_pnos[64];
    int path_len = 0;

    uint64_t cur_page = bt->root_page;
    while (1) {
        uint8_t *page = get_page_data(bt, cur_page);
        if (!page) return YQ_ERR_NOMEM;
        yq_page_header hdr;
        read_page_header(page, &hdr);

        if (hdr.page_type == YQ_PAGE_TYPE_LEAF) {
            uint64_t new_page = 0;
            uint8_t *new_page_data = NULL;
            int ret = insert_into_leaf(bt, cur_page, page, key, val, &new_page, &new_page_data);
            (void)new_page_data;
            if (ret == YQ_OK) return YQ_OK;
            if (ret == YQ_ERR_NOMEM) return ret;

            uint8_t *left_page = page;
            uint8_t *right_page = alloc_page(bt, 1);
            if (!right_page) return YQ_ERR_NOMEM;

            yq_page_header left_hdr, right_hdr;
            read_page_header(left_page, &left_hdr);
            read_page_header(right_page, &right_hdr);

            right_hdr = left_hdr;
            right_hdr.right_sibling = left_hdr.right_sibling;
            right_hdr.left_sibling = cur_page + 1;
            write_page_header(right_page, &right_hdr);

            left_hdr.right_sibling = bt->npages;
            write_page_header(left_page, &left_hdr);

            yq_slice median_key;
            uint8_t key_buf[1025];
            size_t key_len = 0;
            read_key_from_slot(right_page, 0, key_buf, &key_len, bt->page_size);
            median_key.data = key_buf;
            median_key.size = key_len;

            if (path_len == 0) {
                uint8_t *new_root = alloc_page(bt, 0);
                if (!new_root) return YQ_ERR_NOMEM;
                yq_page_header new_root_hdr = {0};
                new_root_hdr.page_type = YQ_PAGE_TYPE_INTERNAL;
                new_root_hdr.nkeys = 1;
                new_root_hdr.header_size = YQ_PAGE_HEADER_SIZE;
                new_root_hdr.free_bytes = bt->page_size - YQ_PAGE_HEADER_SIZE - YQ_PAGE_SLOT_SIZE - 8;
                set_slot(new_root, 0, (uint16_t)(bt->page_size - 8 - YQ_PAGE_SLOT_SIZE - YQ_VARINT_MAX_SIZE - key_len));
                uint8_t *root_cell = new_root + get_slot(new_root, 0);
                size_t enc_len = 0;
                yq_varint_encode(key_len, root_cell, &enc_len);
                memcpy(root_cell + enc_len, key_buf, key_len);
                *(uint64_t *)(root_cell + enc_len + key_len) = bt->npages;
                *(uint64_t *)(new_root + bt->page_size - 8) = cur_page;
                write_page_header(new_root, &new_root_hdr);
                write_page_crc(new_root, bt->page_size);
                bt->root_page = bt->npages;
                bt->npages++;
                return YQ_OK;
            }

            path_len--;
            uint64_t parent_page = path_pnos[path_len];
            uint8_t *parent_data = get_page_data(bt, parent_page);
            if (!parent_data) return YQ_ERR_NOMEM;

            yq_page_header parent_hdr;
            read_page_header(parent_data, &parent_hdr);
            if (parent_hdr.page_type == YQ_PAGE_TYPE_INTERNAL) {
                size_t enc_len = 0;
                yq_varint_encode(key_len, key_buf, &enc_len);
                uint64_t dummy_new = 0;
                uint8_t *dummy_data = NULL;
                int iret = insert_into_internal(bt, parent_page, parent_data, median_key, cur_page, bt->npages, &dummy_new, &dummy_data);
                (void)dummy_new;
                (void)dummy_data;
                (void)iret;
            }
            bt->npages++;
            return YQ_OK;
        }

        if (path_len >= YQ_BTREE_MAX_PATH_DEPTH) return YQ_ERR_PANIC;
        path_pnos[path_len] = cur_page;
        path_len++;

        int slot = find_slot(page, (const uint8_t *)key.data, key.size, hdr.nkeys, bt->page_size);
        uint64_t child;
        if (slot >= (int)hdr.nkeys) {
            child = *(uint64_t *)(page + bt->page_size - 8);
        } else {
            uint16_t offset = get_slot(page, (uint16_t)slot);
            uint8_t *cell = page + offset;
            size_t n = 0;
            uint64_t klen;
            yq_varint_decode(cell, bt->page_size - offset, &klen, &n);
            child = *(uint64_t *)(cell + n + klen);
        }
        cur_page = child;
    }
}

int yq_btree_lookup(yq_btree *bt, yq_slice key, yq_slice *out) {
    if (bt->root_page == 0) return YQ_ERR_NOTFOUND;

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
            if (klen != key.size || memcmp(cell + n, key.data, klen) != 0) return YQ_ERR_NOTFOUND;

            size_t pos = n + klen;
            uint64_t val_len;
            if (yq_varint_decode(cell + pos, ps - offset - pos, &val_len, &n) != 0) return YQ_ERR_CORRUPT;
            pos += n;

            if (val_len <= YQ_INLINE_MAX(ps)) {
                out->data = cell + pos;
                out->size = (size_t)val_len;
            } else {
                uint64_t overflow_page = *(uint64_t *)(cell + pos);
                (void)overflow_page;
                out->data = NULL;
                out->size = 0;
            }
            return YQ_OK;
        }

        int slot = find_slot(page, (const uint8_t *)key.data, key.size, hdr.nkeys, ps);
        uint64_t child;
        if (slot >= (int)hdr.nkeys) {
            child = *(uint64_t *)(page + ps - 8);
        } else {
            uint16_t offset = get_slot(page, (uint16_t)slot);
            uint8_t *cell = page + offset;
            size_t n = 0;
            uint64_t klen;
            yq_varint_decode(cell, ps - offset, &klen, &n);
            child = *(uint64_t *)(cell + n + klen);
        }
        cur_page = child;
    }
}

int yq_btree_delete(yq_btree *bt, yq_slice key) {
    if (bt->root_page == 0) return YQ_OK;

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
            (void)offset;
            int csz = cell_size(page, (uint16_t)slot, ps);
            if (csz < 0) return YQ_ERR_CORRUPT;

            for (uint16_t i = (uint16_t)slot; i < hdr.nkeys - 1; i++) {
                set_slot(page, i, get_slot(page, i + 1));
            }

            uint16_t new_free = hdr.free_bytes + (uint16_t)csz;
            hdr.nkeys--;
            hdr.free_bytes = new_free;
            write_page_header(page, &hdr);
            write_page_crc(page, ps);
            return YQ_OK;
        }

        int slot = find_slot(page, (const uint8_t *)key.data, key.size, hdr.nkeys, ps);
        uint64_t child;
        if (slot >= (int)hdr.nkeys) {
            child = *(uint64_t *)(page + ps - 8);
        } else {
            uint16_t offset = get_slot(page, (uint16_t)slot);
            uint8_t *cell = page + offset;
            size_t n = 0;
            uint64_t klen;
            yq_varint_decode(cell, ps - offset, &klen, &n);
            child = *(uint64_t *)(cell + n + klen);
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

static int find_leftmost_leaf(yq_btree *bt, uint64_t page_no, uint64_t *leaf_page, uint8_t **leaf_data) {
    uint32_t ps = bt->page_size;
    uint64_t cur = page_no;
    while (1) {
        uint8_t *page = get_page_data(bt, cur);
        if (!page) return YQ_ERR_IO;
        yq_page_header hdr;
        read_page_header(page, &hdr);
        if (hdr.page_type == YQ_PAGE_TYPE_LEAF) {
            *leaf_page = cur;
            *leaf_data = page;
            return YQ_OK;
        }
        uint64_t child = *(uint64_t *)(page + ps - 8);
        cur = child;
    }
}

static int find_rightmost_leaf(yq_btree *bt, uint64_t page_no, uint64_t *leaf_page, uint8_t **leaf_data) {
    uint32_t ps = bt->page_size;
    uint64_t cur = page_no;
    while (1) {
        uint8_t *page = get_page_data(bt, cur);
        if (!page) return YQ_ERR_IO;
        yq_page_header hdr;
        read_page_header(page, &hdr);
        if (hdr.page_type == YQ_PAGE_TYPE_LEAF) {
            *leaf_page = cur;
            *leaf_data = page;
            return YQ_OK;
        }
        uint16_t last_slot = hdr.nkeys > 0 ? hdr.nkeys - 1 : 0;
        uint16_t offset = get_slot(page, last_slot);
        uint8_t *cell = page + offset;
        size_t n = 0;
        uint64_t klen;
        yq_varint_decode(cell, ps - offset, &klen, &n);
        uint64_t child = *(uint64_t *)(cell + n + klen);
        cur = child;
    }
}

int yq_btree_cursor_first(yq_btree_cursor *c) {
    yq_btree *bt = c->bt;
    if (bt->root_page == 0) {
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
    if (bt->root_page == 0) {
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
    size_t pos = n + klen;
    uint64_t val_len;
    if (yq_varint_decode(cell + pos, bt->page_size - offset - pos, &val_len, &n) != 0) return YQ_ERR_CORRUPT;
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
    if (bt->root_page == 0) {
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

        /* Internal page: follow the same descent rule as yq_btree_lookup. */
        uint64_t child;
        if (slot >= (int)hdr.nkeys) {
            child = *(uint64_t *)(page + ps - 8);
        } else {
            uint16_t offset = get_slot(page, (uint16_t)slot);
            uint8_t *cell = page + offset;
            size_t n = 0;
            uint64_t klen;
            if (yq_varint_decode(cell, ps - offset, &klen, &n) != 0) return YQ_ERR_CORRUPT;
            child = *(uint64_t *)(cell + n + klen);
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
    return bt->npages;
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
