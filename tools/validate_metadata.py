#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Validate metadata blobs by reading from device and comparing to known-good."""

import subprocess
import sys
import os
import struct
import tempfile

MINIBITTY = r"C:\repos\Jetperch\minibitty_host\cmake-build\example\Debug\minibitty.exe"
DEVICE = "u/js320/8w2a"

BLOBS = [
    {
        "name": "ctrl pubsub_metadata",
        "topic": "c/xspi/!cmd",
        "offset": 0x080000,
        "golden": r"C:\repos\Jetperch\js320\mbbuild\js320_p1_0_0_app\pubsub_metadata.bin",
    },
    {
        "name": "sensor pubsub_metadata",
        "topic": "s/flash/!cmd",
        "offset": 0x140000,
        "golden": r"C:\repos\Jetperch\js320\mbbuild\js320_p1_1_0_app\pubsub_metadata.bin",
    },
]


def check32_xxhash(data):
    """Compute mb_check32_xxhash over data (bytes, must be multiple of 4)."""
    assert len(data) % 4 == 0
    words = len(data) // 4
    value = 0x9E3779B1
    mask = 0xFFFFFFFF
    for i in range(words):
        w = struct.unpack_from("<I", data, i * 4)[0]
        value = (value + (w * 0x85EBCA6B) & mask) & mask
        value = ((value << 13) | (value >> 19)) & mask
        value = (value * 0xC2B2AE35) & mask
    return value


def read_blob(topic, offset, size):
    """Read a blob from the device using minibitty mem read."""
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
        out_path = f.name

    try:
        cmd = [
            MINIBITTY, "mem", topic, "0",
            "read", f"0x{offset:06X}", str(size),
            "--out", out_path, DEVICE,
        ]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        if result.returncode != 0:
            print(f"  mem read failed (rc={result.returncode})")
            if result.stderr:
                print(f"  stderr: {result.stderr[:200]}")
            return None
        with open(out_path, "rb") as f:
            return f.read()
    except subprocess.TimeoutExpired:
        print("  mem read timed out")
        return None
    finally:
        try:
            os.unlink(out_path)
        except OSError:
            pass


def compare(name, golden_data, read_data):
    """Compare golden vs read data, return True if match."""
    if len(read_data) < len(golden_data):
        print(f"  {name}: read too short ({len(read_data)} < {len(golden_data)})")
        return False

    # Truncate read to golden size (read may include page padding)
    read_trimmed = read_data[:len(golden_data)]

    if read_trimmed == golden_data:
        return True

    # Find first difference
    for i in range(len(golden_data)):
        if read_trimmed[i] != golden_data[i]:
            context = min(16, len(golden_data) - i)
            print(f"  {name}: MISMATCH at byte {i} (0x{i:04X})")
            print(f"    golden: {golden_data[i:i+context].hex(' ')}")
            print(f"    read:   {read_trimmed[i:i+context].hex(' ')}")
            return False
    return True


def validate_check32(name, data):
    """Validate the check32 of the blob."""
    if len(data) < 8 or len(data) % 4 != 0:
        print(f"  {name}: invalid size {len(data)}")
        return False

    total_size = struct.unpack_from("<I", data, 12)[0]
    if total_size > len(data):
        print(f"  {name}: total_size {total_size} > data size {len(data)}")
        return False

    check_data = data[:total_size - 4]
    stored_check = struct.unpack_from("<I", data, total_size - 4)[0]
    computed_check = check32_xxhash(check_data)

    if computed_check != stored_check:
        print(f"  {name}: check32 MISMATCH stored=0x{stored_check:08X}"
              f" computed=0x{computed_check:08X}")
        return False
    return True


def run_iteration(iteration, golden_blobs):
    """Run one iteration of read + validate for all blobs."""
    all_ok = True
    for blob_info, golden_data in zip(BLOBS, golden_blobs):
        name = blob_info["name"]
        size = len(golden_data)
        # Round up to page boundary for read
        read_size = ((size + 255) // 256) * 256

        read_data = read_blob(blob_info["topic"], blob_info["offset"], read_size)
        if read_data is None:
            all_ok = False
            continue

        # Validate check32 of read data
        if not validate_check32(name, read_data[:size]):
            all_ok = False

        # Compare to golden
        if not compare(name, golden_data, read_data):
            all_ok = False

    return all_ok


def main():
    iterations = int(sys.argv[1]) if len(sys.argv) > 1 else 20

    # Load golden files
    golden_blobs = []
    for blob_info in BLOBS:
        path = blob_info["golden"]
        with open(path, "rb") as f:
            data = f.read()
        golden_blobs.append(data)
        # Verify golden file itself
        assert validate_check32(blob_info["name"] + " (golden)", data)
        print(f"Golden: {blob_info['name']} = {len(data)} bytes, check32 OK")

    print(f"\nRunning {iterations} iterations...\n")

    pass_count = 0
    fail_count = 0

    for i in range(1, iterations + 1):
        ok = run_iteration(i, golden_blobs)
        if ok:
            pass_count += 1
            print(f"  [{i}/{iterations}] PASS")
        else:
            fail_count += 1
            print(f"  [{i}/{iterations}] FAIL")

    print(f"\n=== Results: pass={pass_count}, fail={fail_count},"
          f" total={pass_count + fail_count} ===")
    return 1 if fail_count else 0


if __name__ == "__main__":
    sys.exit(main())
