/*
 * Copyright 2014-2022 Jetperch LLC
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

/**
 * @file
 *
 * @brief JSDRV time representation.
 */

#ifndef JSDRV_TIME_H__
#define JSDRV_TIME_H__

#include "jsdrv/cmacro_inc.h"
#include <stdint.h>
#include <stddef.h>

/**
 * @ingroup jsdrv
 * @defgroup jsdrv_time Time representation
 *
 * @brief JSDRV time representation.
 *
 * The C standard library includes time.h which is very inconvenient for
 * embedded systems.  This module defines a much simpler 64-bit fixed point
 * integer for representing time.  The value is 34Q30 with the upper 34 bits
 * to represent whole seconds and the lower 30 bits to represent fractional
 * seconds.  A value of 2**30 (1 << 30) represents 1 second.  This
 * representation gives a resolution of 2 ** -30 (approximately 1 nanosecond)
 * and a range of +/- 2 ** 33 (approximately 272 years).  The value is
 * signed to allow for simple arithmetic on the time either as a fixed value
 * or as deltas.
 *
 * Certain elements may elect to use floating point time given in seconds.
 * The macros JSDRV_TIME_TO_F64() and JSDRV_F64_TO_TIME() facilitate
 * converting between the domains.  Note that double precision floating
 * point is not able to maintain the same resolution over the time range
 * as the 64-bit representation.  JSDRV_TIME_TO_F32() and JSDRV_F32_TO_TIME()
 * allow conversion to single precision floating point which has significantly
 * reduce resolution compared to the 34Q30 value.
 *
 * Float64 only has 53 bits of precision, which can only represent up to
 * 104 days with nanosecond precision.  While this duration and precision is
 * often adequate for relative time, it is insufficient to store absolute time
 * from the epoch.  In contrast, the int64 34Q30 time with nanosecond resolution
 * can store ±272 years relative to its epoch.
 *
 * For applications that need floating-point time, store a
 * separate offset in seconds relative to the starting time.
 * This improves precision between intervals.  For example:
 *
 *      x = np.linspace(0, time_end - time_start, length, dtype=np.float64)
 *
 * @{
 */

JSDRV_CPP_GUARD_START

/**
 * @brief The number of fractional bits in the 64-bit time representation.
 */
#define JSDRV_TIME_Q 30

/**
 * @brief The maximum (positive) time representation
 */
#define JSDRV_TIME_MAX ((int64_t) 0x7fffffffffffffffU)

/**
 * @brief The minimum (negative) time representation.
 */
#define JSDRV_TIME_MIN ((int64_t) 0x8000000000000000U)

/**
 * @brief The offset from the standard UNIX (POSIX) epoch.
 *
 * This offset allows translation between JSDRV time and the
 * standard UNIX (POSIX) epoch of Jan 1, 1970.
 *
 * The value was computed using python3:
 *
 *     import dateutil.parser
 *     dateutil.parser.parse('2018-01-01T00:00:00Z').timestamp()
 *
 * JSDRV chooses a different epoch to advance "zero" by 48 years!
 */
#define JSDRV_TIME_EPOCH_UNIX_OFFSET_SECONDS 1514764800

/**
 * @brief The fixed-point representation for 1 second.
 */
#define JSDRV_TIME_SECOND (((int64_t) 1) << JSDRV_TIME_Q)

/// The mask for the fractional bits
#define JSDRV_FRACT_MASK (JSDRV_TIME_SECOND - 1)

/**
 * @brief The approximate fixed-point representation for 1 millisecond.
 */
#define JSDRV_TIME_MILLISECOND ((JSDRV_TIME_SECOND + 500) / 1000)

/**
 * @brief The approximate fixed-point representation for 1 microsecond.
 *
 * CAUTION: this value is 0.024% accurate (240 ppm)
 */
#define JSDRV_TIME_MICROSECOND ((JSDRV_TIME_SECOND + 500000) / 1000000)

