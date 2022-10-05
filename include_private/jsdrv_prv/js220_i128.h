/*
 * Copyright 2021-2022 Jetperch LLC
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
 * @brief JS220 i128 math.
 */

#include "jsdrv/cmacro_inc.h"
#include "js220_api.h"
#include <stdint.h>

#ifndef JSDRV_JS220_I128_MATH_H__
#define JSDRV_JS220_I128_MATH_H__

/**
 * @ingroup jsdrv_prv
 * @defgroup jsdrv_prv_js220_i128stats i128
 *
 * @brief Perform i128 math.
 *
 * @{
 */


JSDRV_CPP_GUARD_START

//js220_i128 js220_i128_add(js220_i128 a, js220_i128 b);
//js220_i128 js220_i128_sub(js220_i128 a, js220_i128 b);
js220_i128 js220_i128_neg(js220_i128 x);
js220_i128 js220_i128_udiv(js220_i128 dividend, uint64_t divisor, uint64_t * remainder);
js220_i128 js220_i128_lshift(js220_i128 x, int32_t shift);
js220_i128 js220_i128_rshift(js220_i128 x, int32_t shift);
double js220_i128_to_f64(js220_i128 x, uint32_t q);
double js220_i128_compute_std(int64_t x1, js220_i128 x2, uint32_t n, uint32_t q);


JSDRV_CPP_GUARD_END

#endif // JSDRV_JS220_I128_MATH_H__