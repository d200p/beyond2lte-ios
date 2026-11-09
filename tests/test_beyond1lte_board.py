#!/usr/bin/env python3
"""Galaxy S10 (beyond1lte) board support: overlay generation and memory evidence.

Runs offline with synthetic fixtures plus the checked-in evidence files. It does
not need a phone, a Mu-Silicium checkout or a compiler.
"""
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
OVERLAY = REPO / "scripts/make-beyond1lte-overlay.py"
EVIDENCE = REPO / "scripts/fdt-memory-evidence.py"

spec = importlib.util.spec_from_file_location("fdt_memory_evidence", EVIDENCE)
fdt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fdt)
spec = importlib.util.spec_from_file_location("package_boot", REPO / "scripts/package-boot.py")
pack = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pack)
spec = importlib.util.spec_from_file_location("pull_logs", REPO / "scripts/pull-b2l-unified-logs.py")
logs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(logs)


def make_dtb(model, extra_nodes=()):
    """Build a minimal valid FDT: root model, cells, and optional child nodes."""
    strings = bytearray()
    offsets = {}

    def name(text):
        if text not in offsets:
            offsets[text] = len(strings)
            strings.extend(text.encode() + b"\0")
        return offsets[text]

    struct_block = bytearray()

    def begin(node):
        struct_block.extend(struct.pack(">I", 1) + node.encode() + b"\0")
        while len(struct_block) % 4:
            struct_block.append(0)

    def prop(key, value):
        struct_block.extend(struct.pack(">III", 3, len(value), name(key)) + value)
        while len(struct_block) % 4:
            struct_block.append(0)

    begin("")
    prop("model", model.encode() + b"\0")
    prop("#address-cells", struct.pack(">I", 2))
    prop("#size-cells", struct.pack(">I", 1))
    for node, props in extra_nodes:
        begin(node)
        for key, value in props.items():
            prop(key, value)
        struct_block.extend(struct.pack(">I", 2))
    struct_block.extend(struct.pack(">I", 2) + struct.pack(">I", 9))
    header_size, rsvmap = 40, 16
    off_rsv = header_size
    off_struct = off_rsv + rsvmap
    off_strings = off_struct + len(struct_block)
    total = off_strings + len(strings)
    header = struct.pack(">10I", 0xD00DFEED, total, off_struct, off_strings, off_rsv,
                         17, 16, 0, len(strings), len(struct_block))
    return header + bytes(rsvmap) + bytes(struct_block) + bytes(strings)


S10_MODEL = "Samsung BEYOND1LTE EUR OPEN 26 board based on EXYNOS9820"
S10P_MODEL = "Samsung BEYOND2LTE EUR OPEN 26 board based on EXYNOS9820"


