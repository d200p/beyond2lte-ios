#!/usr/bin/env python3
"""Collect beyond2lte Wi-Fi/PCIe recovery evidence without writing the phone.

Does not enable Wi-Fi, issue PCI config transactions, mount volumes, read MAC
addresses, or invoke the controller's sysfs tests. Output stays private.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import os
from pathlib import Path
import re
import subprocess
import sys


class ProbeError(Exception):
    pass


class Recovery:
    def __init__(self, adb: str, serial: str | None):
        self.adb = adb
        self.serial = serial
        if not serial:
            rows = self.call('devices').decode().splitlines()[1:]
            devices = [row.split() for row in rows if row.strip()]
            if len(devices) != 1 or devices[0][1] != 'recovery':
                raise ProbeError('Connect exactly one phone in TWRP, or select it with --serial.')
            self.serial = devices[0][0]
        if self.call('get-state').strip() != b'recovery':
            raise ProbeError('Target must be in recovery.')
        product = self.shell('getprop ro.product.device').strip()
        if product not in (b'beyond2lte', b'beyond1lte'):
            raise ProbeError('Target is not a supported board (beyond2lte or beyond1lte).')
        self.product = product.decode()
        if not self.shell('getprop ro.twrp.version').strip():
            raise ProbeError('Target must run TWRP.')
        if self.shell('id -u').strip() != b'0':
            raise ProbeError('Recovery must provide root.')

    def call(self, *args: str) -> bytes:
        argv = [self.adb]
        if self.serial:
            argv += ['-s', self.serial]
        return subprocess.check_output(argv + list(args), timeout=30, stderr=subprocess.PIPE)

    def shell(self, command: str) -> bytes:
        return self.call('shell', '-T', command)


# These PCI sysfs attributes are cached by Linux. Deliberately exclude config,
# rom, resourceN, reset, rescan, enable writes and all network address files.
PCI_INVENTORY = r'''
for d in /sys/bus/pci/devices/*; do
    [ -d "$d" ] || continue
    printf 'DEVICE %s\n' "${d##*/}"
    for a in vendor device subsystem_vendor subsystem_device class revision enable resource power/runtime_status; do
        if [ -r "$d/$a" ]; then
            printf 'ATTRIBUTE %s\n' "$a"
            cat "$d/$a"
        fi
    done
    printf 'DRIVER\n'
    readlink "$d/driver" || true
done
'''

PLATFORM_INVENTORY = r'''
for d in /sys/bus/platform/devices/*pcie* /sys/bus/platform/devices/*wlan*; do
    [ -d "$d" ] || continue
    printf 'PLATFORM %s\n' "${d##*/}"
    printf 'DRIVER\n'
    readlink "$d/driver" || true
    if [ -r "$d/power/runtime_status" ]; then cat "$d/power/runtime_status"; fi
    if [ -f "$d/pcie_sysfs" ]; then printf 'pcie_sysfs test interface present (not invoked)\n'; fi
done
'''

GPIO_INVENTORY = r'''
for d in /sys/class/sec/wlan/WLAN_REG_ON /sys/devices/virtual/sec/wlan/WLAN_REG_ON; do
    [ -d "$d" ] || continue
    printf 'WLAN_REG_ON\n'
    readlink -f "$d"
    for a in direction value; do
        if [ -r "$d/$a" ]; then printf '%s=' "$a"; cat "$d/$a"; fi
    done
    break
done
'''


def pci_ids(text: str) -> list[dict[str, str]]:
    result: list[dict[str, str]] = []
    device: dict[str, str] | None = None
    attribute = None
    fields = {'vendor', 'device', 'subsystem_vendor', 'subsystem_device', 'class', 'revision'}
    for line in text.splitlines():
        if line.startswith('DEVICE '):
            bdf = line.removeprefix('DEVICE ')
            if not re.fullmatch(r'[0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-7]', bdf):
                raise ProbeError('Unexpected PCI address in sysfs inventory.')
            device = {'bdf': bdf}
            result.append(device)
            attribute = None
        elif line.startswith('ATTRIBUTE '):
            attribute = line.removeprefix('ATTRIBUTE ')
        elif device is not None and attribute in fields:
            if not re.fullmatch(r'0x[0-9a-fA-F]{2,8}', line):
                raise ProbeError('Invalid PCI ID attribute.')
            device[attribute] = line.lower()
            attribute = None
        else:
            attribute = None
    return result


def collect(recovery: Recovery, output: Path) -> dict:
    output.mkdir(mode=0o700, parents=False, exist_ok=False)
    output.chmod(0o700)
    commands = {
        'pci.txt': PCI_INVENTORY,
        'platform.txt': PLATFORM_INVENTORY,
        'wlan-gpio.txt': GPIO_INVENTORY,
        'kernel.txt': 'uname -a',
    }
    records = {}
    for name, command in commands.items():
        data = recovery.shell(command)
        if len(data) > 1024 * 1024:
            raise ProbeError('Unexpectedly large diagnostic response.')
        path = output / name
        # Exclusive creation avoids replacing a pre-existing symlink or file.
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, 'wb') as stream:
            stream.write(data)
        records[name] = {'bytes': len(data)}
    devices = pci_ids((output / 'pci.txt').read_text())
    receipt = {
        'captured_at': dt.datetime.now(dt.timezone.utc).isoformat(),
        'product': getattr(recovery, 'product', 'unidentified'), 'device_writes': False,
        'pci_config_transactions': False, 'files': records, 'pci_devices': devices,
        'limitation': 'No Broadcom endpoint can mean the link is powered off; it does not prove missing hardware.',
    }
    fd = os.open(output / 'receipt.json', os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'w') as stream:
        json.dump(receipt, stream, indent=2)
        stream.write('\n')
    return receipt


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--adb', default='adb')
    parser.add_argument('--serial')
    parser.add_argument('--output', type=Path, required=True, help='New private directory; parent must exist.')
    args = parser.parse_args()
    try:
        recovery = Recovery(args.adb, args.serial)
        receipt = collect(recovery, args.output)
    except (ProbeError, OSError, subprocess.SubprocessError) as error:
        # Do not print raw ADB stderr, which can contain device identifiers.
        detail = str(error) if isinstance(error, ProbeError) else type(error).__name__
        print('Wi-Fi probe stopped: ' + detail, file=sys.stderr)
        return 1
    print(json.dumps({'output': str(args.output.resolve()), 'pci_devices': receipt['pci_devices'],
                      'device_writes': False}, indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main())
