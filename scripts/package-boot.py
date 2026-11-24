#!/usr/bin/env python3
"""Package external board UEFI + embedded XNU loader + a BOOT trailer.

No phone I/O or flashing is performed. The Android ramdisk is empty; XNU remains
on disk until the firmware loader reads its bounded trailer through UFS.
"""
import argparse
import hashlib
import json
import lzma
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tarfile
import uuid

FD_BASE = 0xC9000000
FD_SIZE = 0x400000
PAGE = 2048
MAGIC = b"B2LXNU02"
PAYLOAD_VERSION = 2
HEADER_SIZE = 320
SEED_SIZE = 256
# Board codename -> (retail model, root-model substring of its reference DTB).
DEVICES = {
    "beyond2lte": ("SM-G975F", "BEYOND2LTE"),
    "beyond1lte": ("SM-G973F", "BEYOND1LTE"),
}


def dtb_model(dtb):
    """Return the root 'model' property of a flattened device tree, or ''."""
    if len(dtb) < 40:
        return ""
    _, total, off_struct, off_strings, _, _, _, _, size_strings, size_struct = struct.unpack_from(">10I", dtb)
    if total > len(dtb):
        return ""
    strings = dtb[off_strings:off_strings + size_strings]
    pos, end, depth = off_struct, off_struct + size_struct, 0
    while pos + 4 <= end:
        token = struct.unpack_from(">I", dtb, pos)[0]
        pos += 4
        if token == 1:
            pos = (dtb.index(b"\0", pos) + 4) & ~3
            depth += 1
        elif token == 2:
            depth -= 1
        elif token == 3:
            length, nameoff = struct.unpack_from(">II", dtb, pos)
            pos += 8
            name = strings[nameoff:strings.find(b"\0", nameoff)]
            if depth == 1 and name == b"model":
                return dtb[pos:pos + length].rstrip(b"\0").decode(errors="replace")
            pos = (pos + length + 3) & ~3
        elif token == 9:
            break
        elif token != 4:
            return ""
    return ""
WIFI_FIRMWARE_SIZE = 1297036
WIFI_FIRMWARE_SHA256 = "1676f46ce56b96f58dc70de08beaab4ab3362ee6dd751465a8d6a0023c3c54ad"


def align(value, alignment=PAGE):
    return (value + alignment - 1) & -alignment


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_wifi_mac(path):
    """Read a private local file; never return its value in error text."""
    import re
    text = path.read_text().strip()
    if not re.fullmatch(r"(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}", text):
        raise ValueError("WiFi MAC file must contain exactly six colon-separated octets")
    mac = bytes.fromhex(text.replace(":", ""))
    if not any(mac) or mac[0] & 1:
        raise ValueError("WiFi MAC must be a nonzero unicast address")
    return mac


# S10 port: Wi-Fi mode 11 boots the firmware with the board NVRAM, which
# follows the firmware in the attachment (B2LFW002, uefi/XnuLoader/BootMetadata.h).
WIFI_NVRAM_MAX = 32768
NVRAM_LAYOUT_ERROR = "private NVRAM is not in the DHD-processed layout"
# S10 port: Wi-Fi mode 19 loads the board's CLM (regulatory) blob with DHD's
# "clmload"; it follows the NVRAM (B2LFW003). Only this blob is accepted.
WIFI_CLM_SIZE = 29464
WIFI_CLM_SHA256 = "5e5520cc655991923b7c88c85c2c027d22b0462dc6a3bfd8fd191cbefae33300"
WIFI_CLM_MAX = 65536
# S10 port: Wi-Fi mode 23 joins the network a fixed record after the CLM blob
# names (B2LFW004): "B2LJOIN1", the name's and passphrase's lengths, two zero
# bytes, the name in 32 bytes and the passphrase (or 64 hex digits) in 64,
# four zero bytes, then the first 16 bytes of the SHA-256 of the 112 before.
# Private input: nothing here ever prints a name, a passphrase or a length.
WIFI_JOIN_SIZE = 128
WIFI_JOIN_MAGIC = b"B2LJOIN1"


