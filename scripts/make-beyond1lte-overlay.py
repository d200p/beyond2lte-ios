#!/usr/bin/env python3
"""Derive a Galaxy S10 (beyond1lte) firmware package from the S10+ overlay.

The S10 (SM-G973F) and S10+ (SM-G975F) share the Exynos 9820 board design that
this port touches: GICv2 addresses, UFS, the sec_ts touch controller on HSI2C10
at 0x48, the MAX77705 on HSI2C45 at 0x66, the key GPIOs, the S-Boot handoff and
the live DRAM/reserved-memory map. The S10+ overlay package (beyond2ltePkg) was
itself derived from Mu-Silicium's beyond1ltePkg.

This script copies the applied S10+ package inside a *disposable* Mu-Silicium
work tree to Platforms/Samsung/beyond1ltePkg, renames it, and keeps
Mu-Silicium's own S10 device tree (Resources/DTBs/beyond1lte.dtb). It replaces
Mu-Silicium's stock S10 package, which lacks the UFS, touch and memory-map work.

It never accesses a phone and never writes outside the given work tree.
"""
import argparse
from pathlib import Path
import re
import shutil
import struct
import sys
import uuid

SOURCE = "beyond2lte"
TARGET = "beyond1lte"
TEXT_SUFFIXES = {".dsc", ".fdf", ".dec", ".inc", ".inf", ".c", ".h", ".S", ".s",
                 ".asm", ".py", ".md", ".txt", ".toml", ".uni", ".vfr"}
# Codename renames apply everywhere, because package paths and module names
# depend on them. Comments keep their S10+ measurements (model numbers, firmware
# builds) unchanged; only the codename inside them is mechanically renamed.
REPLACEMENTS = (
    ("beyond2ltePkg", "beyond1ltePkg"),
    ("beyond2lte", "beyond1lte"),
    ("BEYOND2LTE", "BEYOND1LTE"),
)
# Marketing names are changed only in the SMBIOS PCDs the firmware reports.
SMBIOS_REPLACEMENTS = (
    ("Galaxy_S10+_", "Galaxy_S10_"),
    ("Galaxy S10+", "Galaxy S10"),
)
# Fallback identities when Mu-Silicium's stock S10 package is absent.
NAMESPACE = uuid.UUID("2f1b0c8e-5a49-4b8e-9d55-1c3a7e0b9a10")
FDT_MAGIC = 0xD00DFEED


class OverlayError(Exception):
    pass


def rename(text):
    for old, new in REPLACEMENTS:
        text = text.replace(old, new)
    return text


def rename_smbios(text):
    lines = []
    for line in text.splitlines(True):
        if "PcdSmbios" in line and not line.lstrip().startswith("#"):
            for old, new in SMBIOS_REPLACEMENTS:
                line = line.replace(old, new)
        lines.append(line)
    return "".join(lines)


def read_define(path, key):
    if not path.is_file():
        return None
    match = re.search(rf"^\s*{key}\s*=\s*([0-9A-Fa-f-]{{36}})\s*$", path.read_text(errors="replace"), re.MULTILINE)
    return match.group(1).upper() if match else None


def set_define(text, key, value):
    text, count = re.subn(rf"^(\s*{key}\s*=\s*)[0-9A-Fa-f-]{{36}}(\s*)$",
                          lambda m: m.group(1) + value + m.group(2), text, flags=re.MULTILINE)
    if count != 1:
        raise OverlayError(f"expected exactly one {key}")
    return text


def dtb_model(path):
    """Return the root 'model' string of a flattened device tree."""
    data = path.read_bytes()
    if len(data) < 40:
        raise OverlayError(f"{path.name} is too short to be a DTB")
    magic, total, off_struct, off_strings, _, _, _, _, size_strings, size_struct = struct.unpack_from(">10I", data)
    if magic != FDT_MAGIC or total > len(data):
        raise OverlayError(f"{path.name} is not a valid DTB")
    strings = data[off_strings:off_strings + size_strings]
    pos, end, depth = off_struct, off_struct + size_struct, 0
    while pos + 4 <= end:
        token = struct.unpack_from(">I", data, pos)[0]
        pos += 4
        if token == 1:  # BEGIN_NODE
            name_end = data.index(b"\0", pos)
            pos = (name_end + 4) & ~3
            depth += 1
        elif token == 2:  # END_NODE
            depth -= 1
        elif token == 3:  # PROP
            length, nameoff = struct.unpack_from(">II", data, pos)
            pos += 8
            name = strings[nameoff:strings.index(b"\0", nameoff)].decode()
            if depth == 1 and name == "model":
                return data[pos:pos + length].rstrip(b"\0").decode(errors="replace")
            pos = (pos + length + 3) & ~3
        elif token == 4:  # NOP
            continue
        elif token == 9:  # END
            break
        else:
            raise OverlayError(f"{path.name}: unexpected FDT token {token}")
    raise OverlayError(f"{path.name} has no root model")


