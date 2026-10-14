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

/*
 * Hardware path (SSE4.2).
 *
 * The CRC32 instruction folds 8 bytes per instruction with a 3-cycle
 * dependency chain, which is roughly an order of magnitude faster than the
 * table lookups -- and every B+Tree page read verifies one of these checksums,
 * so it is worth having. The instruction is not part of the base x86 ISA, so it
 * needs three things this build does not otherwise assume:
 *
 *   - GCC/Clang intrinsics plus a target attribute, because the translation
 *     unit is not compiled with -msse4.2 and the compiler would otherwise
 *     refuse to emit the instruction;
 *   - a run-time CPU check, so the same binary still runs on pre-Haswell parts;
 *   - an MSVC / non-x86 fallback, which keeps using slicing-by-8.
 *
 * The result is bit-identical to the software path (same reflected polynomial,
 * same init and final xor), so nothing on disk changes.
 */
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
#include <nmmintrin.h>
#define YQ_CRC32C_HAVE_INTRINSIC 1
#endif

#if defined(YQ_CRC32C_HAVE_INTRINSIC)

__attribute__((target("sse4.2")))
static uint32_t crc32c_hw(const uint8_t *p, size_t len, uint32_t crc) {
    const uint8_t *end = p + len;

#if defined(__x86_64__)
    while (p + 8 <= end) {
        uint64_t v;
        memcpy(&v, p, sizeof(v));
        crc = (uint32_t)_mm_crc32_u64((uint64_t)crc, v);
        p += 8;
    }
#endif
    while (p + 4 <= end) {
        uint32_t v;
        memcpy(&v, p, sizeof(v));
        crc = _mm_crc32_u32(crc, v);
        p += 4;
    }
    while (p < end) {
        crc = _mm_crc32_u8(crc, *p);
        p++;
    }
    return crc;
}

/*
 * Cached CPU probe.
 *
 * __builtin_cpu_supports() itself is cheap but not free, and this sits on the
 * page-read path, so the answer is memoised. The flag is an atomic rather than
 * a plain int for the same reason crc32c_ready is: a torn or stale read of a
 * plain int is undefined behaviour, and two threads racing here would both
 * compute the same value anyway.
 */
#if defined(_WIN32)
static volatile LONG crc32c_hw_ready = -1;
#else
static atomic_int crc32c_hw_ready;
#endif

static int crc32c_hw_available(void) {
#if defined(_WIN32)
    LONG v = InterlockedCompareExchange(&crc32c_hw_ready, -1, -1);
    if (v == -1) {
        v = __builtin_cpu_supports("sse4.2") ? 1 : 0;
        InterlockedExchange(&crc32c_hw_ready, v);
    }
    return (int)v;
#else
    int v = atomic_load_explicit(&crc32c_hw_ready, memory_order_acquire);
    if (v < 0) {
        v = __builtin_cpu_supports("sse4.2") ? 1 : 0;
        atomic_store_explicit(&crc32c_hw_ready, v, memory_order_release);
    }
    return v;
#endif
}

#endif /* YQ_CRC32C_HAVE_INTRINSIC */

uint32_t yq_crc32c(const void *data, size_t len) {
    if (len == 0) return 0;

    /*
     * The hardware path does not touch the tables, so on a CPU with
     * SSE4.2 the 8 KiB of slicing-by-8 tables are never built at all -- and
     * the init call disappears from the page-read hot path.
     */
#if defined(YQ_CRC32C_HAVE_INTRINSIC)
    if (crc32c_hw_available()) {
        return crc32c_hw((const uint8_t *)data, len, 0xFFFFFFFFu) ^ 0xFFFFFFFFu;
    }
#endif

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
    /*
     * Single-byte fast path, mirroring the decode side's up-front check. Values
     * below 128 cover the overwhelming majority of key/value lengths, the small
     * page offsets used throughout the B+Tree, and most WAL payload sizes. This
     * routine runs on every WAL append (yq_wal_append_put/del), every B+Tree
     * cell encode, and every meta-block serialization, so handling the common
     * case first skips the loop, the per-iteration mask/shift, and the branchy
     * tail.
     */
    if (val < 0x80) {
        if (out) out[0] = (uint8_t)val;
        *nout = 1;
        return YQ_OK;
    }
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

    /*
     * Fast path: the overwhelmingly common case is a single-byte varint, i.e.
     * a value below 128. That covers most key/value lengths, the small page
     * offsets used throughout the B+Tree, and a large share of WAL payload
     * sizes. Handling it up front skips the multi-byte loop, the per-iteration
     * shift/overflow bookkeeping and the branchy tail entirely -- and this
     * routine runs on every B+Tree probe, every memtable comparison and every
     * WAL record scan, so the saving is paid back constantly.
     */
    uint8_t first = in[0];
    if ((first & 0x80) == 0) {
        *out = first;
        *nconsumed = 1;
        return YQ_OK;
    }

    uint64_t val = first & 0x7F;
    size_t n = 1;
    int shift = 7;
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
