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

#include "jsdrv_prv/devices/js220/js220_i128.h"
#include <math.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Implementation selection
//
// Exactly one of the JS220_I128_IMPL_* macros is defined below; every
// conditional in this file selects on these macros only. To support a new
// platform, add a branch here -- not scattered throughout the file.
//
//   INT128       clang/gcc: native __int128 extension.
//                See https://gcc.gnu.org/onlinedocs/gcc/_005f_005fint128.html
//   X86_INTRIN   MSVC on x86/x64: _addcarry_u64, _subborrow_u64, _umul128, _udiv128.
//                See https://learn.microsoft.com/en-us/cpp/intrinsics/x64-amd64-intrinsics-list
//   PORTABLE     Pure C 64-bit ops. Used by MSVC on ARM64 (no __int128, and
//                the _addcarry/_umul128/_udiv128 intrinsics are x86/x64-only).
//
// JS220_I128_USE_UMULH is an optional optimization for the portable multiply
// helper: ARM64 MSVC has a native high-multiply instruction exposed as __umulh.
// ---------------------------------------------------------------------------

#if defined(__clang__) || defined(__GNUC__)
#  define JS220_I128_IMPL_INT128
#elif defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
#  define JS220_I128_IMPL_X86_INTRIN
#else
#  define JS220_I128_IMPL_PORTABLE
#endif

#if defined(JS220_I128_IMPL_PORTABLE) && defined(_MSC_VER) && (defined(_M_ARM64) || defined(_M_ARM64EC))
#  define JS220_I128_USE_UMULH
#endif

#if defined(JS220_I128_IMPL_X86_INTRIN)
#  include <intrin.h>
#  include <immintrin.h>
#elif defined(JS220_I128_USE_UMULH)
#  include <intrin.h>
#endif

static const uint64_t U64_SIGN_BIT = 0x8000000000000000LLU;

// ---------------------------------------------------------------------------
// Portable helpers (only compiled when JS220_I128_IMPL_PORTABLE).
// ---------------------------------------------------------------------------

#if defined(JS220_I128_IMPL_PORTABLE)

// 64x64 -> 128 unsigned multiply. Returns the low 64 bits, stores high in *hi.
static uint64_t js220_i128_umul128_(uint64_t a, uint64_t b, uint64_t * hi) {
#if defined(JS220_I128_USE_UMULH)
    *hi = __umulh(a, b);
    return a * b;
#else
    uint64_t a_lo = (uint32_t) a;
    uint64_t a_hi = a >> 32;
    uint64_t b_lo = (uint32_t) b;
    uint64_t b_hi = b >> 32;

    uint64_t p_ll = a_lo * b_lo;
    uint64_t p_lh = a_lo * b_hi;
    uint64_t p_hl = a_hi * b_lo;
    uint64_t p_hh = a_hi * b_hi;

    uint64_t mid = (p_ll >> 32) + (p_lh & 0xFFFFFFFFULL) + (p_hl & 0xFFFFFFFFULL);
    *hi = p_hh + (p_lh >> 32) + (p_hl >> 32) + (mid >> 32);
    return (p_ll & 0xFFFFFFFFULL) | (mid << 32);
#endif
}

// 128/64 -> 64 unsigned divide using restoring long division.
// Precondition: hi < divisor (otherwise the quotient overflows 64 bits).
static uint64_t js220_i128_udiv128_(uint64_t hi, uint64_t lo, uint64_t divisor, uint64_t * remainder) {
    uint64_t quot = 0;
    uint64_t rem = hi;
    for (int i = 0; i < 64; ++i) {
        uint64_t rem_top = rem >> 63;
        rem = (rem << 1) | (lo >> 63);
        lo <<= 1;
        quot <<= 1;
        if (rem_top || rem >= divisor) {
            rem -= divisor;
            quot |= 1;
        }
    }
    *remainder = rem;
    return quot;
}

#endif  // JS220_I128_IMPL_PORTABLE

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

