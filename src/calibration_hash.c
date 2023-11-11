/*
 * Copyright 2014-2023 Jetperch LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// https://en.wikipedia.org/wiki/Salsa20

#include "jsdrv.h"

#define ROUNDS 20
#define DOUBLE_ROUNDS (ROUNDS / 2)

// c, _ = monocypher.generate_signing_key_pair()
// c = np.frombuffer(c, dtype=np.uint8).view(np.uint32)
// print(', '.join([f'0x{x:08x}' for x in c]))
const uint32_t chacha20_constant[4] = {0x381377d5U, 0x4b62bff4U, 0x349dcc7bU, 0x845b865fU};  // custom & random

static const uint8_t rounds[8][4] = {
        {0, 4,  8, 12}, // column 0
        {1, 5,  9, 13}, // column 1
        {2, 6, 10, 14}, // column 2
        {3, 7, 11, 15}, // column 3
        {0, 5, 10, 15}, // diagonal 0
        {1, 6, 11, 12}, // diagonal 1
        {2, 7,  8, 13}, // diagonal 2
        {3, 4,  9, 14}, // diagonal 3
};

static inline uint32_t rotl(uint32_t a, uint32_t b) {
    return (a << b) | (a >> (32 - b));
}

static void QR(uint32_t * ap, uint32_t * bp, uint32_t * cp, uint32_t * dp) {
    uint32_t a = *ap;
    uint32_t b = *bp;
    uint32_t c = *cp;
    uint32_t d = *dp;
    a += b;  d ^= a;  d = rotl(d, 16);
    c += d;  b ^= c;  b = rotl(b, 12);
    a += b;  d ^= a;  d = rotl(d,  8);
    c += d;  b ^= c;  b = rotl(b,  7);
    *ap = a;
    *bp = b;
    *cp = c;
    *dp = d;
}

static void chacha20_block(uint32_t x[16]) {
    for (uint32_t i = 0; i < DOUBLE_ROUNDS; ++i) {
        for (uint32_t j = 0; j < 8; ++j) {
            QR(&x[rounds[j][0]], &x[rounds[j][1]], &x[rounds[j][2]], &x[rounds[j][3]]);
        }
    }
}

/**
 * @brief Copy 32-bit, aligned values.
 *
 * @param dst The destination buffer.
 * @param src The source buffer.
 * @param length The length of of the copy in 32-bit words.
 */
static void copy_u32(uint32_t * dst, const uint32_t * src, uint32_t length) {
    for (uint32_t i = 0; i < length; ++i) {
        *dst++ = *src++;
    }
}

// See https://en.wikipedia.org/wiki/One-way_compression_function
void jsdrv_calibration_hash(const uint32_t * msg, uint32_t length, uint32_t * hash) {
    uint32_t i, j;
    uint32_t h[16];
    // assert(0 == (length & 0x1f));            // must be multiple of 32 bytes
    const uint32_t length_u32 = length >> 2;    // Compute length in 32-bit words

    for (i = 0; i < 16; ++i) {
        hash[i] = 0;                            // Initialize hash to zero
    }
    copy_u32(&h[0], chacha20_constant, 4);      // Initialize chaining state

    // Each block operates on 32 message bytes
    for (i = 0; i < length_u32; i += 8) {
        copy_u32(&h[0], hash, 4);               // [3:0]  chaining, reuse from the previous result
        copy_u32(&h[4], chacha20_constant, 3);  // [6:4]  constant, reduce attack surface
        h[7] = i;                               // [7]    offset counter, ensure order matters
        copy_u32(&h[8], &msg[i], 8);            // [15:8] message chunk
        chacha20_block(h);

        // Compression: XOR with existing hash
        for (j = 0; j < 16; ++j) {
            hash[j] ^= h[j];
        }
    }
}
