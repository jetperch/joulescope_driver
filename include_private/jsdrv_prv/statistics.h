/*
 * Copyright 2020-2022 Jetperch LLC
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

#ifndef JSDRV_STATISTICS_H
#define JSDRV_STATISTICS_H

#include "jsdrv/cmacro_inc.h"
#include <stdint.h>

/**
 * @ingroup jsdrv
 * @defgroup jsdrv_statistics Statistics
 *
 * @brief JSDRV statistics.
 *
 * @{
 */

JSDRV_CPP_GUARD_START

/**
 * @brief The statistics instance for a single variable.
 *
 * This structure and associated "methods" compute mean, sample variance,
 * minimum and maximum over samples.  The statistics are computed in a single
 * pass and are available at any time with minimal additional computation.
 *
 * @see https://en.wikipedia.org/wiki/Variance
 * @see https://en.wikipedia.org/wiki/Unbiased_estimation_of_standard_deviation
 * @see https://www.johndcook.com/blog/standard_deviation/
 * @see https://en.wikipedia.org/wiki/Algorithms_for_calculating_variance
 */
struct jsdrv_statistics_accum_s {
    uint64_t k;    ///< Number of samples.
    double mean;   ///< mean (average value).
    double s;      ///< Scaled running variance.
    double min;    ///< Minimum value.
    double max;    ///< Maximum value.
};

/**
 * @brief Reset the statistics to 0 samples.
 *
 * @param s The statistics instance.
 */
void jsdrv_statistics_reset(struct jsdrv_statistics_accum_s * s);

/**
 * @brief Keep the same statistics, but adjust the number of samples.
 *
 * @param s The statistics instance.
 * @param k The new number of samples.
 */
void jsdrv_statistics_adjust_k(struct jsdrv_statistics_accum_s * s, uint64_t k);

/**
 * @brief Compute the statistics over an array.
 *
 * @param s The statistics instance.
 * @param x The value array.
 * @param length The number of elements in x.
 *
 * Use the "traditional" two pass method.  Compute mean in first pass,
 * then variance in second pass.
 */
void jsdrv_statistics_compute_f32(struct jsdrv_statistics_accum_s * s, const float * x, uint64_t length);

/**
 * @brief Compute the statistics over an array.
 *
 * @param s The statistics instance.
 * @param x The value array.
 * @param length The number of elements in x.
 */
void jsdrv_statistics_compute_f64(struct jsdrv_statistics_accum_s * s, const double * x, uint64_t length);

/**
 * @brief Add a new sample into the statistics.
 *
 * @param s The statistics instance.
 * @param x The new value.
 */
void jsdrv_statistics_add(struct jsdrv_statistics_accum_s * s, double x);

/**
 * @brief Get the sample variance.
 *
 * @param s The statistics instance.
 * @return The sample variance
 *
 * Sample variance uses k-1 denominator, also called the Bessel correction,
 * which is what you want for estimating variance from samples.
 * "Standard" population variance uses k as the denominator which tends to
 * underestimate true variance.
 */
double jsdrv_statistics_var(struct jsdrv_statistics_accum_s * s);

/**
 * @brief Copy one statistics instance to another.
 *
 * @param tgt The target statistics instance.
 * @param src The source statistics instance.
 */
void jsdrv_statistics_copy(struct jsdrv_statistics_accum_s * tgt, struct jsdrv_statistics_accum_s const * src);

/**
 * @brief Compute the combined statistics over two statistics instances.
 *
 * @param tgt The target statistics instance.  It is safe to use a or b for tgt.
 * @param a The first statistics instance to combine.
 * @param b The first statistics instance to combine.
 */
void jsdrv_statistics_combine(struct jsdrv_statistics_accum_s * tgt,
                             struct jsdrv_statistics_accum_s const * a,
                             struct jsdrv_statistics_accum_s const * b);

struct jsdrv_summary_entry_s;
void jsdrv_statistics_from_entry(struct jsdrv_statistics_accum_s * s, struct jsdrv_summary_entry_s const * e, uint64_t k);
void jsdrv_statistics_to_entry(struct jsdrv_statistics_accum_s const * s, struct jsdrv_summary_entry_s * e);

JSDRV_CPP_GUARD_END

/** @} */

#endif  // JSDRV_STATISTICS_H
