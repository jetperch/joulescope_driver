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

import struct
import unittest
from pyjoulescope_driver.mem_client import (
    MemClient, MEM_OP_READ, MEM_OP_WRITE, MEM_OP_ERASE,
    FLASH_PAGE_SIZE, FLASH_BLOCK_64K,
    STDMSG_HDR, MEM_CMD_HDR, MB_STDMSG_MEM,
    PERSONALITY_PUBLIC_SIZE, PERSONALITY_TARGET,
)


DEVICE = 'u/js320/000001'
CMD_TOPIC = 's/flash/!cmd'


def _make_rsp(status=0, data=b'', offset=0, length=None,
              operation=MEM_OP_READ, target=0, transaction_id=1):
    if length is None:
        length = len(data)
    return {
        'dtype': 'stdmsg',
        'transaction_id': transaction_id,
        'target': target,
        'operation': operation,
        'flags': 0,
        'status': status,
        'timeout_ms': 0,
        'offset': offset,
        'length': length,
        'delay_us': 0,
        'data': data,
    }


def _parse_published(msg_bytes):
    """Parse the packed stdmsg + mem header from a published message."""
    hdr_size = STDMSG_HDR.size + MEM_CMD_HDR.size
    stdmsg = STDMSG_HDR.unpack_from(msg_bytes, 0)
    mem = MEM_CMD_HDR.unpack_from(msg_bytes, STDMSG_HDR.size)
    payload = msg_bytes[hdr_size:]
    return {
        'version': stdmsg[0],
        'type': stdmsg[1],
        'origin_prefix': stdmsg[2],
        'metadata': stdmsg[3],
        'transaction_id': mem[0],
        'target': mem[1],
        'operation': mem[2],
        'flags': mem[3],
        'status': mem[4],
        'timeout_ms': mem[5],
        'rsv1': mem[6],
        'offset': mem[7],
        'length': mem[8],
        'delay_us': mem[9],
        'payload': payload,
    }


class MockDriver:
    """Mock Driver that captures publishes and sends responses.

    Implements publish_and_wait matching the real Driver API.
    """

    def __init__(self):
        self.published = []
        self.response_fn = None

    def publish_and_wait(self, publish_topic, publish_value,
                         response_topic, timeout=None):
        msg_bytes = bytes(publish_value)
        self.published.append((publish_topic, msg_bytes))
        if self.response_fn is None:
            raise TimeoutError(
                f'publish_and_wait timed out: '
                f'publish={publish_topic}, '
                f'response={response_topic}')
        rsp = self.response_fn(publish_topic, msg_bytes)
        if rsp is None:
            raise TimeoutError(
                f'publish_and_wait timed out: '
                f'publish={publish_topic}, '
                f'response={response_topic}')
        return rsp


