#include "yq_enc.h"
#include <string.h>
#include <stdint.h>

#define YQ_VARINT_MAX_SIZE 10
#define YQ_CRC32C_MAX_SIZE (1ULL << 30)

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
    /* Validate input parameters */
    if (!data || len > YQ_CRC32C_MAX_SIZE) {
        return 0; /* Return 0 for invalid input */
    }
    
    /* Handle zero-length case */
    if (len == 0) return 0;
    
    crc32c_init_table_once();
    const uint8_t *p = (const uint8_t *)data;
    const uint8_t *end = p + len;
    uint32_t crc = 0xFFFFFFFF;
    
    /* Process 4-byte chunks */
    while (p + 4 <= end) {
        uint32_t word = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        crc = crc32c_table[(crc ^ word) & 0xFF] ^ (crc >> 8);
        crc = crc32c_table[(crc ^ (word >> 8)) & 0xFF] ^ (crc >> 8);
        crc = crc32c_table[(crc ^ (word >> 16)) & 0xFF] ^ (crc >> 8);
        crc = crc32c_table[(crc ^ (word >> 24)) & 0xFF] ^ (crc >> 8);
        p += 4;
    }
    
    /* Process remaining bytes */
    while (p < end) {
        crc = crc32c_table[(crc ^ *p) & 0xFF] ^ (crc >> 8);
        p++;
    }
    
    return crc ^ 0xFFFFFFFF;
}

int yq_varint_encode(uint64_t val, uint8_t *out, size_t *nout) {
    /* Validate input parameters */
    if (!nout) return YQ_ERR_INVAL;
    
    /* Validate output buffer size */
    if (out && *nout > YQ_VARINT_MAX_SIZE) {
        return YQ_ERR_INVAL;
    }
    
    /* Handle zero value case */
    if (val == 0) {
        if (out && *nout > 0) {
            out[0] = 0;
        }
        *nout = 1;
        return YQ_OK;
    }
    
    size_t n = 0;
    uint64_t temp_val = val;
    
    /* Calculate encoded size and validate bounds */
    while (temp_val >= 0x80) {
        n++;
        temp_val >>= 7;
        if (n >= YQ_VARINT_MAX_SIZE) {
            return YQ_ERR_INVAL; /* Value too large to encode */
        }
    }
    n++;
    
    /* Validate output buffer has enough space */
    if (out && *nout < n) {
        return YQ_ERR_INVAL;
    }
    
    /* Encode the value */
    if (out) {
        size_t i = 0;
        while (val >= 0x80) {
            out[i] = (uint8_t)((val & 0x7F) | 0x80);
            i++;
            val >>= 7;
        }
        out[i] = (uint8_t)val;
    }
    
    *nout = n;
    return YQ_OK;
}

int yq_varint_decode(const uint8_t *in, size_t inlen, uint64_t *out, size_t *nconsumed) {
    /* Validate input parameters */
    if (!in || !out || !nconsumed || inlen == 0) {
        return YQ_ERR_INVAL;
    }
    
    /* Validate input buffer size bounds */
    if (inlen > YQ_VARINT_MAX_SIZE) {
        return YQ_ERR_INVAL;
    }
    
    uint64_t val = 0;
    size_t n = 0;
    int shift = 0;
    
    while (n < inlen) {
        uint8_t b = in[n];
        
        /* Validate shift bounds before shifting */
        if (shift > 63) {
            return YQ_ERR_INVAL;
        }
        
        val |= (uint64_t)(b & 0x7F) << shift;
        n++;
        
        /* Check if this is the last byte */
        if ((b & 0x80) == 0) {
            /* Validate that the value doesn't overflow */
            if (val > UINT64_MAX >> shift) {
                return YQ_ERR_INVAL;
            }
            *out = val;
            *nconsumed = n;
            return YQ_OK;
        }
        
        shift += 7;
        
        /* Prevent infinite loop and validate shift bounds */
        if (shift >= 70) { /* 10 bytes * 7 bits = 70 bits max */
            return YQ_ERR_INVAL;
        }
    }
    
    /* If we get here, we ran out of bytes before finding a terminating byte */
    return YQ_ERR_INVAL;
}