/**
 * @brief The approximate fixed-point representation for 1 nanosecond.
 *
 * WARNING: this value is only 6.7% accurate!
 */
#define JSDRV_TIME_NANOSECOND ((int64_t) 1)

/**
 * @brief The fixed-point representation for 1 minute.
 */
#define JSDRV_TIME_MINUTE (JSDRV_TIME_SECOND * 60)

/**
 * @brief The fixed-point representation for 1 hour.
 */
#define JSDRV_TIME_HOUR (JSDRV_TIME_MINUTE * 60)

/**
 * @brief The fixed-point representation for 1 day.
 */
#define JSDRV_TIME_DAY (JSDRV_TIME_HOUR * 24)

/**
 * @brief The fixed-point representation for 1 week.
 */
#define JSDRV_TIME_WEEK (JSDRV_TIME_DAY * 7)

/**
 * @brief The average fixed-point representation for 1 month (365 day year).
 */
#define JSDRV_TIME_MONTH (JSDRV_TIME_YEAR / 12)

/**
 * @brief The approximate fixed-point representation for 1 year (365 days).
 */
#define JSDRV_TIME_YEAR (JSDRV_TIME_DAY * 365)

/**
 * @brief Convert the 64-bit fixed point time to a double.
 *
 * @param x The 64-bit signed fixed point time.
 * @return The time as a double p.  Note that IEEE 747 doubles only have
 *      52 bits of precision, so the result will be truncated for very
 *      small deltas.
 */
#define JSDRV_TIME_TO_F64(x) (((double) (x)) * (1.0 / ((double) JSDRV_TIME_SECOND)))

/**
 * @brief Convert the double precision time to 64-bit fixed point time.
 *
 * @param x The double-precision floating point time in seconds.
 * @return The time as a 34Q30.
 */
JSDRV_INLINE_FN int64_t JSDRV_F64_TO_TIME(double x) {
    int negate = 0;
    if (x < 0) {
        negate = 1;
        x = -x;
    }
    int64_t c = (int64_t) ((x * (double) JSDRV_TIME_SECOND) + 0.5);
    return negate ? -c : c;
}

/**
 * @brief Convert the 64-bit fixed point time to single precision float.
 *
 * @param x The 64-bit signed fixed point time.
 * @return The time as a float p in seconds.  Note that IEEE 747 singles only
 *      have 23 bits of precision, so the result will likely be truncated.
 */
#define JSDRV_TIME_TO_F32(x) (((float) (x)) * (1.0f / ((float) JSDRV_TIME_SECOND)))

/**
 * @brief Convert the single precision float time to 64-bit fixed point time.
 *
 * @param x The single-precision floating point time in seconds.
 * @return The time as a 34Q30.
 */
JSDRV_INLINE_FN int64_t JSDRV_F32_TO_TIME(float x) {
    int negate = 0;
    if (x < 0) {
        negate = 1;
        x = -x;
    }
    int64_t c = (int64_t) ((x * (float) JSDRV_TIME_SECOND) + 0.5f);
    return negate ? -c : c;
}

/**
 * @brief Convert to counter ticks, rounded to nearest.
 *
 * @param x The 64-bit signed fixed point time.
 * @param z The counter frequency in Hz.
 * @return The 64-bit time in counter ticks.
 */
JSDRV_INLINE_FN int64_t JSDRV_TIME_TO_COUNTER(int64_t x, uint64_t z) {
    uint8_t negate = 0;
    if (x < 0) {
        x = -x;
        negate = 1;
    }
    // return (int64_t) ((((x * z) >> (JSDRV_TIME_Q - 1)) + 1) >> 1);
    uint64_t c = (((x & ~JSDRV_FRACT_MASK) >> (JSDRV_TIME_Q - 1)) * z);
    uint64_t fract = (x & JSDRV_FRACT_MASK) << 1;
    // round
    c += ((fract * z) >> JSDRV_TIME_Q) + 1;
    c >>= 1;
    if (negate) {
        c = -((int64_t) c);
    }
    return c;
}

