#!/usr/bin/env python3
"""Extract DRAM and reserved-memory evidence from a flattened device tree.

Input is a binary DTB: a live dump taken in recovery (`/sys/firmware/fdt`) or a
reference DTB. The output uses the same JSON shape as
common/memory/beyond2lte-8g-dt.json: node path -> property -> raw big-endian
hex. Only `/`, `/memory@*` and `/reserved-memory/...` are kept.

The input may also be an evidence JSON produced earlier.

`--compare OTHER.json` reports whether the DRAM banks and every fixed or
dynamic reservation match another capture. With `--allow-extra`, reservations
present only in the input are listed but accepted (the S10 bootloader adds
`mm_debug`); changed or missing ones still fail. `--allowlist HEADER` fails if
any fixed reservation overlaps the loader/kernel RAM allowlist
(common/memory/b2l_ram_ranges.h).

No phone access is performed; pull the DTB yourself, for example:
    adb pull /sys/firmware/fdt s10-live.dtb
"""
import argparse
import json
from pathlib import Path
import re
import struct
import sys

FDT_MAGIC = 0xD00DFEED
KEEP = ("#address-cells", "#size-cells", "reg", "size", "alloc-ranges", "model")


def parse_fdt(data):
    """Yield (path, {prop: bytes}) for every node, in device-tree order."""
    if len(data) < 40:
        raise ValueError("file is too short to be a DTB")
    magic, total, off_struct, off_strings, _, version, _, _, size_strings, size_struct = struct.unpack_from(">10I", data)
    if magic != FDT_MAGIC:
        raise ValueError("missing FDT magic; pass the binary DTB, not decompiled DTS text")
    if total > len(data) or off_struct + size_struct > total or off_strings + size_strings > total or version < 17:
        raise ValueError("truncated or unsupported DTB")
    strings = data[off_strings:off_strings + size_strings]
    nodes, stack = [], []  # stack holds (name, props) of the open nodes
    pos, end = off_struct, off_struct + size_struct
    while pos + 4 <= end:
        token = struct.unpack_from(">I", data, pos)[0]
        pos += 4
        if token == 1:
            name_end = data.index(b"\0", pos, end)
            name = data[pos:name_end].decode()
            pos = (name_end + 4) & ~3
            props = {}
            stack.append((name, props))
            nodes.append(("/" + "/".join(n for n, _ in stack[1:]), props))
        elif token == 2:
            if not stack:
                raise ValueError("unbalanced END_NODE")
            stack.pop()
        elif token == 3:
            length, nameoff = struct.unpack_from(">II", data, pos)
            pos += 8
            if nameoff >= len(strings) or pos + length > end:
                raise ValueError("property outside the DTB blocks")
            if not stack:
                raise ValueError("property outside a node")
            name = strings[nameoff:strings.index(b"\0", nameoff)].decode()
            stack[-1][1][name] = data[pos:pos + length]
            pos = (pos + length + 3) & ~3
        elif token == 4:
            continue
        elif token == 9:
            break
        else:
            raise ValueError(f"unexpected FDT token {token}")
    if stack:
        raise ValueError("unterminated node")
    return nodes


def evidence(nodes):
    out = {}
    for path, props in nodes:
        if not (path == "/" or path == "/reserved-memory" or path.startswith("/reserved-memory/")
                or path.startswith("/memory@")):
            continue
        kept = {}
        for name in KEEP:
            if name in props:
                kept[name] = props[name].hex() if name != "model" else props[name].rstrip(b"\0").decode(errors="replace")
        if path != "/" and "model" in kept:
            del kept["model"]
        out[path] = kept
    return out


def ranges(doc):
    """Normalise to comparable sets of (kind, name, values) ignoring node-name case."""
    banks, reserved = {}, {}
    for path, props in doc.items():
        if path.startswith("/memory@") and "reg" in props:
            banks[path.lower()] = props["reg"].lower()
        elif path.startswith("/reserved-memory/"):
            values = {k: v.lower() for k, v in props.items() if k in ("reg", "size", "alloc-ranges")}
            if values:
                reserved[path.lower()] = values
    return banks, reserved


