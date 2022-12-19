/*
* Copyright 2022 Jetperch LLC
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

#include "jsdrv_prv/js220_i128.h"
#if _WIN32
#include <intrin.h>
#include <immintrin.h>
#endif
#include <math.h>
#include <stdbool.h>

// https://learn.microsoft.com/en-us/cpp/intrinsics/x64-amd64-intrinsics-list
// See https://docs.microsoft.com/en-us/cpp/intrinsics/udiv128
// See https://docs.microsoft.com/en-us/cpp/intrinsics/mul128


static const uint64_t U64_SIGN_BIT = 0x8000000000000000LLU;

#if defined(__clang__) || defined(__GNUC__)
js220_i128 js220_i128_add(js220_i128 a, js220_i128 b) {
    a.i128 = a.i128 + b.i128;
    return a;
}

js220_i128 js220_i128_sub(js220_i128 a, js220_i128 b) {
    a.i128 = a.i128 - b.i128;
    return a;
}

js220_i128 js220_i128_square_i64(int64_t a) {
    js220_i128 r;
    r.i128 = a;
    r.i128 = r.i128 * r.i128;
    return r;
}

#else

js220_i128 js220_i128_add(js220_i128 a, js220_i128 b) {
    js220_i128 r;
    uint8_t c = _addcarry_u64(0, a.u64[0], b.u64[0], &r.u64[0]);
    _addcarry_u64(c, a.u64[1], b.u64[1], &r.u64[1]);
    return r;
}

js220_i128 js220_i128_sub(js220_i128 a, js220_i128 b) {
    js220_i128 r;
    uint8_t c = _subborrow_u64(0, a.u64[0], b.u64[0], &r.u64[0]);
    _subborrow_u64(c, a.u64[1], b.u64[1], &r.u64[1]);
    return r;
}

js220_i128 js220_i128_square_i64(int64_t a) {
    js220_i128 r;
    if (a < 0) {
        a = -a;
    }
    r.u64[0] = _umul128(a, a, &r.u64[1]);
    return r;
}
#endif

js220_i128 js220_i128_neg(js220_i128 x) {
    x.u64[1] = ~x.u64[1];
    x.u64[0] = ~x.u64[0];

    uint64_t z1 = x.u64[0] & U64_SIGN_BIT;
    x.u64[0] += 1LLU;
    if ((x.u64[0] & U64_SIGN_BIT) != z1) {
        x.u64[1] += 1LLU;
    }
    return x;
}

js220_i128 js220_i128_udiv(js220_i128 dividend, uint64_t divisor, uint64_t * remainder) {
    uint64_t r = 0;
    if (NULL == remainder) {
        remainder = &r;
    }
    js220_i128 result;
#if defined(__clang__) || defined(__GNUC__)
    result.i128 = dividend.i128 / divisor;
    *remainder = dividend.i128 - (result.i128 * divisor);
#else
    result.u64[1] = dividend.u64[1] / divisor;
    dividend.u64[1] -= result.u64[1] * divisor;
    result.u64[0] = _udiv128(dividend.u64[1], dividend.u64[0], divisor, remainder);
#endif
    return result;
}

js220_i128 js220_i128_lshift(js220_i128 x, int32_t shift) {
    if (shift > 0) {
        x.u64[1] = (x.u64[1] << shift) | (x.u64[0] >> (64 - shift));
        x.u64[0] = x.u64[0] << shift;
    } else if (0 == shift) {
        // no operation
    } else {  // right shift signed
        shift = -shift;
        x.u64[0] = (x.u64[0] >> shift) | (x.u64[1] << (64 - shift));
        x.i64[1] = x.i64[1] >> shift;
    }
    return x;
}

js220_i128 js220_i128_rshift(js220_i128 x, int32_t shift) {
    return js220_i128_lshift(x, -shift);
}

double js220_i128_to_f64(js220_i128 x, uint32_t q) {
    double f;
    int32_t exponent = 128 - q - 64;
    bool is_neg = false;
    if (0 != (x.u64[1] & U64_SIGN_BIT)) {
        is_neg = true;
        x = js220_i128_neg(x);
    }

    if ((x.u64[0] == 0) && (x.u64[1] == 0)) {
        return 0.0;
    }

    while (0 == (x.u64[1] & U64_SIGN_BIT)) {
        x.u64[1] <<= 1;
        x.u64[1] |= (x.u64[0] >> 63) & 1;
        x.u64[0] <<= 1;
        --exponent;
    }
    f = (double) x.u64[1];     // more precision than a double's mantissa
    f *= pow(2, exponent);
    if (is_neg) {
        f = -f;
    }
    return f;
}

double js220_i128_compute_std(int64_t x1, js220_i128 x2, uint32_t n, uint32_t q) {
    double f;
    if (x1 < 0) {
        x1 = -x1;
    }
    js220_i128 m = js220_i128_square_i64(x1);
    js220_i128 d;

    d = js220_i128_udiv(m, n, NULL);

    if ((x2.u64[1] < d.u64[1]) ||
        ((x2.u64[1] == d.u64[1]) && (x2.u64[0] < d.u64[0]))) {
        // numerical precision error, return 0.
        return 0.0;
    }

    if (d.u64[0] > x2.u64[0]) {
        x2.u64[1] -= 1;
    }
    m.u64[0] = x2.u64[0] - d.u64[0];
    m.u64[1] = x2.u64[1] - d.u64[1];

    // n = n - 1;  // sample variance, not population variance
    d = js220_i128_udiv(m, n, NULL);
    // note: integer square root would have higher precision
    f = js220_i128_to_f64(d, q * 2);
    f = sqrt(f);
    return f;
}

js220_i128 js220_i128_compute_integral(js220_i128 x, uint32_t n) {
    if (x.i64[1] < 0) {
        x = js220_i128_neg(x);
        x = js220_i128_udiv(x, n, NULL);
        x = js220_i128_neg(x);
    } else {
        x = js220_i128_udiv(x, n, NULL);
    };
    return x;
}
