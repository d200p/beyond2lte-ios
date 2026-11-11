#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Write a 4096-byte vbmeta image with verification disabled (libavb flags=2).

Used once, together with TWRP, after the owner unlocks the bootloader. Header
layout follows libavb's AvbVBMetaImageHeader: version 1.0, algorithm NONE, no
descriptors or signature. No device access.
"""
import struct
import sys

out = sys.argv[1] if len(sys.argv) > 1 else sys.exit('usage: make-vbmeta-disabled.py OUTPUT')
h = struct.pack('!4sLLQQL', b'AVB0', 1, 0, 0, 0, 0)
h += struct.pack('!QQQQ', 0, 0, 0, 0) + struct.pack('!QQQQ', 0, 0, 0, 0) + struct.pack('!QQ', 0, 0)
h += struct.pack('!QLL', 0, 2, 0)
h += b'avbtool 1.2.0'.ljust(47, b'\0') + b'\0' + bytes(80)
assert len(h) == 256 and h[120:124] == b'\0\0\0\2'
with open(out, 'wb') as f:
    f.write(h + bytes(4096 - 256))
print(f'{out}: 4096 bytes, verification disabled')