js220_i128 js220_i128_init_i64(int64_t a) {
    js220_i128 r;
    r.i64[0] = a;
    r.i64[1] = (a >= 0) ? 0 : -1;
    return r;
}

js220_i128 js220_i128_add(js220_i128 a, js220_i128 b) {
#if defined(JS220_I128_IMPL_INT128)
    a.i128 = a.i128 + b.i128;
    return a;
#elif defined(JS220_I128_IMPL_X86_INTRIN)
    js220_i128 r;
    uint8_t c = _addcarry_u64(0, a.u64[0], b.u64[0], &r.u64[0]);
    _addcarry_u64(c, a.u64[1], b.u64[1], &r.u64[1]);
    return r;
#else  // JS220_I128_IMPL_PORTABLE
    js220_i128 r;
    r.u64[0] = a.u64[0] + b.u64[0];
    uint64_t carry = (r.u64[0] < a.u64[0]) ? 1 : 0;
    r.u64[1] = a.u64[1] + b.u64[1] + carry;
    return r;
#endif
}

js220_i128 js220_i128_sub(js220_i128 a, js220_i128 b) {
#if defined(JS220_I128_IMPL_INT128)
    a.i128 = a.i128 - b.i128;
    return a;
#elif defined(JS220_I128_IMPL_X86_INTRIN)
    js220_i128 r;
    uint8_t c = _subborrow_u64(0, a.u64[0], b.u64[0], &r.u64[0]);
    _subborrow_u64(c, a.u64[1], b.u64[1], &r.u64[1]);
    return r;
#else  // JS220_I128_IMPL_PORTABLE
    js220_i128 r;
    r.u64[0] = a.u64[0] - b.u64[0];
    uint64_t borrow = (a.u64[0] < b.u64[0]) ? 1 : 0;
    r.u64[1] = a.u64[1] - b.u64[1] - borrow;
    return r;
#endif
}

js220_i128 js220_i128_square_i64(int64_t a) {
#if defined(JS220_I128_IMPL_INT128)
    js220_i128 r;
    r.i128 = a;
    r.i128 = r.i128 * r.i128;
    return r;
#elif defined(JS220_I128_IMPL_X86_INTRIN)
    js220_i128 r;
    if (a < 0) {
        a = -a;
    }
    r.u64[0] = _umul128(a, a, &r.u64[1]);
    return r;
#else  // JS220_I128_IMPL_PORTABLE
    js220_i128 r;
    if (a < 0) {
        a = -a;
    }
    uint64_t ua = (uint64_t) a;
    r.u64[0] = js220_i128_umul128_(ua, ua, &r.u64[1]);
    return r;
#endif
}

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
#if defined(JS220_I128_IMPL_INT128)
    result.i128 = dividend.i128 / divisor;
    *remainder = dividend.i128 - (result.i128 * divisor);
#elif defined(JS220_I128_IMPL_X86_INTRIN)
    result.u64[1] = dividend.u64[1] / divisor;
    dividend.u64[1] -= result.u64[1] * divisor;
    result.u64[0] = _udiv128(dividend.u64[1], dividend.u64[0], divisor, remainder);
#else  // JS220_I128_IMPL_PORTABLE
    result.u64[1] = dividend.u64[1] / divisor;
    dividend.u64[1] -= result.u64[1] * divisor;
    result.u64[0] = js220_i128_udiv128_(dividend.u64[1], dividend.u64[0], divisor, remainder);
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

bool js220_i128_is_neg(js220_i128 x) {
    return x.i64[1] < 0;
}

double js220_i128_compute_std(int64_t x1, js220_i128 x2, uint64_t n, uint32_t q) {
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

js220_i128 js220_i128_compute_integral(js220_i128 x, uint64_t n) {
    if (x.i64[1] < 0) {
        x = js220_i128_neg(x);
        x = js220_i128_udiv(x, n, NULL);
        x = js220_i128_neg(x);
    } else {
        x = js220_i128_udiv(x, n, NULL);
    };
    return x;
}
