/*
 * Copyright 2026 Jetperch LLC
 *
 * Standalone unit test for boot_info decoders.  No jsdrv dependency.
 * Build (mingw):  gcc -std=c11 boot_info.c boot_info_test.c -o boot_info_test
 */

#include "boot_info.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond) do { \
    if (cond) { \
        printf("  PASS: %s\n", #cond); \
    } else { \
        printf("  FAIL: %s  (line %d)\n", #cond, __LINE__); \
        ++failures; \
    } \
} while (0)

static void put_u32(uint8_t * p, uint32_t v) {
    p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8);
    p[2] = (uint8_t) (v >> 16); p[3] = (uint8_t) (v >> 24);
}

static void put_u16(uint8_t * p, uint16_t v) {
    p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8);
}

static void put_u64(uint8_t * p, uint64_t v) {
    put_u32(p, (uint32_t) v);
    put_u32(p + 4, (uint32_t) (v >> 32));
}


static void test_boot(void) {
    printf("test_boot:\n");
    uint8_t buf[256];
    memset(buf, 0, sizeof(buf));
    // fault_registers_length=3, history_length=2
    put_u32(buf + 0, 0x55AACC33);            // magic
    put_u32(buf + 4, 0x01020003);            // bootloader_version 1.2.3
    put_u32(buf + 8, 0x00010000);            // image_version[0] 0.1.0
    put_u32(buf + 12, 0xFFFFFFFF);           // image_version[1] invalid
    put_u32(buf + 16, 0x00000000);           // image_version[2] unknown
    put_u32(buf + 20, 0x00000000);           // image_version[3] unknown
    put_u64(buf + 24, 0x1122334455667788ULL);// command_arg0
    // word8: command=0, command_result=0, stable=1
    put_u32(buf + 32, 0x00010000);
    // word9: fault_registers_length=3, history_length=2, reset_cause=0x83, fault_flags=0
    put_u32(buf + 36, 0x00830203);
    put_u32(buf + 40, 0xDEADBEEF);           // fault_location
    put_u32(buf + 44, 0x11111111);           // fault_registers[0]
    put_u32(buf + 48, 0x22222222);           // fault_registers[1]
    put_u32(buf + 52, 0x33333333);           // fault_registers[2]
    put_u32(buf + 56, 0x20001234);           // personality ptr (skipped)
    put_u32(buf + 60, 0x85003000);           // history[0]
    put_u32(buf + 64, 0x00000000);           // history[1] invalid

    struct boot_info_s b;
    int rc = boot_info_decode(buf, 68, &b);
    CHECK(rc == 0);
    CHECK(b.valid == 1);
    CHECK(b.magic == 0x55AACC33);
    CHECK(b.bootloader_version == 0x01020003);
    CHECK(b.image_version[0] == 0x00010000);
    CHECK(b.image_version[1] == 0xFFFFFFFF);
    CHECK(b.command_arg0 == 0x1122334455667788ULL);
    CHECK(b.command == 0);
    CHECK(b.command_result == 0);
    CHECK(b.stable == 1);
    CHECK(b.fault_registers_length == 3);
    CHECK(b.history_length == 2);
    CHECK(b.reset_cause == 0x83);
    CHECK(b.fault_flags == 0);
    CHECK(b.fault_location == 0xDEADBEEF);
    CHECK(b.history[0] == 0x85003000);
    CHECK(b.history[1] == 0x00000000);

    // too-short buffer must fail cleanly
    struct boot_info_s b2;
    CHECK(boot_info_decode(buf, 10, &b2) == 1);
    CHECK(b2.valid == 0);

    printf("\n");
}

static void test_personality(void) {
    printf("test_personality:\n");
    uint8_t buf[76];
    memset(buf, 0, sizeof(buf));
    buf[0] = 1;                              // format_version
    put_u16(buf + 2, 0xAE49);               // vendor_id (JS320)
    put_u16(buf + 4, 0x0003);               // product_id (JS320)
    buf[6] = 0;                             // subtype
    buf[7] = 1;                             // hw_ver
    put_u64(buf + 8, 0x0102030405060708ULL);// hw_opts
    put_u64(buf + 16, 0x0000000012345678ULL);// mfg_time
    memcpy(buf + 24, "US:MD:01:01;ABCD;202516", 23);
    memcpy(buf + 56, "GY22", 4);
    for (int i = 0; i < 8; ++i) { buf[68 + i] = (uint8_t) (i + 1); }

    struct personality_info_s p;
    int rc = personality_info_decode(buf, sizeof(buf), &p);
    CHECK(rc == 0);
    CHECK(p.valid == 1);
    CHECK(p.format_version == 1);
    CHECK(p.vendor_id == 0xAE49);
    CHECK(p.product_id == 0x0003);
    CHECK(p.subtype == 0);
    CHECK(p.hw_ver == 1);
    CHECK(p.hw_opts == 0x0102030405060708ULL);
    CHECK(p.mfg_time == 0x12345678);
    CHECK(0 == strcmp(p.mfg_info, "US:MD:01:01;ABCD;202516"));
    CHECK(0 == strcmp(p.serial_number, "GY22"));
    CHECK(p.app[0] == 1 && p.app[7] == 8);

    struct personality_info_s p2;
    CHECK(personality_info_decode(buf, 10, &p2) == 1);
    CHECK(p2.valid == 0);

    printf("\n");
}


int main(void) {
    test_boot();
    test_personality();

    printf("=== printed output (visual check) ===\n");
    uint8_t buf[256];
    memset(buf, 0, sizeof(buf));
    put_u32(buf + 0, 0x55AACC33);
    put_u32(buf + 4, 0x01020003);
    put_u32(buf + 36, 0x00830203);
    put_u32(buf + 60, 0x85003000);
    struct boot_info_s b;
    boot_info_decode(buf, 68, &b);
    boot_info_print(stdout, &b);

    if (failures) {
        printf("\n%d CHECK(s) FAILED\n", failures);
        return 1;
    }
    printf("\nALL CHECKS PASSED\n");
    return 0;
}
