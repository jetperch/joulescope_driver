# Copyright 2026 Jetperch LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Generic mb_stdmsg_mem_s protocol client."""

import struct
from .stdmsg import StdMsg


FLASH_PAGE_SIZE = 256
FLASH_BLOCK_64K = 65536

# mb_stdmsg_mem_op_e
MEM_OP_READ = 1
MEM_OP_WRITE = 2
MEM_OP_ERASE = 3

MB_STDMSG_MEM = 0x07
PERSONALITY_TARGET = 2
PERSONALITY_PUBLIC_SIZE = 76
_PERSONALITY_HDR = struct.Struct('<BBHHBBqq')

# mb_stdmsg_header_s: version(u16), type(u8), origin_prefix(u8),
#   metadata(u32)
STDMSG_HDR = struct.Struct('<HBBI')

# mb_stdmsg_mem_s: transaction_id(u32), target(u8), operation(u8),
#   flags(u8), status(u8), timeout_ms(u16), rsv1_u16(u16),
#   offset(u32), length(u32), delay_us(u32)
MEM_CMD_HDR = struct.Struct('<I4B2H3I')


class MemClient:
    """Generic mb_stdmsg_mem_s protocol client.

    :param driver: The Driver instance.
    :param device_path: The device path (e.g. 'u/js320/000000').
    :param cmd_topic: The memory command sub-topic
        (e.g. 's/flash/!cmd').
    :param target: The target memory region ID (0-255).
    """

    def __init__(self, driver, device_path, cmd_topic, target=0):
        self._driver = driver
        self._device_path = device_path
        self._cmd_topic = cmd_topic
        self._target = int(target) & 0xFF
        self._txn_id = 0

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        return False

    @property
    def rsp_topic(self):
        return f'{self._device_path}/h/!rsp'

    @property
    def pub_topic(self):
        return f'{self._device_path}/{self._cmd_topic}'

    def cmd(self, op, offset=0, length=0, data=b'',
            timeout_ms=5000, target=None):
        """Send a memory command and wait for the response.

        :param op: The memory operation (MEM_OP_READ, etc.).
        :param offset: The byte offset within the target region.
        :param length: The operation length in bytes.
        :param data: The payload data for write operations.
        :param timeout_ms: The device-side timeout in milliseconds.
        :param target: Override the instance target for this call.
        :return: The response data bytes.
        :raises TimeoutError: If no response within timeout.
        :raises RuntimeError: If the device returns a non-zero status.
        """
        if target is None:
            target = self._target
        else:
            target = int(target) & 0xFF
        self._txn_id += 1
        stdmsg_hdr = STDMSG_HDR.pack(0, MB_STDMSG_MEM, ord('h'), 0)
        mem_hdr = MEM_CMD_HDR.pack(
            self._txn_id, target, op, 0, 0,
            timeout_ms, 0, offset, length, 0)
        rsp = self._driver.publish_and_wait(
            self.pub_topic,
            StdMsg(stdmsg_hdr + mem_hdr + data),
            self.rsp_topic,
            timeout=(timeout_ms / 1000) + 5)
        status = rsp['status']
        if status != 0:
            raise RuntimeError(
                f'mem op 0x{op:02x} failed '
                f'(status={status}, {self._cmd_topic})')
        return rsp['data']

    def erase_64k(self, offset):
        """Erase a 64 KB block.

        :param offset: The block-aligned byte offset.
        """
        self.cmd(MEM_OP_ERASE, offset=offset,
                 length=FLASH_BLOCK_64K, timeout_ms=5000)

    def write_pages(self, offset, data):
        """Write data in 256-byte page chunks.

        :param offset: The starting byte offset.
        :param data: The bytes to write.
        """
        for i in range(0, len(data), FLASH_PAGE_SIZE):
            chunk = data[i:i + FLASH_PAGE_SIZE]
            if len(chunk) < FLASH_PAGE_SIZE:
                chunk = chunk + b'\xff' * (FLASH_PAGE_SIZE - len(chunk))
            self.cmd(MEM_OP_WRITE, offset=offset + i,
                     length=FLASH_PAGE_SIZE, data=chunk,
                     timeout_ms=1000)

    def read(self, offset, length):
        """Read data in 256-byte page chunks.

        :param offset: The starting byte offset.
        :param length: The number of bytes to read.
        :return: The read bytes.
        """
        result = bytearray()
        for i in range(0, length, FLASH_PAGE_SIZE):
            chunk_len = min(FLASH_PAGE_SIZE, length - i)
            data = self.cmd(MEM_OP_READ, offset=offset + i,
                            length=chunk_len, timeout_ms=1000)
            result.extend(data[:chunk_len])
        return bytes(result)

    def write_verify(self, offset, data, retries=3):
        """Erase, write, and verify with retries.

        :param offset: The flash byte offset.
        :param data: The data to write.
        :param retries: Number of attempts before raising.
        :raises RuntimeError: If verification fails after all retries.
        """
        for attempt in range(retries):
            self.erase_64k(offset & 0xFFFF_0000)
            self.write_pages(offset, data)
            readback = self.read(offset, len(data))
            if readback == data:
                return
        raise RuntimeError(
            f'write/verify failed at 0x{offset:06x} '
            f'after {retries} attempts')

    def write_verify_records(self, records, retries=3):
        """Erase, write, and verify multiple records with retries.

        :param records: The list of (offset, data) pairs.
        :param retries: Number of attempts before raising.
        :raises RuntimeError: If verification fails after all retries.
        """
        for attempt in range(retries):
            erased = {}
            rc = True
            for offset, data in records:
                erase_addr = offset & 0xFFFF_0000
                if erase_addr not in erased:
                    self.erase_64k(erase_addr)
                    erased[erase_addr] = True
                self.write_pages(offset, data)
                readback = self.read(offset, len(data))
                rc &= readback == data
            if rc:
                return
        raise RuntimeError(
            f'write/verify failed after {retries} attempts')

    def read_personality(self):
        """Read the public mb_personality_app_s from the sys task.

        The MemClient must be pointed at the sys task command topic
        (e.g. ``MemClient(driver, device, 'c/sys/!cmd')``).

        :return: A dict with the public personality fields.
        :raises ValueError: If the response is too short.
        """
        data = self.cmd(MEM_OP_READ, offset=0,
                        length=PERSONALITY_PUBLIC_SIZE,
                        target=PERSONALITY_TARGET)
        if len(data) < PERSONALITY_PUBLIC_SIZE:
            raise ValueError(
                f'personality response too short: '
                f'{len(data)} < {PERSONALITY_PUBLIC_SIZE}')
        (fmt_ver, _rsv, vendor_id, product_id,
         subtype, hw_ver, hw_opts, mfg_time
         ) = _PERSONALITY_HDR.unpack_from(data, 0)
        return {
            'format_version': fmt_ver,
            'vendor_id': vendor_id,
            'product_id': product_id,
            'subtype': subtype,
            'hw_ver': hw_ver,
            'hw_opts': hw_opts,
            'mfg_time': mfg_time,
            'mfg_info': data[24:56].rstrip(b'\x00').decode('utf-8'),
            'serial_number': data[56:68].rstrip(b'\x00').decode('utf-8'),
            'app': bytes(data[68:76]),
        }