def compare(left, right, allow_extra=False):
    """Return (problems, extras, bank count, reservation count) for left vs right."""
    problems, extras = [], []
    lb, lr = ranges(left)
    rb, rr = ranges(right)
    for name in sorted(set(lb) | set(rb)):
        if lb.get(name) != rb.get(name):
            problems.append(f"DRAM bank {name}: {lb.get(name)} != {rb.get(name)}")
    for name in sorted(set(lr) | set(rr)):
        if lr.get(name) == rr.get(name):
            continue
        if allow_extra and name not in rr:
            extras.append(f"reservation {name}: {lr[name]} (only in input)")
        else:
            problems.append(f"reservation {name}: {lr.get(name)} != {rr.get(name)}")
    return problems, extras, len(lb), len(lr)


def allowlist_overlaps(doc, header_text):
    """Fixed reservations that intersect any allowlisted RAM range."""
    allow = [(int(b, 16), int(s, 16)) for b, s in
             re.findall(r"\{\s*(0x[0-9a-fA-F]+)ULL\s*,\s*(0x[0-9a-fA-F]+)ULL\s*\}", header_text)]
    if not allow:
        raise ValueError("no allowlist ranges found in header")
    hits = []
    for path, props in doc.items():
        if not path.startswith("/reserved-memory/") or "reg" not in props:
            continue
        raw = bytes.fromhex(props["reg"])
        if len(raw) != 12:
            raise ValueError(f"{path}: unexpected reg geometry")
        base, size = int.from_bytes(raw[:8], "big"), int.from_bytes(raw[8:], "big")
        for a, s in allow:
            if base < a + s and a < base + size:
                hits.append(f"{path} {base:#x}+{size:#x} overlaps allowlist {a:#x}+{s:#x}")
    return hits, len(allow)


def load(path):
    data = path.read_bytes()
    if data[:4] == struct.pack(">I", FDT_MAGIC):
        return evidence(parse_fdt(data))
    try:
        doc = json.loads(data)
    except ValueError:
        raise ValueError("input is neither a binary DTB nor evidence JSON")
    if not isinstance(doc, dict) or "/" not in doc:
        raise ValueError("evidence JSON lacks the root node")
    return doc


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("dtb", type=Path, help="Binary DTB (live /sys/firmware/fdt dump or reference DTB) or evidence JSON")
    p.add_argument("--output", type=Path, help="Write evidence JSON here")
    p.add_argument("--compare", type=Path, help="Evidence JSON to compare against")
    p.add_argument("--allow-extra", action="store_true", help="Accept reservations present only in the input")
    p.add_argument("--allowlist", type=Path, help="RAM allowlist header that no fixed reservation may overlap")
    p.add_argument("--expect-model", help="Substring the root model must contain, e.g. BEYOND1LTE")
    a = p.parse_args()
    try:
        doc = load(a.dtb)
    except (ValueError, OSError) as error:
        p.error(str(error))
    model = doc.get("/", {}).get("model", "")
    print(f"model: {model or '(none)'}")
    if a.expect_model and a.expect_model.upper() not in model.upper():
        p.error(f"root model does not contain {a.expect_model!r}")
    if a.output:
        a.output.write_text(json.dumps(doc, indent=2) + "\n")
        print(f"wrote {a.output}")
    failed = False
    if a.compare:
        problems, extras, banks, reserved = compare(doc, json.loads(a.compare.read_text()), a.allow_extra)
        print(f"compared {banks} DRAM banks and {reserved} reservations with {a.compare.name}")
        for line in extras:
            print("EXTRA", line)
        for line in problems:
            print("MISMATCH", line)
        failed |= bool(problems)
        if not problems:
            print("MATCH" + (" (extra reservations accepted)" if extras else ""))
    if a.allowlist:
        try:
            hits, count = allowlist_overlaps(doc, a.allowlist.read_text())
        except ValueError as error:
            p.error(str(error))
        for line in hits:
            print("OVERLAP", line)
        failed |= bool(hits)
        if not hits:
            print(f"ALLOWLIST OK: no fixed reservation overlaps the {count} RAM ranges of {a.allowlist.name}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
