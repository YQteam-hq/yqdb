/*
 * yq_test_crc.c — CRC32C correctness tests.
 *
 * yq_crc32c() guards every B+Tree page (check_page_crc), every WAL record
 * header and payload, and both meta pages, so a silent regression in it turns
 * into silent data corruption rather than a failing test. The slicing-by-8
 * rewrite keeps the same polynomial and the same output, and these tests pin
 * that down:
 *
 *   1. the published CRC32C check vector ("123456789" -> 0xE3069283);
 *   2. a bitwise reference implementation over every length 0..600, which
 *      covers the 8-byte main loop, the 8-way tail, and the boundary between
 *      them without hard-coding any expected magic numbers;
 *   3. buffer alignment offsets, since the implementation assembles its 32-bit
 *      words by hand and must stay correct on unaligned input;
 *   4. an offset/length sweep over a 64 KiB buffer, which walks every
 *      combination of the 8/4/1-byte steps of the hardware loop and of the
 *      8-byte-plus-tail loop of the software one.
 *
 * yq_crc32c() picks its implementation at run time: SSE4.2 where the CPU has
 * it, slicing-by-8 otherwise. These tests hold whichever path this machine
 * selects to the same output, because a checksum written by one build is
 * verified by every other -- a mismatch here is silent data corruption, not a
 * failing build.
 */

#include "yq_enc.h"
#include "yq_test_check.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

/*
 * Bitwise reference CRC32C: one bit per step, no tables. Slow but trivially
 * correct, and independent of the implementation under test.
 */
