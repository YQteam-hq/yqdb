#include "yq_enc.h"
#include <string.h>
#include <stdint.h>

/*
 * CRC32C (Castagnoli), reflected form, polynomial 0x82F63B78.
 *
 * The engine's convention is the usual one: init 0xFFFFFFFF, final xor
 * 0xFFFFFFFF, and a checksum of 0 for an empty buffer. Every page trailer and
 * every WAL record header/payload is protected by this function, so it sits on
 * both the read and the write hot path: a point lookup verifies one page CRC
 * per tree level, and each logged PUT costs two CRCs over the payload.
 *
 * Two implementations are provided:
 *
 *   1. A slicing-by-8 software path. The original code consumed four table
 *      lookups per four input bytes; slicing-by-8 consumes eight lookups per
 *      eight bytes on independent tables, so the loads no longer sit on a
 *      single serial dependency chain. Portable C11, roughly 2-3x faster.
 *
 *   2. An SSE4.2 hardware path built on the CRC32 instruction. Selected at
 *      run time when the CPU advertises the feature; roughly an order of
 *      magnitude faster again.
 *
 * Both paths produce bit-identical results to the previous byte-at-a-time
 * implementation, so no on-disk format change is involved.
 */

#define YQ_CRC32C_POLY 0x82F63B78u

/* ── slicing-by-8 tables ───────────────────────────────────────────────── */

/*
 * Table 0 is the classic reflected CRC32C table. Table k (1..7) is the CRC of
 * one byte followed by k zero bytes, which is what lets the main loop fold
 * eight bytes per iteration.
 */
static uint32_t yq_crc32c_table[8][256];

static void yq_crc32c_build_tables(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) {
            c = (c >> 1) ^ (YQ_CRC32C_POLY & (0u - (c & 1u)));
        }
        yq_crc32c_table[0][i] = c;
    }
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = yq_crc32c_table[0][i];
        for (int k = 1; k < 8; k++) {
            c = (c >> 8) ^ yq_crc32c_table[0][c & 0xFF];
            yq_crc32c_table[k][i] = c;
        }
    }
}

static void yq_crc32c_init_tables_once(void) {
    static int initialized = 0;
    if (!initialized) {
        yq_crc32c_build_tables();
        initialized = 1;
    }
}

/* ── software path (slicing by 8) ──────────────────────────────────────── */

static uint32_t yq_crc32c_sw(const uint8_t *p, size_t len, uint32_t crc) {
    const uint8_t *end = p + len;
    const uint32_t (*t)[256] = yq_crc32c_table;

    while (p + 8 <= end) {
        uint32_t lo, hi;
        memcpy(&lo, p, 4);
        memcpy(&hi, p + 4, 4);
        uint32_t c = crc ^ lo;
        crc = t[7][c & 0xFF]           ^ t[6][(c >> 8) & 0xFF] ^
              t[5][(c >> 16) & 0xFF]   ^ t[4][(c >> 24) & 0xFF] ^
              t[3][hi & 0xFF]          ^ t[2][(hi >> 8) & 0xFF] ^
              t[1][(hi >> 16) & 0xFF]  ^ t[0][(hi >> 24) & 0xFF];
        p += 8;
    }
    while (p < end) {
        crc = t[0][(crc ^ *p) & 0xFF] ^ (crc >> 8);
        p++;
    }
    return crc;
}

/* ── hardware path (SSE4.2) ────────────────────────────────────────────── */

/*
 * The CRC32 instruction is x86-only and is not part of the base ISA, so it is
 * reached through GCC/Clang intrinsics plus a target attribute (the compiler
 * must be allowed to emit the instruction even when the translation unit as a
 * whole is not built with -msse4.2) and gated by a runtime CPU check. MSVC and
 * every non-x86 target simply keep using the software path.
 */
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
#include <nmmintrin.h>
#define YQ_CRC32C_HAVE_INTRINSIC 1
#endif

#if defined(YQ_CRC32C_HAVE_INTRINSIC)

__attribute__((target("sse4.2")))
static uint32_t yq_crc32c_hw(const uint8_t *p, size_t len, uint32_t crc) {
    const uint8_t *end = p + len;

#if defined(__x86_64__)
    while (p + 8 <= end) {
        uint64_t v;
        memcpy(&v, p, 8);
        crc = (uint32_t)_mm_crc32_u64((uint64_t)crc, v);
        p += 8;
    }
#endif
    while (p + 4 <= end) {
        uint32_t v;
        memcpy(&v, p, 4);
        crc = _mm_crc32_u32(crc, v);
        p += 4;
    }
    while (p < end) {
        crc = _mm_crc32_u8(crc, *p);
        p++;
    }
    return crc;
}

static int yq_crc32c_hw_available(void) {
    static int cached = -1;
    if (cached < 0) {
        cached = __builtin_cpu_supports("sse4.2") ? 1 : 0;
    }
    return cached;
}

#endif /* YQ_CRC32C_HAVE_INTRINSIC */

uint32_t yq_crc32c(const void *data, size_t len) {
    if (len == 0) return 0;

    yq_crc32c_init_tables_once();

    const uint8_t *p = (const uint8_t *)data;
    const uint32_t init = 0xFFFFFFFFu;

#if defined(YQ_CRC32C_HAVE_INTRINSIC)
    if (yq_crc32c_hw_available()) {
        return yq_crc32c_hw(p, len, init) ^ 0xFFFFFFFFu;
    }
#endif

    return yq_crc32c_sw(p, len, init) ^ 0xFFFFFFFFu;
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