def generate(root):
    samsung = root / "Platforms/Samsung"
    source = samsung / f"{SOURCE}Pkg"
    target = samsung / f"{TARGET}Pkg"
    configs = root / "Resources/Configs"
    dtb = root / f"Resources/DTBs/{TARGET}.dtb"

    if not (root / "Mu_Basecore").is_dir():
        raise OverlayError("--mu must be a disposable Mu-Silicium work tree (Mu_Basecore missing)")
    for required in (source / f"{SOURCE}.dsc", source / f"{SOURCE}.fdf", source / f"{SOURCE}.dec",
                     source / "Library/MemoryMapLib/MemoryMapLib.c", configs / f"{SOURCE}.toml", dtb):
        if not required.is_file():
            raise OverlayError(f"missing input: {required.relative_to(root)}")
    model = dtb_model(dtb)
    if "BEYOND1LTE" not in model.upper():
        raise OverlayError(f"{dtb.name} describes {model!r}, not a Galaxy S10 (BEYOND1LTE)")

    # Keep the stock S10 platform identity when Mu-Silicium provides it.
    platform_guid = read_define(target / f"{TARGET}.dsc", "PLATFORM_GUID") or \
        str(uuid.uuid5(NAMESPACE, "beyond1lte-platform")).upper()
    package_guid = str(uuid.uuid5(NAMESPACE, "beyond1lte-package")).upper()
    if package_guid == read_define(source / f"{SOURCE}.dec", "PACKAGE_GUID"):
        raise OverlayError("derived package GUID collides with the S10+ package")

    if target.exists():
        shutil.rmtree(target)
    shutil.copytree(source, target)
    for path in sorted(target.rglob("*"), key=lambda p: len(p.parts), reverse=True):
        if SOURCE in path.name:
            path.rename(path.with_name(path.name.replace(SOURCE, TARGET)))
    changed = 0
    for path in target.rglob("*"):
        if not path.is_file() or path.suffix not in TEXT_SUFFIXES:
            continue
        original = path.read_text(errors="surrogateescape")
        text = rename(original)
        if path.name == f"{TARGET}.dsc":
            text = set_define(rename_smbios(text), "PLATFORM_GUID", platform_guid)
        elif path.name == f"{TARGET}.dec":
            text = set_define(text, "PACKAGE_GUID", package_guid)
        if text != original:
            path.write_text(text, errors="surrogateescape")
            changed += 1
    (configs / f"{TARGET}.toml").write_text(rename((configs / f"{SOURCE}.toml").read_text()))

    leftovers = [str(p.relative_to(root)) for p in target.rglob("*")
                 if p.is_file() and p.suffix in TEXT_SUFFIXES
                 and re.search(r"beyond2lte", p.read_text(errors="replace"), re.IGNORECASE)]
    if leftovers:
        raise OverlayError("S10+ codename remains in: " + ", ".join(leftovers))
    dsc = (target / f"{TARGET}.dsc").read_text()
    if re.search(rf"MemoryMapLib\s*\|\s*{TARGET}Pkg/Library/MemoryMapLib/MemoryMapLib\.inf", dsc) is None:
        raise OverlayError("generated DSC lacks the beyond1lte MemoryMapLib binding")
    if re.search(r'PcdSmbiosSystemModel\|"Galaxy S10"', dsc) is None:
        raise OverlayError("generated DSC does not report the Galaxy S10 SMBIOS model")
    (target / "GENERATED-FROM.txt").write_text(
        f"Generated by beyond1lte-ios scripts/make-beyond1lte-overlay.py from {SOURCE}Pkg.\n"
        f"Device tree: Resources/DTBs/{TARGET}.dtb ({model}).\n"
        "The codename was renamed mechanically, including inside comments. Measurements\n"
        "quoted in comments (model numbers, firmware builds, timings) were taken on the\n"
        "S10+ and have not been repeated on an S10.\n"
        "Do not edit: regenerate from the S10+ overlay instead.\n")
    return model, changed


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--mu", type=Path, required=True,
                        help="Disposable Mu-Silicium work tree with the S10+ overlay already applied")
    args = parser.parse_args()
    try:
        model, changed = generate(args.mu.resolve())
    except OverlayError as error:
        parser.error(str(error))
    print(f"Generated Platforms/Samsung/{TARGET}Pkg from {SOURCE}Pkg ({changed} files renamed in place)")
    print(f"Device tree: {model}")


if __name__ == "__main__":
    sys.exit(main())
