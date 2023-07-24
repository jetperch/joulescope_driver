/*
 * Copyright 2023 Jetperch LLC
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

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "jsdrv_prv/buffer_signal.h"
#include "jsdrv/cstr.h"
#include "tinyprintf.h"

const char SRC_TOPIC[] = "src/topic/!data";


#define initialize()                                        \
    (void) state;                                           \
    struct bufsig_s b;                                      \
    memset(&b, 0, sizeof(b));                               \
    jsdrv_cstr_copy(b.topic, SRC_TOPIC, sizeof(b.topic));   \
    b.hdr.sample_id = 1000;                                 \
    b.hdr.field_id = JSDRV_FIELD_CURRENT;                   \
    b.hdr.index = 7;                                        \
    b.hdr.element_type = JSDRV_DATA_TYPE_FLOAT;             \
    b.hdr.element_size_bits = 32;                           \
    b.hdr.element_count = 0;                                \
    b.hdr.decimate_factor = 1;                              \
    b.hdr.sample_rate = 1000000;                            \
    b.time_map.counter_rate = (double) b.hdr.sample_rate;   \
    b.active = true;                                        \
    jsdrv_bufsig_alloc(&b, 1000000, 10, 10)



static void test_initialize_finalize(void **state) {
    initialize();
    struct jsdrv_buffer_info_s info;
    memset(&info, 0, sizeof(info));
    jsdrv_bufsig_info(&b, &info);
    assert_int_equal(1, info.version);
    assert_int_equal(JSDRV_FIELD_CURRENT, info.field_id);
    assert_int_equal(7, info.index);
    assert_int_equal(JSDRV_DATA_TYPE_FLOAT, info.element_type);
    assert_int_equal(32, info.element_size_bits);
    assert_string_equal(SRC_TOPIC, info.topic);
    assert_int_equal(JSDRV_TIME_SECOND, info.size_in_utc);
    assert_int_equal(1000000, info.size_in_samples);
    assert_int_equal(0, info.time_range_utc.length);
    assert_int_equal(0, info.time_range_samples.length);
    assert_int_equal(1000000, info.time_map.counter_rate);

    jsdrv_bufsig_free(&b);
}

static void insert_samples(struct bufsig_s * b, uint64_t sample_id_start, uint32_t length) {
    struct jsdrv_stream_signal_s s;
    memset(&s, 0, sizeof(s));
    s.sample_id = sample_id_start;
    s.field_id = JSDRV_FIELD_CURRENT;
    s.index = 7;
    s.element_type = JSDRV_DATA_TYPE_FLOAT;
    s.element_size_bits = 32;
    s.element_count = length;
    s.sample_rate = 1000000;
    s.decimate_factor = 1;
    s.time_map.offset_time = JSDRV_TIME_HOUR;
    s.time_map.counter_rate = s.sample_rate;
    s.time_map.offset_counter = 0;
    float * f32 = (float *) s.data;
    for (uint32_t i = 0; i < s.element_count; ++i) {
        f32[i] = (s.sample_id + i) / 1000000.0f;
    }
    jsdrv_bufsig_recv_data(b, &s);
}

static void check_samples(struct jsdrv_buffer_response_s * rsp, uint64_t sample_id_start, uint64_t length) {
    assert_int_equal(1, rsp->version);
    assert_int_equal(JSDRV_BUFFER_RESPONSE_SAMPLES, rsp->response_type);
    assert_int_equal(JSDRV_FIELD_CURRENT, rsp->info.field_id);
    assert_int_equal(7, rsp->info.index);
    assert_int_equal(JSDRV_DATA_TYPE_FLOAT, rsp->info.element_type);
    assert_int_equal(32, rsp->info.element_size_bits);
    assert_string_equal(SRC_TOPIC, rsp->info.topic);
    assert_int_equal(JSDRV_TIME_SECOND, rsp->info.size_in_utc);
    assert_int_equal(1000000, rsp->info.size_in_samples);
    assert_int_equal(length, rsp->info.time_range_samples.length);
    assert_int_equal(sample_id_start, rsp->info.time_range_samples.start);
    assert_int_equal(sample_id_start + length - 1, rsp->info.time_range_samples.end);
    float * data = (float *) rsp->data;
    for (uint32_t i = 0; i < rsp->info.time_range_samples.length; ++i) {
        assert_float_equal((rsp->info.time_range_samples.start + i) / 1000000.0f, data[i], 1e-12);
    }
    assert_int_equal(rsp->info.time_range_samples.length, rsp->info.time_range_utc.length);
}

static void insert_const_samples(struct bufsig_s * b, uint64_t sample_id_start, uint32_t length, float value) {
    struct jsdrv_stream_signal_s s;
    memset(&s, 0, sizeof(s));
    s.sample_id = sample_id_start;
    s.field_id = JSDRV_FIELD_CURRENT;
    s.index = 7;
    s.element_type = JSDRV_DATA_TYPE_FLOAT;
    s.element_size_bits = 32;
    s.element_count = length;
    s.sample_rate = 1000000;
    s.decimate_factor = 1;
    s.time_map.offset_time = JSDRV_TIME_HOUR;
    s.time_map.counter_rate = s.sample_rate;
    s.time_map.offset_counter = 0;
    float * f32 = (float *) s.data;
    for (uint32_t i = 0; i < s.element_count; ++i) {
        f32[i] = value;
    }
    jsdrv_bufsig_recv_data(b, &s);
}

static void test_samples_start_length(void **state) {
    initialize();
    insert_samples(&b, 1000, 1000);
    struct jsdrv_buffer_info_s info;
    jsdrv_bufsig_info(&b, &info);
    assert_int_equal(1000, info.time_range_utc.length);
    assert_int_equal(1000, info.time_range_samples.length);

    assert_int_equal(1000, info.time_range_samples.start);
    assert_int_equal(1999, info.time_range_samples.end);

    int64_t offset_time = jsdrv_time_from_counter(&b.time_map, 1000);
    assert_int_equal(offset_time, info.time_range_utc.start);
    assert_int_equal(offset_time + JSDRV_TIME_MILLISECOND - JSDRV_TIME_MICROSECOND, info.time_range_utc.end);

    struct jsdrv_buffer_request_s req;
    memset(&req, 0, sizeof(req));
    req.version = 1;
    req.time_type = JSDRV_TIME_SAMPLES;
    req.time.samples.start = 1000;
    req.time.samples.length = 1000;
    uint64_t rsp_u64[1 << 12];
    struct jsdrv_buffer_response_s * rsp = (struct jsdrv_buffer_response_s *) rsp_u64;
    jsdrv_bufsig_process_request(&b, &req, rsp);
    check_samples(rsp, 1000, 1000);

    jsdrv_bufsig_free(&b);
}

static void test_samples_start_end(void **state) {
    initialize();
    insert_samples(&b, 1000, 1000);
    struct jsdrv_buffer_request_s req;
    memset(&req, 0, sizeof(req));
    req.version = 1;
    req.time_type = JSDRV_TIME_SAMPLES;
    req.time.samples.start = 1000;
    req.time.samples.end = 1999;
    uint64_t rsp_u64[1 << 12];
    struct jsdrv_buffer_response_s * rsp = (struct jsdrv_buffer_response_s *) rsp_u64;
    jsdrv_bufsig_process_request(&b, &req, rsp);
    check_samples(rsp, 1000, 1000);

    jsdrv_bufsig_free(&b);
}

static void test_samples_all(void **state) {
    initialize();
    insert_samples(&b, 1000, 1000);
    struct jsdrv_buffer_request_s req;
    memset(&req, 0, sizeof(req));
    req.version = 1;
    req.time_type = JSDRV_TIME_SAMPLES;
    req.time.samples.start = 1000;
    req.time.samples.end = 1999;
    req.time.samples.length = 1000;
    uint64_t rsp_u64[1 << 12];
    struct jsdrv_buffer_response_s * rsp = (struct jsdrv_buffer_response_s *) rsp_u64;
    jsdrv_bufsig_process_request(&b, &req, rsp);
    check_samples(rsp, 1000, 1000);

    jsdrv_bufsig_free(&b);
}

static void test_samples_wrap(void **state) {
    initialize();
    uint32_t length = 873;
    for (uint64_t sample_id = 0; sample_id < 1100000; sample_id += length) {
        insert_samples(&b, sample_id, 873);
    }
    struct jsdrv_buffer_request_s req;
    memset(&req, 0, sizeof(req));
    req.version = 1;
    req.time_type = JSDRV_TIME_SAMPLES;
    req.time.samples.start = 999990;
    req.time.samples.length = 1000;
    uint64_t rsp_u64[1 << 12];
    struct jsdrv_buffer_response_s * rsp = (struct jsdrv_buffer_response_s *) rsp_u64;
    jsdrv_bufsig_process_request(&b, &req, rsp);
    check_samples(rsp, 999990, 1000);

    jsdrv_bufsig_free(&b);
}

static void test_summary_simple(void **state) {
    initialize();
    insert_samples(&b, 1000, 1000);
    struct jsdrv_buffer_request_s req;
    memset(&req, 0, sizeof(req));
    req.version = 1;
    req.time_type = JSDRV_TIME_SAMPLES;
    req.time.samples.start = 1000;
    req.time.samples.end = 1006;
    req.time.samples.length = 2;
    uint64_t rsp_u64[1 << 12];
    struct jsdrv_buffer_response_s * rsp = (struct jsdrv_buffer_response_s *) rsp_u64;
    jsdrv_bufsig_process_request(&b, &req, rsp);
    assert_int_equal(JSDRV_BUFFER_RESPONSE_SUMMARY, rsp->response_type);
    struct jsdrv_summary_entry_s * entries = (struct jsdrv_summary_entry_s *) rsp->data;
    assert_float_equal(1001 / 1000000.0, entries[0].avg, 1e-9);
    assert_float_equal(1004 / 1000000.0, entries[1].avg, 1e-9);
    jsdrv_bufsig_free(&b);
}

static void test_summary_level1(void **state) {
    initialize();
    insert_const_samples(&b, 0, 99, 1.0f);
    insert_const_samples(&b, 99, 101, 0.0f);
    struct jsdrv_buffer_request_s req;
    memset(&req, 0, sizeof(req));
    req.version = 1;
    req.time_type = JSDRV_TIME_SAMPLES;
    req.time.samples.start = 100;
    req.time.samples.end = 200;
    req.time.samples.length = 1;
    uint64_t rsp_u64[1 << 12];
    struct jsdrv_buffer_response_s * rsp = (struct jsdrv_buffer_response_s *) rsp_u64;
    jsdrv_bufsig_process_request(&b, &req, rsp);
    assert_int_equal(JSDRV_BUFFER_RESPONSE_SUMMARY, rsp->response_type);
    struct jsdrv_summary_entry_s * entries = (struct jsdrv_summary_entry_s *) rsp->data;
    assert_float_equal(0.0, entries[0].avg, 1e-9);
    jsdrv_bufsig_free(&b);
}

static void test_summary_nan_on_out_of_range(void **state) {
    initialize();
    insert_samples(&b, 1000, 1000);
    struct jsdrv_buffer_request_s req;
    memset(&req, 0, sizeof(req));
    req.version = 1;
    req.time_type = JSDRV_TIME_SAMPLES;
    req.time.samples.start = 997;
    req.time.samples.end = 1005;
    req.time.samples.length = 3;
    uint64_t rsp_u64[1 << 12];
    struct jsdrv_buffer_response_s * rsp = (struct jsdrv_buffer_response_s *) rsp_u64;
    jsdrv_bufsig_process_request(&b, &req, rsp);
    assert_int_equal(JSDRV_BUFFER_RESPONSE_SUMMARY, rsp->response_type);
    struct jsdrv_summary_entry_s * entries = (struct jsdrv_summary_entry_s *) rsp->data;
    assert_true(isnan(entries[0].avg));
    assert_false(isnan(entries[1].avg));
    assert_false(isnan(entries[2].avg));
    assert_float_equal(1001 / 1000000.0, entries[1].avg, 1e-9);
    assert_float_equal(1004 / 1000000.0, entries[2].avg, 1e-9);

    req.time.samples.start = 1994;
    req.time.samples.end = 2002;
    req.time.samples.length = 3;
    jsdrv_bufsig_process_request(&b, &req, rsp);
    assert_int_equal(JSDRV_BUFFER_RESPONSE_SUMMARY, rsp->response_type);
    assert_false(isnan(entries[0].avg));
    assert_false(isnan(entries[1].avg));
    assert_true(isnan(entries[2].avg));
    assert_float_equal(1995 / 1000000.0, entries[0].avg, 1e-9);
    assert_float_equal(1998 / 1000000.0, entries[1].avg, 1e-9);

    jsdrv_bufsig_free(&b);
}

static void test_summary_wrap(void **state) {
    initialize();
    uint32_t length = 873;
    for (uint64_t sample_id = 0; sample_id < 1100000; sample_id += length) {
        insert_samples(&b, sample_id, 873);
    }
    struct jsdrv_buffer_request_s req;
    memset(&req, 0, sizeof(req));
    req.version = 1;
    req.time_type = JSDRV_TIME_SAMPLES;
    req.time.samples.start = 999950;
    req.time.samples.end = 1000149;
    req.time.samples.length = 2;
    uint64_t rsp_u64[1 << 12];
    struct jsdrv_buffer_response_s * rsp = (struct jsdrv_buffer_response_s *) rsp_u64;
    jsdrv_bufsig_process_request(&b, &req, rsp);
    assert_int_equal(JSDRV_BUFFER_RESPONSE_SUMMARY, rsp->response_type);
    struct jsdrv_summary_entry_s * entries = (struct jsdrv_summary_entry_s *) rsp->data;
    assert_false(isnan(entries[0].avg));
    assert_false(isnan(entries[1].avg));
    assert_float_equal(999999.5 / 1000000.0, entries[0].avg, 1e-4);
    assert_float_equal(1000099.5 / 1000000.0, entries[1].avg, 1e-4);

    jsdrv_bufsig_free(&b);
}


int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_initialize_finalize),
            cmocka_unit_test(test_samples_start_length),
            cmocka_unit_test(test_samples_start_end),
            cmocka_unit_test(test_samples_all),
            cmocka_unit_test(test_samples_wrap),
            cmocka_unit_test(test_summary_simple),
            cmocka_unit_test(test_summary_level1),
            cmocka_unit_test(test_summary_nan_on_out_of_range),
            cmocka_unit_test(test_summary_wrap),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