class OverlayTests(unittest.TestCase):
    def tree(self, root, dtb_model=S10_MODEL):
        (root / "Mu_Basecore").mkdir()
        pkg = root / "Platforms/Samsung/beyond2ltePkg"
        files = {
            pkg / "beyond2lte.dsc": (
                "[Defines]\n  PLATFORM_NAME = beyond2lte\n"
                "  PLATFORM_GUID                  = 556BDA6E-EBC8-4D8F-8EA6-DAA2D7AD7A68\n"
                "  FLASH_DEFINITION = beyond2ltePkg/beyond2lte.fdf\n"
                "[PcdsFixedAtBuild]\n"
                '  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemModel|"Galaxy S10+"\n'
                '  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailSku|"Galaxy_S10+_beyond2lte"\n'
                "  # measured on SM-G975F with Galaxy S10+ firmware\n"
                "[LibraryClasses]\n  MemoryMapLib|beyond2ltePkg/Library/MemoryMapLib/MemoryMapLib.inf\n"),
            pkg / "beyond2lte.fdf": "!include beyond2ltePkg/Include/DXE.inc\n",
            pkg / "beyond2lte.dec": "[Defines]\n  PACKAGE_NAME = beyond2ltePkg\n"
                                    "  PACKAGE_GUID                   = 77A1BDCE-7345-4B7D-AE53-D803AD66D1E8\n",
            pkg / "Include/DXE.inc": "  # INF beyond2ltePkg/Drivers/B2LTouchDxe/B2LTouchDxe.inf\n",
            pkg / "Library/MemoryMapLib/MemoryMapLib.c": '  {"Lower dynamic guard", 0x80000000, 0x07000000},\n',
            pkg / "Drivers/B2LTouchDxe/B2LTouchDxe.c": "/* sec_ts for Galaxy S10+ (beyond2lte); BEYOND2LTE banner */\n",
            root / "Resources/Configs/beyond2lte.toml": "[uefi_fd]\nbase = 0xC9000000\nsize = 0x400000\n",
            # Mu-Silicium's stock S10 package: its platform GUID must be kept.
            root / "Platforms/Samsung/beyond1ltePkg/beyond1lte.dsc":
                "[Defines]\n  PLATFORM_GUID                  = 4E670446-C67E-4FEB-8D15-A0A05C824B80\n",
        }
        for path, text in files.items():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        dtbs = root / "Resources/DTBs"
        dtbs.mkdir(parents=True)
        (dtbs / "beyond1lte.dtb").write_bytes(make_dtb(dtb_model))

    def run_overlay(self, root):
        return subprocess.run([sys.executable, str(OVERLAY), "--mu", str(root)], capture_output=True, text=True)

    def test_generates_renamed_package_and_is_idempotent(self):
        with tempfile.TemporaryDirectory(prefix="b1l-overlay-") as directory:
            root = Path(directory)
            self.tree(root)
            for unused in range(2):
                result = self.run_overlay(root)
                self.assertEqual(result.returncode, 0, result.stderr)
            pkg = root / "Platforms/Samsung/beyond1ltePkg"
            dsc = (pkg / "beyond1lte.dsc").read_text()
            self.assertIn("PLATFORM_NAME = beyond1lte", dsc)
            self.assertIn("4E670446-C67E-4FEB-8D15-A0A05C824B80", dsc)  # stock S10 identity
            self.assertIn('PcdSmbiosSystemModel|"Galaxy S10"', dsc)
            self.assertIn('"Galaxy_S10_beyond1lte"', dsc)
            self.assertIn("# measured on SM-G975F with Galaxy S10+ firmware", dsc)  # provenance kept
            self.assertIn("MemoryMapLib|beyond1ltePkg/Library/MemoryMapLib/MemoryMapLib.inf", dsc)
            dec = (pkg / "beyond1lte.dec").read_text()
            self.assertIn("PACKAGE_NAME = beyond1ltePkg", dec)
            self.assertNotIn("77A1BDCE-7345-4B7D-AE53-D803AD66D1E8", dec)
            self.assertEqual((pkg / "Include/DXE.inc").read_text(),
                             "  # INF beyond1ltePkg/Drivers/B2LTouchDxe/B2LTouchDxe.inf\n")
            self.assertIn("BEYOND1LTE", (pkg / "Drivers/B2LTouchDxe/B2LTouchDxe.c").read_text())
            self.assertFalse((pkg / "beyond2lte.dsc").exists())
            self.assertIn("size = 0x400000", (root / "Resources/Configs/beyond1lte.toml").read_text())
            self.assertTrue((root / "Platforms/Samsung/beyond2ltePkg/beyond2lte.dsc").is_file())  # source untouched

    def test_rejects_s10_plus_device_tree(self):
        with tempfile.TemporaryDirectory(prefix="b1l-overlay-") as directory:
            root = Path(directory)
            self.tree(root, dtb_model=S10P_MODEL)
            result = self.run_overlay(root)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("not a Galaxy S10", result.stderr)
            self.assertFalse((root / "Platforms/Samsung/beyond1ltePkg/beyond1lte.fdf").exists())

    def test_rejects_non_disposable_tree(self):
        with tempfile.TemporaryDirectory(prefix="b1l-overlay-") as directory:
            root = Path(directory)
            self.tree(root)
            (root / "Mu_Basecore").rmdir()
            result = self.run_overlay(root)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("disposable", result.stderr)


