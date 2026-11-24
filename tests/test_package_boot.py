#!/usr/bin/env python3
"""Regression coverage for nested GenFv section streams in the real FD check."""
import importlib.util
import hashlib
import lzma
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import uuid

spec = importlib.util.spec_from_file_location("package_boot", Path(__file__).resolve().parents[1] / "scripts/package-boot.py")
pack = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pack)


def section(kind, payload):
    length = 4 + len(payload)
    return length.to_bytes(3, "little") + bytes([kind]) + payload


def volume(loader):
    data = bytearray(256)
    struct.pack_into("<Q", data, 32, len(data))
    data[40:44] = b"_FVH"
    struct.pack_into("<H", data, 48, 56)
    data[128:128 + len(loader)] = loader
    return bytes(data)


def guided_lzma(stream):
    payload = lzma.compress(stream, format=lzma.FORMAT_ALONE)
    header = uuid.UUID("EE4E5898-3914-4259-9D6E-DC7BD79403CF").bytes_le + struct.pack("<HH", 24, 1)
    return section(2, header + payload)


class FirmwarePayloadTests(unittest.TestCase):
    loader = b"MZ-B2L-XNU-LOADER-REGRESSION"

    def test_real_genfv_shape_raw_then_fv_image(self):
        # Actual built stream: 04 00 00 19 (empty RAW), then FV_IMAGE header.
        stream = section(0x19, b"") + section(0x17, volume(self.loader))
        self.assertEqual(stream[:4], b"\x04\x00\x00\x19")
        self.assertTrue(pack.firmware_contains_loader(guided_lzma(stream), self.loader))

    def test_direct_fv_supported(self):
        self.assertTrue(pack.firmware_contains_loader(guided_lzma(volume(self.loader)), self.loader))

    def test_loader_outside_fv_rejected(self):
        stream = section(0x17, volume(b"")) + self.loader
        self.assertFalse(pack.firmware_contains_loader(guided_lzma(stream), self.loader))

    def test_truncated_fv_rejected(self):
        stream = section(0x17, volume(self.loader)[:-1])
        self.assertFalse(pack.firmware_contains_loader(guided_lzma(stream), self.loader))

    def test_truncated_section_rejected(self):
        stream = section(0x19, b"") + section(0x17, volume(self.loader))[:-1]
        self.assertFalse(pack.firmware_contains_loader(guided_lzma(stream), self.loader))

    def test_missing_loader_rejected(self):
        self.assertFalse(pack.firmware_contains_loader(guided_lzma(section(0x17, volume(b""))), self.loader))