/**
 * @brief Convert to counter ticks, rounded towards zero
 *
 * @param x The 64-bit signed fixed point time.
 * @param z The counter frequency in Hz.
 * @return The 64-bit time in counter ticks.
 */
JSDRV_INLINE_FN int64_t JSDRV_TIME_TO_COUNTER_RZERO(int64_t x, uint64_t z) {
    int negate = 0;
    if (x < 0) {
        negate = 1;
        x = -x;
    }
    uint64_t c_u64 = (x >> JSDRV_TIME_Q) * z;
    c_u64 += ((x & JSDRV_FRACT_MASK) * z) >> JSDRV_TIME_Q;
    int64_t c = (int64_t) c_u64;
    return negate ? -c : c;
}

/**
 * @brief Convert to counter ticks, rounded towards infinity.
 *
 * @param x The 64-bit signed fixed point time.
 * @param z The counter frequency in Hz.
 * @return The 64-bit time in counter ticks.
 */
JSDRV_INLINE_FN int64_t JSDRV_TIME_TO_COUNTER_RINF(int64_t x, uint64_t z) {
    int negate = 0;
    if (x < 0) {
        negate = 1;
        x = -x;
    }
    x += JSDRV_TIME_SECOND - 1;
    uint64_t c_u64 = (x >> JSDRV_TIME_Q) * z;
    c_u64 += ((x & JSDRV_FRACT_MASK) * z) >> JSDRV_TIME_Q;
    int64_t c = (int64_t) c_u64;
    return negate ? -c : c;
}

/**
 * @brief Convert to 32-bit unsigned seconds.
 *
 * @param x The 64-bit signed fixed point time.
 * @return The 64-bit unsigned time in seconds, rounded to nearest.
 */
#define JSDRV_TIME_TO_SECONDS(x) JSDRV_TIME_TO_COUNTER(x, 1)

/**
 * @brief Convert to milliseconds.
 *
 * @param x The 64-bit signed fixed point time.
 * @return The 64-bit signed time in milliseconds, rounded to nearest.
 */
#define JSDRV_TIME_TO_MILLISECONDS(x) JSDRV_TIME_TO_COUNTER(x, 1000)

/**
 * @brief Convert to microseconds.
 *
 * @param x The 64-bit signed fixed point time.
 * @return The 64-bit signed time in microseconds, rounded to nearest.
 */
#define JSDRV_TIME_TO_MICROSECONDS(x) JSDRV_TIME_TO_COUNTER(x, 1000000)

/**
 * @brief Convert to nanoseconds.
 *
 * @param x The 64-bit signed fixed point time.
 * @return The 64-bit signed time in nanoseconds, rounded to nearest.
 */
#define JSDRV_TIME_TO_NANOSECONDS(x) JSDRV_TIME_TO_COUNTER(x, 1000000000ll)

/**
 * @brief Convert a counter to 64-bit signed fixed point time.
 *
 * @param x The counter value in ticks.
 * @param z The counter frequency in Hz.
 * @return The 64-bit signed fixed point time.
 */
JSDRV_INLINE_FN int64_t JSDRV_COUNTER_TO_TIME(uint64_t x, uint64_t z) {
    // compute (x << JSDRV_TIME_Q) / z, but without unnecessary saturation
    uint64_t seconds = x / z;
    uint64_t remainder = x - (seconds * z);
    uint64_t fract = (remainder << JSDRV_TIME_Q) / z;
    uint64_t t = (int64_t) ((seconds << JSDRV_TIME_Q) + fract);
    return t;
}

/**
 * @brief Convert to 64-bit signed fixed point time.
 *
 * @param x he 32-bit unsigned time in seconds.
 * @return The 64-bit signed fixed point time.
 */
#define JSDRV_SECONDS_TO_TIME(x) (((int64_t) (x)) << JSDRV_TIME_Q)

/**
 * @brief Convert to 64-bit signed fixed point time.
 *
 * @param x The 32-bit unsigned time in milliseconds.
 * @return The 64-bit signed fixed point time.
 */