def wifi_join_record(ssid, password):
    if not 1 <= len(ssid) <= 32:
        raise ValueError("the network name must be 1 to 32 bytes")
    hex_key = len(password) == 64 and all(c in b"0123456789abcdefABCDEF" for c in password)
    if not hex_key and (not 8 <= len(password) <= 63 or any(c < 0x20 or c > 0x7e for c in password)):
        raise ValueError("the password must be a WPA2 passphrase (8 to 63 printable ASCII) or 64 hex digits")
    body = (WIFI_JOIN_MAGIC + bytes([len(ssid), len(password), 0, 0]) + ssid.ljust(32, b"\0") +
            password.ljust(64, b"\0") + bytes(4))
    return body + hashlib.sha256(body).digest()[:16]


def check_wifi_join(record):
    if len(record) != WIFI_JOIN_SIZE or record[:8] != WIFI_JOIN_MAGIC or \
            hashlib.sha256(record[:112]).digest()[:16] != record[112:]:
        raise ValueError("invalid private join record")
    ssid_len, password_len = record[8], record[9]
    if record[10:12] != bytes(2) or record[108:112] != bytes(4) or not 1 <= ssid_len <= 32 or \
            not 8 <= password_len <= 64 or any(record[12 + ssid_len:44]) or any(record[44 + password_len:108]):
        raise ValueError("invalid private join record")
    wifi_join_record(record[12:12 + ssid_len], record[44:44 + password_len])


def read_wifi_join(path):
    """(name, passphrase) from a private two-line file; never printed."""
    raw = path.read_bytes()
    if raw.startswith(b"\xef\xbb\xbf"):
        raw = raw[3:]
    lines = raw.replace(b"\r\n", b"\n").replace(b"\r", b"\n").split(b"\n")
    while lines and lines[-1] == b"":
        lines.pop()
    if len(lines) != 2:
        raise ValueError("the join file must hold two lines: the network name, then its password")
    wifi_join_record(lines[0], lines[1])
    return lines[0], lines[1]


def check_nvram(data):
    """The rule b2l_wifi_diag.c applies: printable name=value entries, each
    ended by one NUL, then NUL padding; whole words; the last word zero.
    Error text never includes the private content."""
    if len(data) < 8 or len(data) > WIFI_NVRAM_MAX or len(data) % 4 or any(data[-4:]):
        raise ValueError(NVRAM_LAYOUT_ERROR)
    body = data.rstrip(b"\0")
    if not body:
        raise ValueError(NVRAM_LAYOUT_ERROR)
    for entry in body.split(b"\0"):
        if not entry or any(c < 0x20 or c > 0x7e for c in entry) or entry.find(b"=") < 1:
            raise ValueError(NVRAM_LAYOUT_ERROR)
    return len(data)


def process_nvram(text):
    """What Linux DHD writes to the chip from an nvram.txt (bcmutils.c
    process_nvram_vars, then dhdpcie_download_nvram): carriage returns and
    '#' comments to the end of the line dropped, empty lines dropped, each
    line ended by a NUL; padded to a word, one more NUL, rounded to a word."""
    out = bytearray()
    find_newline = False
    column = 0
    for c in text:
        if c == 0x0d:
            continue
        if find_newline and c != 0x0a:
            continue
        find_newline = False
        if c == 0x23:
            find_newline = True
            continue
        if c == 0x0a:
            if column:
                out.append(0)
                column = 0
            continue
        out.append(c)
        column += 1
    out += bytes(-len(out) % 4)
    out.append(0)
    out += bytes(-len(out) % 4)
    check_nvram(bytes(out))
    return bytes(out)


