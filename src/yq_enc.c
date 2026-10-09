#include "yq_enc.h"
#include <string.h>
#include <stdint.h>

static uint32_t crc32c_table[256];

static void crc32c_init_table(void) {
    const uint32_t poly = 0x82F63B78;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) {
            c = (c >> 1) ^ (poly & -(c & 1));
        }
        crc32c_table[i] = c;
    }
}

static void crc32c_init_table_once(void) {
    static volatile int initialized = 0;
    if (!initialized) {
        crc32c_init_table();
        __atomic_store_n(&initialized, 1, __ATOMIC_RELEASE);
    }
}

uint32_t yq_crc32c(const void *data, size_t len) {
    if (len == 0) return 0;
    crc32c_init_table_once();
    const uint8_t *p = (const uint8_t *)data;
    const uint8_t *end = p + len;
    uint32_t crc = 0xFFFFFFFF;
    while (p + 4 <= end) {
        uint32_t word = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        crc = crc32c_table[(crc ^ word) & 0xFF] ^ (crc >> 8);
        crc = crc32c_table[(crc ^ (word >> 8)) & 0xFF] ^ (crc >> 8);
        crc = crc32c_table[(crc ^ (word >> 16)) & 0xFF] ^ (crc >> 8);
        crc = crc32c_table[(crc ^ (word >> 24)) & 0xFF] ^ (crc >> 8);
        p += 4;
    }
    while (p < end) {
        crc = crc32c_table[(crc ^ *p) & 0xFF] ^ (crc >> 8);
        p++;
    }
    return crc ^ 0xFFFFFFFF;
}

int yq_varint_encode(uint64_t val, uint8_t *out, size_t *nout) {
    if (!nout) return YQ_ERR_INVAL;
    if (val > (1ULL << 56)) return YQ_ERR_TOOBIG; /* 7 bytes max */
    
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
    if (!in || inlen == 0 || !out || !nconsumed) {
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
