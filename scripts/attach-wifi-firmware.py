#!/usr/bin/env python3
"""Make a private BOOT image with the Wi-Fi firmware attached, from a CI one.

The CI's BOOT image (s10-userspace-boot/boot.img) carries no firmware and no
Wi-Fi address. This adds both locally, the way docs/packaging.md describes:
the 32-byte field at trailer + 288 becomes B2LWIFI1, the six address octets
and the B2LFW001 descriptor, and the pinned original station firmware
(bcmdhd_sta.bin_b1, 1,297,036 bytes) follows the Mach-O to the end of the
file. The loader then hands it to XNU through /chosen/b2l,wifi-firmware, and
b2l_wifi_diag=10 copies it to the halted chip's RAM.

S10 port: --nvram also attaches the board's nvram.txt, processed the way Linux
DHD processes it (scripts/package-boot.py process_nvram), right after the
firmware; the descriptor becomes B2LFW002 and b2l_wifi_diag=11 boots the
firmware with it. The NVRAM is not pinned by hash: the kernel checks its layout.

The result holds a personal address and vendor firmware: write it outside the
repository, never upload it. No phone I/O is done here.

  python3 scripts/attach-wifi-firmware.py --boot boot.img \
      --firmware /private/bcmdhd_sta.bin_b1 --mac-file /private/wifi-mac.txt \
      [--nvram /private/nvram.txt_CS01_semco_b1] --output /private/boot-wifi.img
"""
import argparse
import hashlib
import importlib.util
from pathlib import Path
import struct
import sys

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("package_boot", HERE / "package-boot.py")
package_boot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package_boot)


def attach(boot, firmware, mac, nvram=None):
    info = package_boot.check_boot(boot)
    if info["wifi_firmware_bytes"]:
        raise ValueError("the BOOT image already carries firmware")
    blob = firmware.read_bytes()
    if len(blob) != package_boot.WIFI_FIRMWARE_SIZE or \
            hashlib.sha256(blob).hexdigest() != package_boot.WIFI_FIRMWARE_SHA256:
        raise ValueError("not the pinned original station firmware")
    data = boot.read_bytes()
    trailer = info["trailer_file_offset"]
    _, _, _, size, offset = struct.unpack_from("<8sIIQQ", data, trailer)
    end = trailer + offset + size
    if end != len(data):
        raise ValueError("unexpected bytes after the Mach-O")
    board = package_boot.process_nvram(nvram.read_bytes()) if nvram else b""
    field = package_boot.wifi_metadata(mac, len(blob), len(board))
    return data[:trailer + 288] + field + data[trailer + 320:end] + blob + board


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--boot", type=Path, required=True, help="BOOT image from CI, without firmware")
    p.add_argument("--firmware", type=Path, required=True, help="private bcmdhd_sta.bin_b1")
    p.add_argument("--mac-file", type=Path, required=True, help="private file: six colon-separated octets")
    p.add_argument("--nvram", type=Path, help="private board nvram.txt, for b2l_wifi_diag=11")
    p.add_argument("--output", type=Path, required=True, help="private output image (must not exist)")
    a = p.parse_args()
    if a.output.exists():
        p.error("output exists; refusing to overwrite")
    try:
        image = attach(a.boot, a.firmware, package_boot.read_wifi_mac(a.mac_file), a.nvram)
        a.output.write_bytes(image)
        info = package_boot.check_boot(a.output)
    except ValueError as error:
        if a.output.exists():
            a.output.unlink()
        print(f"attach failed: {error}", file=sys.stderr)
        return 2
    print(f"boot_bytes={info['boot_bytes']} firmware_offset={info['wifi_firmware_file_offset']} "
          f"nvram_bytes={info['wifi_nvram_bytes']} "
          f"sha256={hashlib.sha256(a.output.read_bytes()).hexdigest()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