def wifi_metadata(mac=None, firmware_size=0, nvram_size=0, clm_size=0, join_size=0):
    if mac is None:
        if firmware_size or nvram_size or clm_size or join_size:
            raise ValueError("private firmware requires WiFi metadata")
        return bytes(32)
    if len(mac) != 6 or not any(mac) or mac[0] & 1:
        raise ValueError("WiFi metadata requires six nonzero unicast address octets")
    if firmware_size not in (0, WIFI_FIRMWARE_SIZE):
        raise ValueError("unsupported private firmware length")
    if nvram_size and (not firmware_size or nvram_size > WIFI_NVRAM_MAX or nvram_size % 4):
        raise ValueError("unsupported private NVRAM length")
    if clm_size and (not nvram_size or clm_size > WIFI_CLM_MAX or clm_size % 4):
        raise ValueError("unsupported private CLM length")
    if join_size and (join_size != WIFI_JOIN_SIZE or not clm_size):
        raise ValueError("the join record follows the CLM blob and is 128 bytes")
    if clm_size:
        version = b"B2LFW004" if join_size else b"B2LFW003"
        extension = version + struct.pack("<IHHH", firmware_size, nvram_size, nvram_size ^ 0xffff, clm_size // 4)
    elif nvram_size:
        extension = b"B2LFW002" + struct.pack("<IHH", firmware_size, nvram_size, nvram_size ^ 0xffff) + bytes(2)
    elif firmware_size:
        extension = b"B2LFW001" + struct.pack("<I", firmware_size) + bytes(6)
    else:
        extension = bytes(18)
    return b"B2LWIFI1" + mac + extension


def wifi_attachment(field):
    """(firmware bytes, NVRAM bytes, CLM bytes, join record bytes) the descriptor declares."""
    extension = field[14:]
    if extension == bytes(18):
        return 0, 0, 0, 0
    if len(extension) != 18 or extension[:7] != b"B2LFW00" or extension[7:8] not in (b"1", b"2", b"3", b"4"):
        raise ValueError("invalid private firmware descriptor")
    size = struct.unpack_from("<I", extension, 8)[0]
    if size != WIFI_FIRMWARE_SIZE:
        raise ValueError("unsupported private firmware length")
    if extension[7:8] == b"1":
        if any(extension[12:]):
            raise ValueError("invalid private firmware descriptor")
        return size, 0, 0, 0
    nvram, check, clm_words = struct.unpack_from("<HHH", extension, 12)
    if not nvram or nvram ^ check != 0xffff or nvram > WIFI_NVRAM_MAX or nvram % 4:
        raise ValueError("invalid private firmware descriptor")
    if (extension[7:8] == b"2") != (clm_words == 0) or clm_words * 4 > WIFI_CLM_MAX:
        raise ValueError("invalid private firmware descriptor")
    return size, nvram, clm_words * 4, WIFI_JOIN_SIZE if extension[7:8] == b"4" else 0


def wifi_firmware_size(field):
    return wifi_attachment(field)[0]


def check_wifi_metadata(field):
    if field == bytes(32):
        return False
    if len(field) != 32 or field[:8] != b"B2LWIFI1":
        raise ValueError("invalid optional BOOT WiFi metadata")
    wifi_metadata(field[8:14], *wifi_attachment(field))
    return True


def make_payload_header(kernel_size, wifi_mac=None):
    """Supply the full XNU boot seed directly from the host operating system."""
    header = struct.pack("<8sIIQQ", MAGIC, PAYLOAD_VERSION, HEADER_SIZE, kernel_size, HEADER_SIZE)
    return header + os.urandom(SEED_SIZE) + wifi_metadata(wifi_mac)


def volume_contains_loader(data, loader):
    """Match the loader only within a bounded FV, including FV_IMAGE sections."""
    def match_fv(candidate):
        if len(candidate) < 56 or candidate[40:44] != b"_FVH":
            return False
        length = struct.unpack_from("<Q", candidate, 32)[0]
        header_length = struct.unpack_from("<H", candidate, 48)[0]
        if length > len(candidate) or header_length < 56 or header_length > length:
            return False
        return loader in candidate[:length]

    if match_fv(data):
        return True
    # GenFv wraps the FV in a section stream. Alignment RAW sections can precede
    # EFI_SECTION_FIRMWARE_VOLUME_IMAGE (0x17), so the FV need not start at zero.
    position = 0
    while position + 4 <= len(data):
        size = int.from_bytes(data[position:position + 3], "little")
        section_type = data[position + 3]
        header_size = 4
        if size == 0xffffff:
            if position + 8 > len(data):
                return False
            size = struct.unpack_from("<I", data, position + 4)[0]
            header_size = 8
        if size < header_size or size > len(data) - position:
            return False
        if section_type == 0x17 and match_fv(data[position + header_size:position + size]):
            return True
        position = align(position + size, 4)
    return False


def firmware_contains_loader(firmware, loader):
    """Inspect the real FD's guided LZMA section, not just an adjacent map file."""
    marker = uuid.UUID("EE4E5898-3914-4259-9D6E-DC7BD79403CF").bytes_le
    search = 0
    while (position := firmware.find(marker, search)) >= 0:
        search = position + len(marker)
        section = position - 4
        if section < 0 or section + 24 > len(firmware):
            continue
        size = int.from_bytes(firmware[section:section + 3], "little")
        data_offset = struct.unpack_from("<H", firmware, section + 20)[0]
        if firmware[section + 3] != 2 or data_offset < 24 or size <= data_offset or section + size > len(firmware):
            continue
        try:
            decoder = lzma.LZMADecompressor(format=lzma.FORMAT_ALONE)
            volume = decoder.decompress(firmware[section + data_offset:section + size], max_length=64 * 1024 * 1024)
        except lzma.LZMAError:
            continue
        if decoder.eof and volume_contains_loader(volume, loader):
            return True
    return False


def check_boot(path):
    """Validate container arithmetic independently of the pack command."""
    data = path.read_bytes()
    if len(data) < PAGE or data[:8] != b"ANDROID!":
        raise ValueError("missing Android BOOT header")
    kernel_size, _, ramdisk_size, _, second_size = struct.unpack_from("<5I", data, 8)
    page, version = struct.unpack_from("<2I", data, 36)
    if page != PAGE or version != 1 or ramdisk_size or second_size:
        raise ValueError("expected Android v1, 2048-byte pages and empty ramdisk/second")
    if struct.unpack_from("<I", data, 1644)[0] != 1648:
        raise ValueError("incorrect Android v1 header_size")
    trailer = PAGE + align(kernel_size)
    if trailer + HEADER_SIZE > len(data):
        raise ValueError("BOOT trailer is truncated")
    magic, payload_version, header_size, size, offset = struct.unpack_from("<8sIIQQ", data, trailer)
    if magic == b"B2LXNU01":
        raise ValueError("legacy BOOT trailer provides only 64 seed bytes; repackage v2 with 256 seed bytes")
    if (magic, payload_version, header_size, offset) != (MAGIC, PAYLOAD_VERSION, HEADER_SIZE, HEADER_SIZE):
        raise ValueError("invalid BOOT trailer header")
    if size < 4096 or size > 128 * 1024 * 1024 or trailer + offset + size > len(data):
        raise ValueError("invalid BOOT trailer size")
    wifi_present = check_wifi_metadata(data[trailer + 288:trailer + 320])
    firmware_size, nvram_size, clm_size, join_size = wifi_attachment(data[trailer + 288:trailer + 320])
    firmware_offset = trailer + offset + size
    if firmware_offset + firmware_size + nvram_size + clm_size + join_size != len(data):
        raise ValueError("invalid BOOT trailer size")
    firmware_end = firmware_offset + firmware_size
    if firmware_size and hashlib.sha256(data[firmware_offset:firmware_end]).hexdigest() != WIFI_FIRMWARE_SHA256:
        raise ValueError("private firmware SHA-256 mismatch")
    if nvram_size:
        check_nvram(data[firmware_end:firmware_end + nvram_size])
    clm_end = firmware_end + nvram_size + clm_size
    if clm_size and (clm_size != WIFI_CLM_SIZE or
                     hashlib.sha256(data[firmware_end + nvram_size:clm_end]).hexdigest() != WIFI_CLM_SHA256):
        raise ValueError("private CLM blob SHA-256 mismatch")
    if join_size:
        check_wifi_join(data[clm_end:clm_end + join_size])
    macho = trailer + offset
    if struct.unpack_from("<III", data, macho) != (0xfeedfacf, 0x0100000c, 0):
        raise ValueError("XNU must be a thin, plain ARM64 Mach-O image")
    return {"android_kernel_bytes": kernel_size, "trailer_file_offset": trailer,
            "xnu_bytes": size, "boot_bytes": len(data), "ramdisk_bytes": 0,
            "payload_version": PAYLOAD_VERSION, "seed_bytes": SEED_SIZE,
            "wifi_mac_present": wifi_present, "wifi_firmware_bytes": firmware_size,
            "wifi_firmware_file_offset": firmware_offset if firmware_size else None,
            "wifi_nvram_bytes": nvram_size, "wifi_clm_bytes": clm_size, "wifi_join_present": bool(join_size)}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--mu", type=Path, help="Disposable external Mu-Silicium build tree")
    p.add_argument("--firmware", type=Path, help="4 MiB FD rebuilt with XnuLoader embedded")
    p.add_argument("--kernel", type=Path, help="Plain ARM64 mach.development.b2l")
    p.add_argument("--loader", type=Path, help="XnuLoader.efi embedded in that FD")
    p.add_argument("--output-dir", type=Path)
    p.add_argument("--wifi-mac-file", type=Path, help="Private local six-octet WiFi MAC; keep file and personalized image outside Git")
    p.add_argument("--max-image-bytes", type=lambda x: int(x, 0), help="Explicit offline packaging size ceiling")
    p.add_argument("--boot-partition-bytes", type=lambda x: int(x, 0), help="Confirmed target BOOT capacity, if available")
    p.add_argument("--clang", default=shutil.which("clang"))
    p.add_argument("--objcopy", default=shutil.which("llvm-objcopy") or "/opt/homebrew/opt/llvm/bin/llvm-objcopy")
    p.add_argument("--verify", type=Path, help="Only validate an existing packaged image")
    p.add_argument("--device", choices=sorted(DEVICES), default="beyond2lte",
                   help="Target board: beyond2lte (S10+, default) or beyond1lte (S10)")
    a = p.parse_args()
    if a.verify:
        print(json.dumps(check_boot(a.verify), indent=2))
        return
    for name in ("mu", "firmware", "kernel", "loader", "output_dir", "max_image_bytes"):
        if getattr(a, name) is None:
            p.error(f"--{name.replace('_', '-')} is required when packaging")
    if a.max_image_bytes <= 0:
        p.error("size ceiling must be positive")
    fd = a.firmware.read_bytes()
    kernel = a.kernel.read_bytes()
    if len(fd) != FD_SIZE:
        p.error("firmware FD must be exactly 4 MiB; stack/framebuffer layout cannot be enlarged")
    if len(kernel) < 4096 or struct.unpack_from("<III", kernel) != (0xfeedfacf, 0x0100000c, 0):
        p.error("kernel must be a thin plain ARM64 Mach-O image")
    if not a.loader.is_file() or a.loader.read_bytes()[:2] != b"MZ":
        p.error("--loader must be the built ARM64 EFI application")
    if not firmware_contains_loader(fd, a.loader.read_bytes()):
        p.error("the supplied EFI loader is not embedded in this FD's compressed firmware volume; rebuild the XNU firmware profile first")
    device = a.device
    retail_model, dtb_marker = DEVICES[device]
    dtb_path = a.mu / f"Resources/DTBs/{device}.dtb"
    dtb = dtb_path.read_bytes()
    if len(dtb) < 40 or struct.unpack_from(">I", dtb)[0] != 0xd00dfeed:
        p.error(f"reference {device} DTB is invalid")
    if dtb_marker not in dtb_model(dtb).upper():
        p.error(f"reference DTB model {dtb_model(dtb)!r} is not a {device} board")
    a.output_dir.mkdir(parents=True, exist_ok=True)
    intermediates = a.output_dir / "components"
    intermediates.mkdir(exist_ok=True)
    shim_elf = intermediates / "BootShim.elf"
    shim_bin = intermediates / "BootShim.bin"
    subprocess.run([a.clang, "--target=aarch64-linux-gnu", "-c", str(a.mu / "BootShim/BootShim.S"),
                    "-o", str(shim_elf), "-DREQUIRES_KERNEL_HEADER=1", f"-DFD_BASE={FD_BASE}",
                    f"-DFD_SIZE={FD_SIZE}"], check=True)
    subprocess.run([a.objcopy, "-O", "binary", str(shim_elf), str(shim_bin)], check=True)
    shim = shim_bin.read_bytes()
    if len(shim) % 16 or shim[56:60] != b"ARM\x64" or struct.unpack_from("<QQ", shim, 8) != (FD_BASE, FD_SIZE):
        p.error("unexpected BootShim layout")
    android_kernel = intermediates / "uefi-kernel.bin"
    android_kernel.write_bytes(shim + fd + dtb)
    empty_ramdisk = intermediates / "empty-ramdisk"
    empty_ramdisk.write_bytes(b"")
    image = a.output_dir / "boot.img"
    subprocess.run([sys.executable, str(a.mu / "Resources/Scripts/mkbootimg.py"),
                    "--kernel", str(android_kernel), "--ramdisk", str(empty_ramdisk),
                    "--header_version", "1", "--pagesize", str(PAGE), "--board", device,
                    "--output", str(image)], check=True)
    boot = image.read_bytes()
    expected_trailer = PAGE + align(len(shim) + len(fd) + len(dtb))
    if len(boot) != expected_trailer:
        p.error("unexpected Android BOOT component padding")
    wifi_mac = read_wifi_mac(a.wifi_mac_file) if a.wifi_mac_file else None
    header = make_payload_header(len(kernel), wifi_mac)
    final_size = len(boot) + len(header) + len(kernel)
    limit = min(a.max_image_bytes, a.boot_partition_bytes or a.max_image_bytes)
    if final_size > limit:
        image.unlink()
        p.error(f"BOOT image needs {final_size} bytes, above explicit ceiling {limit}")
    with image.open("ab") as out:
        out.write(header)
        out.write(kernel)
    layout = check_boot(image)
    manifest = {
        "format": "beyond2lte-xnu-boot-v2", "device": device, "model": retail_model,
        "status": "offline-built; not device-boot-verified", "layout": layout,
        "firmware_base": hex(FD_BASE), "firmware_bytes": FD_SIZE,
        "confirmed_target_boot_partition_bytes": a.boot_partition_bytes,
        "offline_size_ceiling_bytes": a.max_image_bytes,
        "boot_sha256": sha256(image), "kernel_sha256": sha256(a.kernel),
        "firmware_sha256": sha256(a.firmware), "loader_sha256": sha256(a.loader),
        "embedded_loader_verified": True,
        "storage_writes_performed": False,
        "boot_path": "Samsung S-Boot -> BootShim -> external Mu UEFI -> embedded XnuLoader -> BOOT trailer Mach-O",
        "notes": ["Android ramdisk and second fields are empty; S-Boot does not load the XNU trailer.",
                  "Firmware UFS/BlockIo reads the trailer from the BOOT partition.",
                  "The host seed changes per packaged image, not per reboot; firmware RNG is preferred.",
                  "Verify actual device identity and BOOT capacity, and obtain approval before writing."]}
    (a.output_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    archive = a.output_dir / f"{device}-xnu-boot.tar"
    with tarfile.open(archive, "w", format=tarfile.USTAR_FORMAT) as tar:
        for name in ("boot.img", "manifest.json"):
            tar.add(a.output_dir / name, arcname=name)
    print(json.dumps(manifest, indent=2))
    print(f"Created {archive}; no device write was performed")


if __name__ == "__main__":
    main()