class TestMemClient(unittest.TestCase):

    def setUp(self):
        self.driver = MockDriver()
        self.mem = MemClient(self.driver, DEVICE, CMD_TOPIC, target=0)

    def _set_auto_response(self, status=0, data=b''):
        def response_fn(topic, msg_bytes):
            parsed = _parse_published(msg_bytes)
            return _make_rsp(
                status=status,
                data=data,
                offset=parsed['offset'],
                length=len(data),
                operation=parsed['operation'],
                target=parsed['target'],
                transaction_id=parsed['transaction_id'],
            )
        self.driver.response_fn = response_fn

    def test_context_manager(self):
        driver = MockDriver()
        with MemClient(driver, DEVICE, CMD_TOPIC) as mem:
            self.assertIsNotNone(mem)

    def test_cmd_packs_target(self):
        self.mem = MemClient(self.driver, DEVICE, CMD_TOPIC, target=5)
        self._set_auto_response()
        self.mem.cmd(MEM_OP_READ, offset=0x1000, length=16)
        msg = _parse_published(self.driver.published[-1][1])
        self.assertEqual(5, msg['target'])
        self.assertEqual(MEM_OP_READ, msg['operation'])
        self.assertEqual(0x1000, msg['offset'])
        self.assertEqual(16, msg['length'])
        self.assertEqual(MB_STDMSG_MEM, msg['type'])

    def test_cmd_target_override(self):
        self._set_auto_response()
        self.mem.cmd(MEM_OP_READ, offset=0, length=1, target=42)
        msg = _parse_published(self.driver.published[-1][1])
        self.assertEqual(42, msg['target'])

    def test_cmd_default_target_zero(self):
        self._set_auto_response()
        self.mem.cmd(MEM_OP_READ, offset=0, length=1)
        msg = _parse_published(self.driver.published[-1][1])
        self.assertEqual(0, msg['target'])

    def test_cmd_timeout(self):
        self.driver.response_fn = None
        with self.assertRaises(TimeoutError):
            self.mem.cmd(MEM_OP_READ, timeout_ms=100)

    def test_cmd_error_status(self):
        self._set_auto_response(status=1)
        with self.assertRaises(RuntimeError):
            self.mem.cmd(MEM_OP_READ)

    def test_cmd_publishes_to_correct_topic(self):
        self._set_auto_response()
        self.mem.cmd(MEM_OP_READ)
        topic = self.driver.published[-1][0]
        self.assertEqual(f'{DEVICE}/{CMD_TOPIC}', topic)

    def test_transaction_id_increments(self):
        self._set_auto_response()
        self.mem.cmd(MEM_OP_READ)
        msg1 = _parse_published(self.driver.published[-1][1])
        self.mem.cmd(MEM_OP_READ)
        msg2 = _parse_published(self.driver.published[-1][1])
        self.assertEqual(msg1['transaction_id'] + 1,
                         msg2['transaction_id'])

    def test_read_single_page(self):
        test_data = bytes(range(64))

        def response_fn(topic, msg_bytes):
            parsed = _parse_published(msg_bytes)
            return _make_rsp(data=test_data,
                             offset=parsed['offset'],
                             operation=parsed['operation'],
                             transaction_id=parsed['transaction_id'])
        self.driver.response_fn = response_fn
        result = self.mem.read(0x1000, 64)
        self.assertEqual(test_data, result)
        self.assertEqual(1, len(self.driver.published))

    def test_read_multi_page(self):
        total_len = FLASH_PAGE_SIZE + 100
        full_data = bytes(i & 0xFF for i in range(total_len))

        def response_fn(topic, msg_bytes):
            parsed = _parse_published(msg_bytes)
            off = parsed['offset'] - 0x2000
            chunk_len = parsed['length']
            return _make_rsp(data=full_data[off:off + chunk_len],
                             offset=parsed['offset'],
                             operation=parsed['operation'],
                             transaction_id=parsed['transaction_id'])
        self.driver.response_fn = response_fn
        result = self.mem.read(0x2000, total_len)
        self.assertEqual(full_data, result)
        self.assertEqual(2, len(self.driver.published))

    def test_write_pages_exact(self):
        data = bytes(range(FLASH_PAGE_SIZE))
        self._set_auto_response()
        self.mem.write_pages(0x3000, data)
        self.assertEqual(1, len(self.driver.published))
        msg = _parse_published(self.driver.published[0][1])
        self.assertEqual(MEM_OP_WRITE, msg['operation'])
        self.assertEqual(0x3000, msg['offset'])
        self.assertEqual(FLASH_PAGE_SIZE, msg['length'])
        self.assertEqual(data, msg['payload'])

    def test_write_pages_partial(self):
        data = bytes(range(100))
        self._set_auto_response()
        self.mem.write_pages(0x4000, data)
        self.assertEqual(1, len(self.driver.published))
        msg = _parse_published(self.driver.published[0][1])
        expected = data + b'\xff' * (FLASH_PAGE_SIZE - 100)
        self.assertEqual(expected, msg['payload'])

    def test_write_pages_multi(self):
        data = bytes(i & 0xFF for i in range(FLASH_PAGE_SIZE + 50))
        self._set_auto_response()
        self.mem.write_pages(0x5000, data)
        self.assertEqual(2, len(self.driver.published))
        msg0 = _parse_published(self.driver.published[0][1])
        msg1 = _parse_published(self.driver.published[1][1])
        self.assertEqual(0x5000, msg0['offset'])
        self.assertEqual(0x5000 + FLASH_PAGE_SIZE, msg1['offset'])

    def test_erase_64k(self):
        self._set_auto_response()
        self.mem.erase_64k(0x10000)
        self.assertEqual(1, len(self.driver.published))
        msg = _parse_published(self.driver.published[0][1])
        self.assertEqual(MEM_OP_ERASE, msg['operation'])
        self.assertEqual(0x10000, msg['offset'])
        self.assertEqual(FLASH_BLOCK_64K, msg['length'])

    def test_write_verify_success(self):
        data = bytes(range(64))
        call_count = [0]

        def response_fn(topic, msg_bytes):
            call_count[0] += 1
            parsed = _parse_published(msg_bytes)
            if parsed['operation'] == MEM_OP_READ:
                return _make_rsp(data=data,
                                 offset=parsed['offset'],
                                 operation=parsed['operation'],
                                 transaction_id=parsed['transaction_id'])
            return _make_rsp(offset=parsed['offset'],
                             operation=parsed['operation'],
                             transaction_id=parsed['transaction_id'])
        self.driver.response_fn = response_fn
        self.mem.write_verify(0x10000, data)
        # erase + write_pages(1) + read(1) = 3
        self.assertEqual(3, call_count[0])

    def test_write_verify_retry(self):
        data = bytes(range(64))
        attempt = [0]

        def response_fn(topic, msg_bytes):
            parsed = _parse_published(msg_bytes)
            if parsed['operation'] == MEM_OP_READ:
                attempt[0] += 1
                if attempt[0] == 1:
                    return _make_rsp(
                        data=b'\x00' * len(data),
                        offset=parsed['offset'],
                        operation=parsed['operation'],
                        transaction_id=parsed['transaction_id'])
                return _make_rsp(data=data,
                                 offset=parsed['offset'],
                                 operation=parsed['operation'],
                                 transaction_id=parsed['transaction_id'])
            return _make_rsp(offset=parsed['offset'],
                             operation=parsed['operation'],
                             transaction_id=parsed['transaction_id'])
        self.driver.response_fn = response_fn
        self.mem.write_verify(0x10000, data)
        self.assertEqual(2, attempt[0])

    def test_write_verify_failure(self):
        data = bytes(range(64))

        def response_fn(topic, msg_bytes):
            parsed = _parse_published(msg_bytes)
            if parsed['operation'] == MEM_OP_READ:
                return _make_rsp(
                    data=b'\x00' * len(data),
                    offset=parsed['offset'],
                    operation=parsed['operation'],
                    transaction_id=parsed['transaction_id'])
            return _make_rsp(offset=parsed['offset'],
                             operation=parsed['operation'],
                             transaction_id=parsed['transaction_id'])
        self.driver.response_fn = response_fn
        with self.assertRaises(RuntimeError):
            self.mem.write_verify(0x10000, data, retries=2)

    def test_write_verify_records_success(self):
        data1 = bytes(range(64))
        data2 = bytes(range(64, 128))
        records = [(0x10000, data1), (0x10100, data2)]

        def response_fn(topic, msg_bytes):
            parsed = _parse_published(msg_bytes)
            if parsed['operation'] == MEM_OP_READ:
                off = parsed['offset']
                if off == 0x10000:
                    return _make_rsp(
                        data=data1, offset=off,
                        operation=MEM_OP_READ,
                        transaction_id=parsed['transaction_id'])
                else:
                    return _make_rsp(
                        data=data2, offset=off,
                        operation=MEM_OP_READ,
                        transaction_id=parsed['transaction_id'])
            return _make_rsp(offset=parsed['offset'],
                             operation=parsed['operation'],
                             transaction_id=parsed['transaction_id'])
        self.driver.response_fn = response_fn
        self.mem.write_verify_records(records)

    def test_write_verify_records_retry_resets_erased(self):
        """Verify that erased blocks are re-erased on retry."""
        data = bytes(range(64))
        records = [(0x10000, data)]
        attempt = [0]
        erase_count = [0]

        def response_fn(topic, msg_bytes):
            parsed = _parse_published(msg_bytes)
            if parsed['operation'] == MEM_OP_ERASE:
                erase_count[0] += 1
            if parsed['operation'] == MEM_OP_READ:
                attempt[0] += 1
                if attempt[0] == 1:
                    return _make_rsp(
                        data=b'\x00' * len(data),
                        offset=parsed['offset'],
                        operation=parsed['operation'],
                        transaction_id=parsed['transaction_id'])
                return _make_rsp(data=data,
                                 offset=parsed['offset'],
                                 operation=parsed['operation'],
                                 transaction_id=parsed['transaction_id'])
            return _make_rsp(offset=parsed['offset'],
                             operation=parsed['operation'],
                             transaction_id=parsed['transaction_id'])
        self.driver.response_fn = response_fn
        self.mem.write_verify_records(records)
        self.assertEqual(2, erase_count[0],
                         'block must be re-erased on retry')

    def test_read_personality(self):
        payload = struct.pack('<BBHHBBqq',
                              1, 0, 0x1234, 0x5678, 2, 3,
                              0xDEADBEEF, -1000000)
        payload += b'US:MD:01:01;ABCD;202516\x00\x00\x00\x00\x00\x00\x00\x00\x00'
        payload += b'JS320-00001\x00'
        payload += bytes(range(8))
        self.assertEqual(PERSONALITY_PUBLIC_SIZE, len(payload))

        def response_fn(topic, msg_bytes):
            parsed = _parse_published(msg_bytes)
            self.assertEqual(PERSONALITY_TARGET, parsed['target'])
            return _make_rsp(data=payload,
                             offset=parsed['offset'],
                             operation=parsed['operation'],
                             target=parsed['target'],
                             transaction_id=parsed['transaction_id'])
        self.driver.response_fn = response_fn
        p = self.mem.read_personality()
        self.assertEqual(1, p['format_version'])
        self.assertEqual(0x1234, p['vendor_id'])
        self.assertEqual(0x5678, p['product_id'])
        self.assertEqual(2, p['subtype'])
        self.assertEqual(3, p['hw_ver'])
        self.assertEqual(0xDEADBEEF, p['hw_opts'])
        self.assertEqual(-1000000, p['mfg_time'])
        self.assertEqual('US:MD:01:01;ABCD;202516', p['mfg_info'])
        self.assertEqual('JS320-00001', p['serial_number'])
        self.assertEqual(bytes(range(8)), p['app'])

    def test_read_personality_short_data(self):
        def response_fn(topic, msg_bytes):
            parsed = _parse_published(msg_bytes)
            return _make_rsp(data=b'\x00' * 10,
                             offset=parsed['offset'],
                             operation=parsed['operation'],
                             transaction_id=parsed['transaction_id'])
        self.driver.response_fn = response_fn
        with self.assertRaises(ValueError):
            self.mem.read_personality()


if __name__ == '__main__':
    unittest.main()