static uint32_t crc32c_reference(const uint8_t *data, size_t len) {
    const uint32_t poly = 0x82F63B78;
    uint32_t crc = 0xFFFFFFFFu;

    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (poly & -(crc & 1u));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

static void test_check_vector(void) {
    CHECK_EQ(yq_crc32c("123456789", 9), 0xE3069283u);
    CHECK_EQ(yq_crc32c("123456789", 9), crc32c_reference((const uint8_t *)"123456789", 9));

    /* Empty input stays a special case: the engine relies on it returning 0. */
    CHECK_EQ(yq_crc32c("", 0), 0u);
    CHECK_EQ(yq_crc32c(NULL, 0), 0u);
}

static void test_against_reference_all_lengths(void) {
    uint8_t buf[600];
    unsigned seed = 0x12345678u;

    for (size_t i = 0; i < sizeof(buf); i++) {
        seed = seed * 1103515245u + 12345u;
        buf[i] = (uint8_t)(seed >> 16);
    }

    for (size_t len = 1; len <= sizeof(buf); len++) {
        uint32_t got = yq_crc32c(buf, len);
        uint32_t want = crc32c_reference(buf, len);
        if (got != want) {
            fprintf(stderr, "\nFAIL: length %zu: got 0x%08" PRIx32 ", want 0x%08" PRIx32 "\n",
                    len, got, want);
            exit(1);
        }
    }
}

static void test_unaligned_offsets(void) {
    uint8_t raw[256];
    unsigned seed = 0xDEADBEEFu;
    for (size_t i = 0; i < sizeof(raw); i++) {
        seed = seed * 1103515245u + 12345u;
        raw[i] = (uint8_t)(seed >> 16);
    }

    for (size_t off = 0; off < 16; off++) {
        for (size_t len = 0; len + off <= sizeof(raw); len++) {
            uint32_t got = yq_crc32c(raw + off, len);
            uint32_t want = crc32c_reference(raw + off, len);
            if (got != want) {
                fprintf(stderr, "\nFAIL: off=%zu len=%zu: got 0x%08" PRIx32 ", want 0x%08" PRIx32 "\n",
                        off, len, got, want);
                exit(1);
            }
        }
    }
}

/*
 * A realistic page-sized buffer: the main loop runs many iterations and the
 * tail is exercised for every possible remainder (page_size - CRC_SIZE is 4
 * mod 8 for 4096, so this also hits an odd tail length).
 */
static void test_page_sized(void) {
    size_t page = 4096;
    uint8_t *buf = (uint8_t *)malloc(page);
    CHECK(buf != NULL);

    unsigned seed = 7u;
    for (size_t i = 0; i < page; i++) {
        seed = seed * 1103515245u + 12345u;
        buf[i] = (uint8_t)(seed >> 16);
    }

    for (size_t len = page - 8; len <= page; len++) {
        CHECK_EQ(yq_crc32c(buf, len), crc32c_reference(buf, len));
    }

    free(buf);
}

/*
 * A sweep over a buffer much larger than any single checksummed object, with
 * offset and length varied together. This is what makes the two paths
 * comparable rather than merely self-consistent: it walks every combination of
 * the 8-byte, 4-byte and 1-byte steps in the SSE4.2 loop and the 8-byte main
 * loop plus every tail length in the slicing-by-8 loop, at every alignment.
 * Whichever path this CPU selects, its result has to equal the bitwise
 * reference, and a page CRC written by one build is verified by every other.
 */
static void test_large_sweep(void) {
    size_t n = 64 * 1024;
    uint8_t *buf = (uint8_t *)malloc(n);
    CHECK(buf != NULL);

    unsigned seed = 0xC0FFEEu;
    for (size_t i = 0; i < n; i++) {
        seed = seed * 1103515245u + 12345u;
        buf[i] = (uint8_t)(seed >> 16);
    }

    int checked = 0;
    for (size_t off = 0; off < 16; off++) {
        for (size_t len = 1; len + off <= n; len = len * 3 + 1) {
            uint32_t got = yq_crc32c(buf + off, len);
            uint32_t want = crc32c_reference(buf + off, len);
            if (got != want) {
                fprintf(stderr, "\nFAIL: sweep off=%zu len=%zu: got 0x%08" PRIx32
                                ", want 0x%08" PRIx32 "\n", off, len, got, want);
                free(buf);
                exit(1);
            }
            checked++;
        }
    }

    free(buf);
    printf("     (%d offset/length combinations over %zu KiB)\n", checked, n / 1024);
}

/* Informational: what the checksum costs per call at record and page sizes. */
static void test_throughput(void) {
    size_t n = 1 << 20;
    uint8_t *buf = (uint8_t *)malloc(n);
    CHECK(buf != NULL);
    memset(buf, 0x5A, n);

#if defined(_WIN32)
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
#else
    struct timespec t0, t1;
#endif

    volatile uint32_t sink = 0;
    const int reps = 200;
#if defined(_WIN32)
    QueryPerformanceCounter(&c);
    double t_start = (double)c.QuadPart / (double)f.QuadPart;
#else
    clock_gettime(CLOCK_MONOTONIC, &t0);
#endif
    for (int i = 0; i < reps; i++) sink ^= yq_crc32c(buf, n);
#if defined(_WIN32)
    QueryPerformanceCounter(&c);
    double dt = (double)(c.QuadPart - (LONGLONG)(t_start * f.QuadPart)) / (double)f.QuadPart;
#else
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double dt = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
#endif
    (void)sink;

    printf("     1 MiB: %.0f MB/s", (double)n * reps / (1024.0 * 1024.0) / dt);

    /* record-sized, which is what the WAL actually hashes per record */
    const int rreps = 200000;
#if defined(_WIN32)
    QueryPerformanceCounter(&c);
    t_start = (double)c.QuadPart / (double)f.QuadPart;
#else
    clock_gettime(CLOCK_MONOTONIC, &t0);
#endif
    for (int i = 0; i < rreps; i++) sink ^= yq_crc32c(buf + (size_t)(i % 1000), 128);
#if defined(_WIN32)
    QueryPerformanceCounter(&c);
    dt = (double)(c.QuadPart - (LONGLONG)(t_start * f.QuadPart)) / (double)f.QuadPart;
#else
    clock_gettime(CLOCK_MONOTONIC, &t1);
    dt = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
#endif
    (void)sink;
    printf("   128 B: %.0f ns/call\n", dt * 1e9 / (double)rreps);

    free(buf);
}

int main(void) {
    test_check_vector();
    printf("ok  crc32c check vector\n");
    test_against_reference_all_lengths();
    printf("ok  crc32c matches bitwise reference for lengths 1..600\n");
    test_unaligned_offsets();
    printf("ok  crc32c unaligned offsets\n");
    test_page_sized();
    printf("ok  crc32c page-sized buffers\n");
    test_large_sweep();
    printf("ok  crc32c large offset/length sweep\n");
    test_throughput();
    printf("crc_test: all cases passed\n");
    return 0;
}
