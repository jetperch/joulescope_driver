/*
 * Copyright 2025 Jetperch LLC
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
#include <string.h>
#include <stdlib.h>

#define JSDRV_LOG_LEVEL JSDRV_LOG_LEVEL_ALL
#include "jsdrv_prv/js320_fwup_mgr.h"
#include "jsdrv/error_code.h"
#include "jsdrv_prv/cdef.h"
#include "miniz.h"


// =====================================================================
// Helpers: create test ZIP archives in memory
// =====================================================================

struct test_zip_s {
    uint8_t * data;
    uint32_t  size;
};

static void test_zip_create(struct test_zip_s * tz, const char ** filenames,
                            const uint8_t ** contents, const uint32_t * sizes,
                            uint32_t count) {
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    assert_true(mz_zip_writer_init_heap(&zip, 0, 0));
    for (uint32_t i = 0; i < count; ++i) {
        assert_true(mz_zip_writer_add_mem(&zip, filenames[i],
            contents[i], sizes[i], MZ_DEFAULT_COMPRESSION));
    }
    void * buf = NULL;
    size_t sz = 0;
    assert_true(mz_zip_writer_finalize_heap_archive(&zip, &buf, &sz));
    tz->data = (uint8_t *) buf;
    tz->size = (uint32_t) sz;
    assert_non_null(tz->data);
    mz_zip_writer_end(&zip);
}

static void test_zip_free(struct test_zip_s * tz) {
    if (tz->data) {
        free(tz->data);
        tz->data = NULL;
    }
}

// Build a ZIP containing the full set of update files.
static void test_zip_create_full(struct test_zip_s * tz) {
    const char * names[] = {
        "0/0/app/image.mbfw",
        "1/image.bit",
        "0/0/app/pubsub_metadata.bin",
        "0/0/app/mbgen.bin",
        "1/0/app/pubsub_metadata.bin",
        "1/0/app/mbgen.bin",
        "manifest.yaml",
    };
    const uint8_t ctrl_fw[] = {0xC0, 0xFF, 0xEE, 0x01};
    const uint8_t fpga_bit[] = {0xF0, 0x0D, 0xCA, 0xFE};
    const uint8_t meta0[] = {0xAA};
    const uint8_t trace0[] = {0xBB};
    const uint8_t meta1[] = {0xCC};
    const uint8_t trace1[] = {0xDD};
    const uint8_t manifest[] = "schema: {version: '2.0.0'}";
    const uint8_t * contents[] = {
        ctrl_fw, fpga_bit, meta0, trace0, meta1, trace1, manifest
    };
    const uint32_t sizes[] = {
        sizeof(ctrl_fw), sizeof(fpga_bit), sizeof(meta0), sizeof(trace0),
        sizeof(meta1), sizeof(trace1), sizeof(manifest) - 1
    };
    test_zip_create(tz, names, contents, sizes, 7);
}


// =====================================================================
// Tests: resource table
// =====================================================================

static void test_resource_table_entries(void ** state) {
    (void) state;
    const struct jsdrv_fwup_resource_s * r = JSDRV_FWUP_RESOURCES;

    assert_string_equal(r[0].zip_path, "0/0/app/pubsub_metadata.bin");
    assert_string_equal(r[0].mem_topic, "c/xspi/!cmd");
    assert_int_equal(r[0].offset, 0x080000);
    assert_int_equal(r[0].erase_size, 0x20000);

    assert_string_equal(r[1].zip_path, "0/0/app/mbgen.bin");
    assert_string_equal(r[1].mem_topic, "c/xspi/!cmd");
    assert_int_equal(r[1].offset, 0x0A0000);
    assert_int_equal(r[1].erase_size, 0x20000);

    assert_string_equal(r[2].zip_path, "1/0/app/pubsub_metadata.bin");
    assert_string_equal(r[2].mem_topic, "s/flash/!cmd");
    assert_int_equal(r[2].offset, 0x140000);
    assert_int_equal(r[2].erase_size, 0x20000);

    assert_string_equal(r[3].zip_path, "1/0/app/mbgen.bin");
    assert_string_equal(r[3].mem_topic, "s/flash/!cmd");
    assert_int_equal(r[3].offset, 0x160000);
    assert_int_equal(r[3].erase_size, 0x20000);

    // sentinel
    assert_null(r[4].zip_path);
    assert_null(r[4].mem_topic);
}

static void test_resource_table_count(void ** state) {
    (void) state;
    uint32_t count = 0;
    while (JSDRV_FWUP_RESOURCES[count].zip_path) {
        ++count;
    }
    assert_int_equal(4, count);
}


// =====================================================================
// Tests: ZIP extraction
// =====================================================================

static void test_zip_extract_success(void ** state) {
    (void) state;
    const char * names[] = {"hello.txt", "data.bin"};
    const uint8_t hello[] = "Hello, world!";
    const uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    const uint8_t * contents[] = {hello, data};
    const uint32_t sizes[] = {sizeof(hello) - 1, sizeof(data)};
    struct test_zip_s tz = {0};
    test_zip_create(&tz, names, contents, sizes, 2);

    uint8_t * out = NULL;
    uint32_t out_size = 0;
    assert_int_equal(0, jsdrv_fwup_zip_extract(
        tz.data, tz.size, "hello.txt", &out, &out_size));
    assert_int_equal(sizeof(hello) - 1, out_size);
    assert_memory_equal(hello, out, out_size);
    free(out);

    out = NULL;
    out_size = 0;
    assert_int_equal(0, jsdrv_fwup_zip_extract(
        tz.data, tz.size, "data.bin", &out, &out_size));
    assert_int_equal(sizeof(data), out_size);
    assert_memory_equal(data, out, out_size);
    free(out);

    test_zip_free(&tz);
}

static void test_zip_extract_not_found(void ** state) {
    (void) state;
    const char * names[] = {"hello.txt"};
    const uint8_t hello[] = "Hello";
    const uint8_t * contents[] = {hello};
    const uint32_t sizes[] = {5};
    struct test_zip_s tz = {0};
    test_zip_create(&tz, names, contents, sizes, 1);

    uint8_t * out = NULL;
    uint32_t out_size = 0;
    assert_int_equal(JSDRV_ERROR_NOT_FOUND, jsdrv_fwup_zip_extract(
        tz.data, tz.size, "missing.txt", &out, &out_size));
    assert_null(out);
    assert_int_equal(0, out_size);

    test_zip_free(&tz);
}

static void test_zip_extract_invalid_zip(void ** state) {
    (void) state;
    uint8_t garbage[] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t * out = NULL;
    uint32_t out_size = 0;
    assert_int_equal(JSDRV_ERROR_IO, jsdrv_fwup_zip_extract(
        garbage, sizeof(garbage), "hello.txt", &out, &out_size));
    assert_null(out);
}

static void test_zip_extract_null_params(void ** state) {
    (void) state;
    uint8_t buf[1] = {0};
    uint8_t * out = NULL;
    uint32_t out_size = 0;
    assert_int_equal(JSDRV_ERROR_PARAMETER_INVALID,
        jsdrv_fwup_zip_extract(NULL, 10, "f", &out, &out_size));
    assert_int_equal(JSDRV_ERROR_PARAMETER_INVALID,
        jsdrv_fwup_zip_extract(buf, 0, "f", &out, &out_size));
    assert_int_equal(JSDRV_ERROR_PARAMETER_INVALID,
        jsdrv_fwup_zip_extract(buf, 1, NULL, &out, &out_size));
    assert_int_equal(JSDRV_ERROR_PARAMETER_INVALID,
        jsdrv_fwup_zip_extract(buf, 1, "f", NULL, &out_size));
    assert_int_equal(JSDRV_ERROR_PARAMETER_INVALID,
        jsdrv_fwup_zip_extract(buf, 1, "f", &out, NULL));
}

static void test_zip_extract_nested_path(void ** state) {
    (void) state;
    const char * names[] = {"0/0/app/pubsub_metadata.bin"};
    const uint8_t payload[] = {0xAA, 0xBB, 0xCC};
    const uint8_t * contents[] = {payload};
    const uint32_t sizes[] = {sizeof(payload)};
    struct test_zip_s tz = {0};
    test_zip_create(&tz, names, contents, sizes, 1);

    uint8_t * out = NULL;
    uint32_t out_size = 0;
    assert_int_equal(0, jsdrv_fwup_zip_extract(
        tz.data, tz.size, "0/0/app/pubsub_metadata.bin", &out, &out_size));
    assert_int_equal(sizeof(payload), out_size);
    assert_memory_equal(payload, out, out_size);
    free(out);

    test_zip_free(&tz);
}

static void test_zip_extract_empty_file(void ** state) {
    (void) state;
    const char * names[] = {"empty.bin"};
    const uint8_t dummy = 0;
    const uint8_t * contents[] = {&dummy};
    const uint32_t sizes[] = {0};
    struct test_zip_s tz = {0};
    test_zip_create(&tz, names, contents, sizes, 1);

    uint8_t * out = NULL;
    uint32_t out_size = 0;
    assert_int_equal(0, jsdrv_fwup_zip_extract(
        tz.data, tz.size, "empty.bin", &out, &out_size));
    assert_int_equal(0, out_size);
    // miniz returns non-NULL even for empty files
    free(out);
    test_zip_free(&tz);
}

static void test_zip_extract_all_update_files(void ** state) {
    (void) state;
    struct test_zip_s tz = {0};
    test_zip_create_full(&tz);

    // Extract ctrl firmware
    uint8_t * out = NULL;
    uint32_t out_size = 0;
    assert_int_equal(0, jsdrv_fwup_zip_extract(
        tz.data, tz.size, "0/0/app/image.mbfw", &out, &out_size));
    assert_int_equal(4, out_size);
    assert_int_equal(0xC0, out[0]);
    free(out);

    // Extract FPGA bitstream
    assert_int_equal(0, jsdrv_fwup_zip_extract(
        tz.data, tz.size, "1/image.bit", &out, &out_size));
    assert_int_equal(4, out_size);
    assert_int_equal(0xF0, out[0]);
    free(out);

    // Extract each resource
    for (uint32_t i = 0; JSDRV_FWUP_RESOURCES[i].zip_path; ++i) {
        assert_int_equal(0, jsdrv_fwup_zip_extract(
            tz.data, tz.size, JSDRV_FWUP_RESOURCES[i].zip_path,
            &out, &out_size));
        assert_true(out_size > 0);
        free(out);
    }

    test_zip_free(&tz);
}


// =====================================================================
// Tests: add command header
// =====================================================================

static void test_add_header_size(void ** state) {
    (void) state;
    // Compile-time assert is in js320_fwup_mgr.c; runtime check here
    assert_int_equal(40, sizeof(struct jsdrv_fwup_add_header_s));
}

static void test_add_header_fields(void ** state) {
    (void) state;
    struct jsdrv_fwup_add_header_s hdr;
    memset(&hdr, 0, sizeof(hdr));
    strcpy(hdr.device_prefix, "u/js320/8w2a");
    hdr.flags = JSDRV_FWUP_FLAG_SKIP_CTRL | JSDRV_FWUP_FLAG_SKIP_FPGA;
    hdr.zip_size = 999999;

    assert_string_equal(hdr.device_prefix, "u/js320/8w2a");
    assert_int_equal(hdr.flags, 3);
    assert_int_equal(hdr.zip_size, 999999);
}

static void test_add_header_prefix_max_length(void ** state) {
    (void) state;
    struct jsdrv_fwup_add_header_s hdr;
    memset(&hdr, 0, sizeof(hdr));
    // Fill prefix to max (31 chars + null)
    memset(hdr.device_prefix, 'x', 31);
    hdr.device_prefix[31] = '\0';
    assert_int_equal(31, strlen(hdr.device_prefix));
}

static void test_add_header_payload_layout(void ** state) {
    (void) state;
    // Verify header + zip can be read back from a contiguous buffer
    uint8_t zip_stub[] = {0x50, 0x4B, 0x03, 0x04};  // PK magic
    uint32_t hdr_size = sizeof(struct jsdrv_fwup_add_header_s);
    uint32_t total = hdr_size + sizeof(zip_stub);
    uint8_t * buf = (uint8_t *) calloc(1, total);

    struct jsdrv_fwup_add_header_s * hdr = (struct jsdrv_fwup_add_header_s *) buf;
    strcpy(hdr->device_prefix, "u/js320/test");
    hdr->flags = JSDRV_FWUP_FLAG_SKIP_RESOURCES;
    hdr->zip_size = sizeof(zip_stub);
    memcpy(buf + hdr_size, zip_stub, sizeof(zip_stub));

    // Parse it back
    const struct jsdrv_fwup_add_header_s * parsed =
        (const struct jsdrv_fwup_add_header_s *) buf;
    assert_string_equal(parsed->device_prefix, "u/js320/test");
    assert_int_equal(parsed->flags, JSDRV_FWUP_FLAG_SKIP_RESOURCES);
    assert_int_equal(parsed->zip_size, 4);

    const uint8_t * zip_data = buf + hdr_size;
    assert_memory_equal(zip_data, zip_stub, sizeof(zip_stub));

    free(buf);
}


// =====================================================================
// Tests: flags enum values
// =====================================================================

static void test_flags_values(void ** state) {
    (void) state;
    assert_int_equal(1, JSDRV_FWUP_FLAG_SKIP_CTRL);
    assert_int_equal(2, JSDRV_FWUP_FLAG_SKIP_FPGA);
    assert_int_equal(4, JSDRV_FWUP_FLAG_SKIP_RESOURCES);

    // Flags are independent bits
    uint32_t all = JSDRV_FWUP_FLAG_SKIP_CTRL
                 | JSDRV_FWUP_FLAG_SKIP_FPGA
                 | JSDRV_FWUP_FLAG_SKIP_RESOURCES;
    assert_int_equal(7, all);
}


// =====================================================================
// Tests: topic defines
// =====================================================================

static void test_topic_defines(void ** state) {
    (void) state;
    assert_string_equal("fwup/@/!add", JSDRV_FWUP_MGR_TOPIC_ADD);
    assert_string_equal("fwup/@/list", JSDRV_FWUP_MGR_TOPIC_LIST);
    assert_true(JSDRV_FWUP_INSTANCE_MAX >= 1);
    assert_true(JSDRV_FWUP_INSTANCE_MAX <= 16);
}


// =====================================================================
// main
// =====================================================================

int main(void) {
    const struct CMUnitTest tests[] = {
        // Resource table
        cmocka_unit_test(test_resource_table_entries),
        cmocka_unit_test(test_resource_table_count),

        // ZIP extraction
        cmocka_unit_test(test_zip_extract_success),
        cmocka_unit_test(test_zip_extract_not_found),
        cmocka_unit_test(test_zip_extract_invalid_zip),
        cmocka_unit_test(test_zip_extract_null_params),
        cmocka_unit_test(test_zip_extract_nested_path),
        cmocka_unit_test(test_zip_extract_empty_file),
        cmocka_unit_test(test_zip_extract_all_update_files),

        // Add header
        cmocka_unit_test(test_add_header_size),
        cmocka_unit_test(test_add_header_fields),
        cmocka_unit_test(test_add_header_prefix_max_length),
        cmocka_unit_test(test_add_header_payload_layout),

        // Flags and topics
        cmocka_unit_test(test_flags_values),
        cmocka_unit_test(test_topic_defines),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
