#include "yq_enc.h"
#include <string.h>
#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <stdatomic.h>
#endif

/*
 * CRC32C (Castagnoli, reflected polynomial 0x82F63B78) using the
 * "slicing-by-8" algorithm.
 *
 * The previous implementation consumed one byte per table lookup. Every B+Tree
 * page read verifies its CRC (check_page_crc) and every page write recomputes
 * it, and every WAL record stamps both a header and a payload CRC, so this
 * function sits on the hottest paths in the engine.
 *
 * Slicing-by-8 keeps 8 tables of 256 entries (8 KiB total, still comfortably
 * inside L1) and consumes 8 input bytes per iteration, which removes the
 * serial dependency chain of the byte-at-a-time form -- each of the 8 lookups
 * in one step depends only on the input, not on the previous lookup.
 *
 * The generated values are bit-identical to the byte-at-a-time version: the
 * extra tables are derived from table 0 by the standard recurrence
 * t[n][i] = (t[n-1][i] >> 8) ^ t[0][t[n-1][i] & 0xFF], which is exactly the
 * algebraic folding of 8 successive byte steps.
 */
#define YQ_CRC32C_SLICES 8

static uint32_t crc32c_tables[YQ_CRC32C_SLICES][256];

/*
 * One-time table generation.
 *
 * Two threads may race into crc32c_build_tables(); that is benign because the
 * computation is deterministic and both write the identical result, and every
 * write is a plain aligned uint32_t. What must not happen is a thread *reading*
 * a half-built table, which is why the flag is published only after the tables
 * are complete and read with acquire semantics (an Interlocked* call is a full
 * barrier on Windows).
 */
#if defined(_WIN32)
static volatile LONG crc32c_ready = 0;
#else
static atomic_int crc32c_ready;
#endif

static void crc32c_build_tables(void) {
    const uint32_t poly = 0x82F63B78;

    /* Slice 0 is the classic byte-at-a-time CRC32C table. */
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) {
            c = (c >> 1) ^ (poly & -(c & 1));
        }
        crc32c_tables[0][i] = c;
    }

    /* Slices 1..7 fold one extra byte step into the previous slice. */
    for (uint32_t s = 1; s < YQ_CRC32C_SLICES; s++) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = crc32c_tables[s - 1][i];
            crc32c_tables[s][i] = (v >> 8) ^ crc32c_tables[0][v & 0xFF];
        }
    }
}

static int crc32c_ready_get(void) {
#if defined(_WIN32)
    return InterlockedCompareExchange(&crc32c_ready, 0, 0) != 0;
#else
    return atomic_load_explicit(&crc32c_ready, memory_order_acquire) != 0;
#endif
}

static void crc32c_ready_set(void) {
#if defined(_WIN32)
    InterlockedExchange(&crc32c_ready, 1);
#else
    atomic_store_explicit(&crc32c_ready, 1, memory_order_release);
#endif
}

static void crc32c_init_table_once(void) {
    if (crc32c_ready_get()) {
        return;
    }
    crc32c_build_tables();
    crc32c_ready_set();
}

uint32_t yq_crc32c(const void *data, size_t len) {
    if (len == 0) return 0;
    crc32c_init_table_once();

    const uint8_t *p = (const uint8_t *)data;
    size_t n = len;
    uint32_t crc = 0xFFFFFFFFu;

    /*
     * Bytes are assembled little-endian by hand rather than read through a
     * uint32_t* so the function stays correct on unaligned buffers and on
     * strict-alignment targets, matching the previous implementation.
     */
    while (n >= 8) {
        uint32_t lo = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                      ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        uint32_t hi = (uint32_t)p[4] | ((uint32_t)p[5] << 8) |
                      ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);

        crc ^= lo;
        crc = crc32c_tables[7][crc & 0xFF] ^
              crc32c_tables[6][(crc >> 8) & 0xFF] ^
              crc32c_tables[5][(crc >> 16) & 0xFF] ^
              crc32c_tables[4][(crc >> 24)];

        crc ^= crc32c_tables[3][hi & 0xFF] ^
               crc32c_tables[2][(hi >> 8) & 0xFF] ^
               crc32c_tables[1][(hi >> 16) & 0xFF] ^
               crc32c_tables[0][(hi >> 24)];

        p += 8;
        n -= 8;
    }

    /*
     * Remainder: one 4-byte slicing step (tables 3..0) when at least 4 bytes
     * are left, then a byte-at-a-time tail. Without this step a short record
     * -- a 29-byte WAL header, or the 96-byte meta block -- would fall
     * straight through to the byte loop and lose most of the gain.
     */
    if (n >= 4) {
        uint32_t word = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        crc ^= word;
        crc = crc32c_tables[3][crc & 0xFF] ^
              crc32c_tables[2][(crc >> 8) & 0xFF] ^
              crc32c_tables[1][(crc >> 16) & 0xFF] ^
              crc32c_tables[0][(crc >> 24)];
        p += 4;
        n -= 4;
    }

    while (n--) {
        crc = crc32c_tables[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    }

    return crc ^ 0xFFFFFFFFu;
}

int yq_varint_encode(uint64_t val, uint8_t *out, size_t *nout) {
    size_t n = 0;
    while (val >= 0x80) {
        if (out) out[n] = (uint8_t)((val & 0x7F) | 0x80);
        n++;
        val >>= 7;
    }
    if (out) out[n] = (uint8_t)val;
    n++;
    *nout = n;
    return YQ_OK;
}

int yq_varint_decode(const uint8_t *in, size_t inlen, uint64_t *out, size_t *nconsumed) {
    if (inlen == 0) {
        return YQ_ERR_INVAL;
    }
    uint64_t val = 0;
    size_t n = 0;
    int shift = 0;
    while (n < inlen) {
        uint8_t b = in[n];
        val |= (uint64_t)(b & 0x7F) << shift;
        n++;
        if ((b & 0x80) == 0) {
            *out = val;
            *nconsumed = n;
            return YQ_OK;
        }
        shift += 7;
        if (shift > 63) {
            return YQ_ERR_INVAL;
        }
    }
    return YQ_ERR_INVAL;
}