#define JSDRV_MILLISECONDS_TO_TIME(x) JSDRV_COUNTER_TO_TIME(x, 1000)

/**
 * @brief Convert to 64-bit signed fixed point time.
 *
 * @param x The 32-bit unsigned time in microseconds.
 * @return The 64-bit signed fixed point time.
 */
#define JSDRV_MICROSECONDS_TO_TIME(x) JSDRV_COUNTER_TO_TIME(x, 1000000)

/**
 * @brief Convert to 64-bit signed fixed point time.
 *
 * @param x The 32-bit unsigned time in microseconds.
 * @return The 64-bit signed fixed point time.
 */
#define JSDRV_NANOSECONDS_TO_TIME(x) JSDRV_COUNTER_TO_TIME(x, 1000000000ll)

/**
 * @brief Compute the absolute value of a time.
 *
 * @param t The time.
 * @return The absolute value of t.
 */
JSDRV_INLINE_FN int64_t JSDRV_TIME_ABS(int64_t t) {
    return ( (t) < 0 ? -(t) : (t) );
}

/**
 * @brief Return the minimum time.
 *
 * @param a The first time value.
 * @param b The second time value.
 * @return The smaller value of a and b.
 */
JSDRV_INLINE_FN int64_t jsdrv_time_min(int64_t a, int64_t b) {
    return (a < b) ? a : b;
}

/**
 * @brief Return the maximum time.
 *
 * @param a The first time value.
 * @param b The second time value.
 * @return The larger value of a and b.
 */
JSDRV_INLINE_FN int64_t jsdrv_time_max(int64_t a, int64_t b) {
    return (a > b) ? a : b;
}

/**
 * @brief The length of the ISO 8601 string produced by jsdrv_time_to_str().
 */
#define JSDRV_TIME_STRING_LENGTH (27)

/**
 * @brief Converts jsdrv time to an ISO 8601 string.
 *
 * @param t The jsdrv time.
 * @param str The string buffer.
 * @param size The size of str in bytes, which should be at least
 *      JSDRV_TIME_STRING_LENGTH bytes to fit the full ISO 8601
 *      string with the null terminator.
 * @return The number of characters written to buf, not including the
 *      null terminator.  If this value is less than
 *      (JSDRV_TIME_STRING_LENGTH - 1), then the full string was truncated.
 * @see http://howardhinnant.github.io/date_algorithms.html
 * @see https://stackoverflow.com/questions/7960318/math-to-convert-seconds-since-1970-into-date-and-vice-versa
 */
JSDRV_API int32_t jsdrv_time_to_str(int64_t t, char * str, size_t size);

/**
 * @brief Define a mapping between JSDRV time and a counter.
 *
 * This mapping is often used to convert between an
 * increment sample identifier value and JSDRV time.
 *
 * The counter_rate is specified as a double which contains
 * a 52-bit mantissa with fractional accuracy of 2e-16.
 */
struct jsdrv_time_map_s {
    int64_t offset_time;        ///< The offset specified as JSDRV time i64.
    uint64_t offset_counter;    ///< The offset specified as counter values u64.
    double counter_rate;        ///< The counter increment rate (Hz).
};

/**
 * @brief Convert time from a counter value to JSDRV time.
 *
 * @param self The time mapping instance.
 * @param counter The counter value u64.
 * @return The JSDRV time i64.
 */
JSDRV_API int64_t jsdrv_time_from_counter(struct jsdrv_time_map_s * self, uint64_t counter);

/**
 * @brief Convert time from JSDRV time to a counter value.
 *
 * @param self The time mapping instance.
 * @param time The JSDRV time i64.
 * @return The counter value u64.
 */
JSDRV_API uint64_t jsdrv_time_to_counter(struct jsdrv_time_map_s * self, int64_t time64);

JSDRV_CPP_GUARD_END

/** @} */

#endif /* JSDRV_TIME_H__ */