class EvidenceTests(unittest.TestCase):
    def test_live_s10_evidence_extends_s10_plus_basis_by_mm_debug_only(self):
        s10 = json.loads((REPO / "common/memory/beyond1lte-8g-dt.json").read_text())
        s10p = json.loads((REPO / "common/memory/beyond2lte-8g-dt.json").read_text())
        self.assertIn("BEYOND1LTE", s10["/"]["model"])
        self.assertIn("G973FXXUHHXJ1", s10["_source"])
        problems, extras, banks, reserved = fdt.compare(s10, s10p, allow_extra=True)
        self.assertEqual(problems, [])
        self.assertEqual((banks, reserved), (6, 29))
        self.assertEqual(len(extras), 1)
        self.assertIn("/reserved-memory/mm_debug", extras[0])
        strict, unused, unused_banks, unused_reserved = fdt.compare(s10, s10p)
        self.assertEqual(len(strict), 1)  # without --allow-extra the new entry is reported

    def test_no_fixed_reservation_overlaps_the_ram_allowlist(self):
        header = (REPO / "common/memory/b2l_ram_ranges.h").read_text()
        self.assertEqual(header, (REPO / "xnu-overlay/pexpert/pexpert/arm64/b2l_ram_ranges.h").read_text())
        for name in ("beyond1lte-8g-dt.json", "beyond2lte-8g-dt.json"):
            doc = json.loads((REPO / "common/memory" / name).read_text())
            hits, count = fdt.allowlist_overlaps(doc, header)
            self.assertEqual(hits, [], name)
            self.assertEqual(count, 12)
        # The S10 page would have been handed to XNU by the original 11-range list.
        old = "{0x87200000ULL,0xee00000ULL}"
        s10 = json.loads((REPO / "common/memory/beyond1lte-8g-dt.json").read_text())
        hits, unused = fdt.allowlist_overlaps(s10, old)
        self.assertEqual(len(hits), 1)
        self.assertIn("mm_debug", hits[0])

    def test_parser_and_compare_detect_a_changed_reservation(self):
        reg = bytes.fromhex("00000009fd80000002800000")
        dtb = make_dtb(S10_MODEL, [("reserved-memory", {}), ])
        doc = fdt.evidence(fdt.parse_fdt(dtb))
        self.assertEqual(doc["/"]["model"], S10_MODEL)
        left = {"/reserved-memory/el2_earlymem": {"reg": reg.hex()}}
        right = {"/reserved-memory/el2_earlymem": {"reg": "00000009fd80000002000000"}}
        for allow_extra in (False, True):  # a changed shared entry always fails
            problems, extras, unused_banks, reserved = fdt.compare(left, right, allow_extra)
            self.assertEqual(reserved, 1)
            self.assertEqual(len(problems), 1)
            self.assertEqual(extras, [])
        problems, extras, unused_banks, unused = fdt.compare({}, right, allow_extra=True)
        self.assertEqual(len(problems), 1)  # a missing shared entry always fails

    def test_rejects_text_dts(self):
        with self.assertRaisesRegex(ValueError, "FDT magic"):
            fdt.parse_fdt(b"/dts-v1/;\n/ { model = \"x\"; };\n" + bytes(40))

    def test_package_dtb_model_guard(self):
        self.assertIn("BEYOND1LTE", pack.dtb_model(make_dtb(S10_MODEL)))
        self.assertEqual(pack.dtb_model(b"\xd0\x0d\xfe\xed" + bytes(36)), "")
        self.assertEqual(pack.DEVICES["beyond1lte"], ("SM-G973F", "BEYOND1LTE"))


class RecoveryReaderTests(unittest.TestCase):
    """The unified-log exporter on an S10: own serial, by-name USERDATA, read only."""

    def reader(self, product=b"beyond1lte\n", mounts=b"/dev/block/sda20 /cache ext4 rw 0 0\n",
               serial="S10TESTSERIAL", device="/dev/block/by-name/userdata"):
        calls = []
        answers = {
            ("get-state",): b"recovery\n",
            ("shell", "-T", "getprop ro.product.device"): product,
            ("shell", "-T", "id -u"): b"0\n",
            ("shell", "-T", "readlink -f /dev/block/by-name/userdata"): b"/dev/block/sda30\n",
            ("shell", "-T", "readlink -f /dev/block/sda30"): b"/dev/block/sda30\n",
            ("shell", "-T", "cat /proc/mounts"): mounts,
            ("shell", "-T", "readlink -f /dev/block/sda20"): b"/dev/block/sda20\n",
            ("shell", "-T", "blockdev --getsize64 /dev/block/sda30"): b"104857600\n",
            ("exec-out", "sh", "-c", "dd if=/dev/block/sda30 bs=4096 skip=0 count=1 2>/dev/null"): bytes(4096),
        }

        class Fake(logs.AdbReader):
            def call(self, *args):
                calls.append((self.adb, args))
                return answers[args]
        return Fake("adb", serial, device), calls

    def test_s10_serial_and_by_name_userdata(self):
        reader, calls = self.reader()
        self.assertEqual(reader.device, "/dev/block/sda30")
        self.assertEqual(reader.identity["product"], "beyond1lte")
        self.assertEqual(reader.identity["serial"], "S10TESTSERIAL")
        self.assertEqual(reader.read(0, 16), bytes(16))
        self.assertTrue(all(adb == ["adb", "-s", "S10TESTSERIAL"] for adb, _ in calls))
        self.assertFalse(any(word in " ".join(args) for _, args in calls for word in (" of=", "dd of", "mount ")))

    def test_rejects_other_phone_mounted_volume_and_bad_arguments(self):
        with self.assertRaisesRegex(Exception, "wrong product"):
            self.reader(product=b"starlte\n")
        with self.assertRaisesRegex(Exception, "mounted"):
            self.reader(mounts=b"/dev/block/sda30 /data ext4 rw 0 0\n/dev/block/sda20 /cache ext4 rw 0 0\n")
        with self.assertRaisesRegex(Exception, "invalid ADB serial"):
            self.reader(serial="bad serial; reboot")
        with self.assertRaisesRegex(Exception, "USERDATA must be"):
            self.reader(device="/dev/block/by-name/boot")


if __name__ == "__main__":
    unittest.main()