class BootTrailerTests(unittest.TestCase):
    def kernel(self):
        image = bytearray(4096)
        struct.pack_into("<III", image, 0, 0xfeedfacf, 0x0100000c, 0)
        return bytes(image)

    def android_prefix(self):
        prefix = bytearray(pack.PAGE + pack.align(4096))
        prefix[:8] = b"ANDROID!"
        struct.pack_into("<I", prefix, 8, 4096)
        struct.pack_into("<II", prefix, 36, pack.PAGE, 1)
        struct.pack_into("<I", prefix, 1644, 1648)
        return prefix

    def verify(self, data):
        with tempfile.NamedTemporaryFile() as image:
            image.write(data)
            image.flush()
            return pack.check_boot(Path(image.name))

    def test_seed_uses_all_256_host_bytes_once(self):
        seed = bytes(range(256))
        with patch.object(pack.os, "urandom", return_value=seed) as random:
            header = pack.make_payload_header(4096)
        random.assert_called_once_with(256)
        self.assertEqual(len(header), 320)
        self.assertEqual(header[32:288], seed)
        self.assertEqual(header[288:320], bytes(32))
        result = self.verify(self.android_prefix() + header + self.kernel())
        self.assertEqual(result["payload_version"], 2)
        self.assertEqual(result["seed_bytes"], 256)

    def test_private_wifi_metadata_is_validated_and_not_disclosed(self):
        mac = bytes([2, 0, 0, 0, 0, 1])  # synthetic local fixture
        header = pack.make_payload_header(4096, mac)
        self.assertEqual(header[288:302], b"B2LWIFI1" + mac)
        result = self.verify(self.android_prefix() + header + self.kernel())
        self.assertTrue(result["wifi_mac_present"])
        self.assertNotIn("02:00:00:00:00:01", str(result))
        for offset in (288, 302, 319):
            bad = bytearray(header)
            bad[offset] ^= 1
            with self.assertRaises(ValueError):
                self.verify(self.android_prefix() + bad + self.kernel())
        for invalid in (bytes(6), bytes([1,0,0,0,0,1]), bytes(5), bytes(7)):
            with self.assertRaises(ValueError):
                pack.make_payload_header(4096, invalid)
        with tempfile.TemporaryDirectory() as directory:
            private = Path(directory) / "mac.txt"
            private.write_text("02:00:00:00:00:01\n")
            self.assertEqual(pack.read_wifi_mac(private), mac)
            private.write_text("untrusted-file-content")
            with self.assertRaisesRegex(ValueError, "exactly six") as error:
                pack.read_wifi_mac(private)
            self.assertNotIn("untrusted-file-content", str(error.exception))

    def test_legacy_64_byte_seed_requires_repackaging(self):
        header = struct.pack("<8sIIQQ", b"B2LXNU01", 1, 128, 4096, 128) + bytes(range(64)) + bytes(32)
        with self.assertRaisesRegex(ValueError, "legacy.*64.*256"):
            self.verify(self.android_prefix() + header + self.kernel())

    def test_private_firmware_descriptor_length_hash_and_explicit_presence(self):
        mac = bytes([2,0,0,0,0,1])
        firmware = bytes((i * 13 + 7) & 255 for i in range(pack.WIFI_FIRMWARE_SIZE))
        header = bytearray(pack.make_payload_header(4096, mac))
        header[288:320] = pack.wifi_metadata(mac, len(firmware))
        image = self.android_prefix() + header + self.kernel() + firmware
        with self.assertRaisesRegex(ValueError, 'SHA-256'):
            self.verify(image)  # Synthetic bytes are never a production input.
        with patch.object(pack, 'WIFI_FIRMWARE_SHA256', hashlib.sha256(firmware).hexdigest()):
            result = self.verify(image)
            self.assertEqual(result['wifi_firmware_bytes'], len(firmware))
            for bad in [image[:-4], image + bytes(4), image[:-1] + bytes([image[-1] ^ 1])]:
                with self.assertRaises(ValueError):
                    self.verify(bad)
        with self.assertRaises(ValueError):
            self.verify(self.android_prefix() + pack.make_payload_header(4096, mac) + self.kernel() + firmware)
        for size in (1, pack.WIFI_FIRMWARE_SIZE - 1, pack.WIFI_FIRMWARE_SIZE + 4):
            with self.assertRaises(ValueError):
                pack.wifi_metadata(mac, size)
        with self.assertRaises(ValueError):
            pack.wifi_metadata(None, pack.WIFI_FIRMWARE_SIZE)

    def test_private_nvram_follows_the_firmware(self):
        # S10 port: Wi-Fi mode 11's B2LFW002 attachment, DHD's nvram.txt processing.
        self.assertEqual(
            pack.process_nvram(b"# board\r\nboardtype=0x1234 # id\r\n\r\nboardrev=0x1101\nmacaddr=00:90:4c:c5:12:38"),
            b"boardtype=0x1234 \0boardrev=0x1101\0macaddr=00:90:4c:c5:12:38" + bytes(5))
        for bad in [b"", b"novalue\n", b"=x\n", b"a=\x01\n"]:
            with self.assertRaises(ValueError):
                pack.process_nvram(bad)
        mac = bytes([2,0,0,0,0,1])
        firmware = bytes((i * 13 + 7) & 255 for i in range(pack.WIFI_FIRMWARE_SIZE))
        nvram = pack.process_nvram(b"boardtype=0x1234\nboardrev=0x1101\n")
        self.assertEqual(len(nvram), 40)
        header = bytearray(pack.make_payload_header(4096, mac))
        header[288:320] = pack.wifi_metadata(mac, len(firmware), len(nvram))
        self.assertEqual(bytes(header[302:310]), b"B2LFW002")
        prefix = self.android_prefix()
        image = prefix + header + self.kernel() + firmware + nvram
        with patch.object(pack, 'WIFI_FIRMWARE_SHA256', hashlib.sha256(firmware).hexdigest()):
            result = self.verify(image)
            self.assertEqual((result['wifi_firmware_bytes'], result['wifi_nvram_bytes']), (len(firmware), len(nvram)))
            for bad in [image[:-4], image + bytes(4), image[:-len(nvram)] + b"\x01" + nvram[1:]]:
                with self.assertRaises(ValueError):
                    self.verify(bad)
            for i in range(14, 32):
                for bit in range(8):
                    broken = bytearray(image)
                    broken[len(prefix) + 288 + i] ^= 1 << bit
                    with self.assertRaises(ValueError):
                        self.verify(bytes(broken))
        for size in (2, 32772):
            with self.assertRaises(ValueError):
                pack.wifi_metadata(mac, len(firmware), size)
        with self.assertRaises(ValueError):
            pack.wifi_metadata(mac, 0, 64)

    def test_private_clm_follows_the_nvram(self):
        # S10 port: Wi-Fi mode 19's B2LFW003 attachment, the board CLM blob after the NVRAM.
        mac = bytes([2,0,0,0,0,1])
        firmware = bytes((i * 13 + 7) & 255 for i in range(pack.WIFI_FIRMWARE_SIZE))
        nvram = pack.process_nvram(b"boardtype=0x1234\nboardrev=0x1101\n")
        clm = b"BLOB" + bytes((i * 7 + 3) & 255 for i in range(pack.WIFI_CLM_SIZE - 4))
        header = bytearray(pack.make_payload_header(4096, mac))
        header[288:320] = pack.wifi_metadata(mac, len(firmware), len(nvram), len(clm))
        self.assertEqual(bytes(header[302:310]), b"B2LFW003")
        self.assertEqual(struct.unpack_from("<H", header, 318)[0], len(clm) // 4)
        prefix = self.android_prefix()
        image = prefix + header + self.kernel() + firmware + nvram + clm
        with patch.object(pack, 'WIFI_FIRMWARE_SHA256', hashlib.sha256(firmware).hexdigest()):
            with self.assertRaisesRegex(ValueError, 'CLM'):
                self.verify(image)  # synthetic bytes are not the pinned blob
            with patch.object(pack, 'WIFI_CLM_SHA256', hashlib.sha256(clm).hexdigest()):
                result = self.verify(image)
                self.assertEqual((result['wifi_nvram_bytes'], result['wifi_clm_bytes']), (len(nvram), len(clm)))
                for bad in [image[:-4], image + bytes(4), image[:-1] + bytes([image[-1] ^ 1])]:
                    with self.assertRaises(ValueError):
                        self.verify(bad)
                for i in range(14, 32):
                    for bit in range(8):
                        broken = bytearray(image)
                        broken[len(prefix) + 288 + i] ^= 1 << bit
                        with self.assertRaises(ValueError):
                            self.verify(bytes(broken))
        # S10 port: B2LFW004, the 128-byte join record after the CLM blob (a model
        # network; nothing about it is ever printed).
        record = pack.wifi_join_record(b"ModelNet", b"model passphrase")
        self.assertEqual(len(record), pack.WIFI_JOIN_SIZE)
        header[288:320] = pack.wifi_metadata(mac, len(firmware), len(nvram), len(clm), len(record))
        self.assertEqual(bytes(header[302:310]), b"B2LFW004")
        joined = prefix + header + self.kernel() + firmware + nvram + clm + record
        with patch.object(pack, 'WIFI_FIRMWARE_SHA256', hashlib.sha256(firmware).hexdigest()), \
                patch.object(pack, 'WIFI_CLM_SHA256', hashlib.sha256(clm).hexdigest()):
            result = self.verify(joined)
            self.assertTrue(result['wifi_join_present'])
            self.assertEqual(result['wifi_clm_bytes'], len(clm))
            for at in (0, 8, 9, 10, 20, 60, 108, 112, 127):
                broken = bytearray(joined)
                broken[len(joined) - 128 + at] ^= 1
                with self.assertRaisesRegex(ValueError, 'join record'):
                    self.verify(bytes(broken))
            with self.assertRaises(ValueError):
                self.verify(joined[:-4])
        for ssid, password in ((b"", b"model passphrase"), (b"x" * 33, b"model passphrase"),
                               (b"ModelNet", b"short"), (b"ModelNet", b"x" * 64), (b"ModelNet", b"tab\there!")):
            with self.assertRaises(ValueError):
                pack.wifi_join_record(ssid, password)
        self.assertEqual(len(pack.wifi_join_record(b"ModelNet", b"0123456789abcdef" * 4)), 128)
        with tempfile.TemporaryDirectory() as tmp:
            join = Path(tmp) / "join.txt"
            join.write_bytes(b"\xef\xbb\xbfModelNet\r\nmodel passphrase\r\n")
            self.assertEqual(pack.read_wifi_join(join), (b"ModelNet", b"model passphrase"))
            join.write_bytes(b"ModelNet\n")
            with self.assertRaises(ValueError):
                pack.read_wifi_join(join)
        with self.assertRaises(ValueError):
            pack.wifi_metadata(mac, len(firmware), len(nvram), 0, 128)       # needs the CLM blob
        with self.assertRaises(ValueError):
            pack.wifi_metadata(mac, len(firmware), len(nvram), len(clm), 64)  # the record is 128 bytes
        for size in (2, pack.WIFI_CLM_MAX + 4):
            with self.assertRaises(ValueError):
                pack.wifi_metadata(mac, len(firmware), len(nvram), size)
        with self.assertRaises(ValueError):
            pack.wifi_metadata(mac, len(firmware), 0, 64)

    def test_truncated_v2_seed_rejected(self):
        with self.assertRaisesRegex(ValueError, "truncated"):
            self.verify(self.android_prefix() + pack.make_payload_header(4096)[:200])

    def test_wrong_v2_offset_rejected(self):
        header = bytearray(pack.make_payload_header(4096))
        struct.pack_into("<Q", header, 24, 128)
        with self.assertRaisesRegex(ValueError, "invalid BOOT trailer header"):
            self.verify(self.android_prefix() + header + self.kernel())

    def test_declared_kernel_length_overflow_rejected(self):
        header = bytearray(pack.make_payload_header(4096))
        struct.pack_into("<Q", header, 16, 2**64 - 1)
        with self.assertRaisesRegex(ValueError, "invalid BOOT trailer size"):
            self.verify(self.android_prefix() + header + self.kernel())


if __name__ == "__main__":
    unittest.main()
