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
 *      words by hand and must stay correct on unaligned input.
 */

#include "yq_enc.h"
#include "yq_test_check.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int main(void) {
    test_check_vector();
    printf("ok  crc32c check vector\n");
    test_against_reference_all_lengths();
    printf("ok  crc32c matches bitwise reference for lengths 1..600\n");
    test_unaligned_offsets();
    printf("ok  crc32c unaligned offsets\n");
    test_page_sized();
    printf("ok  crc32c page-sized buffers\n");
    printf("crc_test: all cases passed\n");
    return 0;
}
