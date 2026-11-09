#!/usr/bin/env python3
"""Exercise actual Wi-Fi diagnostic code, MMIO boundaries and recovery guards."""
import importlib.util
import os
from pathlib import Path
import shlex
import stat
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('wifi_probe', REPO / 'scripts/probe-b2l-wifi.py')
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


class RecoveryTests(unittest.TestCase):
    def test_pci_ids(self):
        data = ('DEVICE 0000:01:00.0\nATTRIBUTE vendor\n0x14e4\n'
                'ATTRIBUTE device\n0x4475\nATTRIBUTE revision\n0x01\n'
                'ATTRIBUTE resource\n00000000 00000000 00000000\nDRIVER\n../bcmdhd\n')
        self.assertEqual(probe.pci_ids(data), [{'bdf': '0000:01:00.0', 'vendor': '0x14e4',
                                              'device': '0x4475', 'revision': '0x01'}])
        self.assertEqual(probe.pci_ids(''), [])
        for bad in ('DEVICE ../../other\n', 'DEVICE 0000:01:00.0\nATTRIBUTE vendor\ngarbage\n'):
            with self.assertRaises(probe.ProbeError):
                probe.pci_ids(bad)

    def test_identity_fails_before_hardware_reads(self):
        class Fake(probe.Recovery):
            def call(self, *args):
                calls.append(args)
                answers = {
                    ('devices',): b'List of devices attached\nTEST\trecovery\n',
                    ('get-state',): b'recovery\n',
                    ('shell', '-T', 'getprop ro.product.device'): b'otherphone\n',
                }
                return answers[args]
        calls = []
        with self.assertRaisesRegex(probe.ProbeError, 'not beyond2lte'):
            Fake('adb', None)
        self.assertEqual(len(calls), 3)
        self.assertFalse(any('sys/' in str(call) for call in calls))

    def test_ambiguous_or_nonrecovery_device(self):
        class Fake(probe.Recovery):
            def call(self, *args):
                self.outer.assertEqual(args, ('devices',))
                return response
        Fake.outer = self
        for response in (b'List of devices attached\n',
                         b'List of devices attached\nA\tdevice\n',
                         b'List of devices attached\nA\trecovery\nB\trecovery\n'):
            with self.assertRaises(probe.ProbeError):
                Fake('adb', None)

    def test_private_capture_and_no_overwrite(self):
        class Fake:
            def shell(self, command):
                commands.append(command)
                return b''
        commands = []
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / 'capture'
            receipt = probe.collect(Fake(), output)
            self.assertFalse(receipt['device_writes'])
            self.assertFalse(receipt['pci_config_transactions'])
            self.assertEqual(stat.S_IMODE(output.stat().st_mode), 0o700)
            for file in output.iterdir():
                self.assertEqual(stat.S_IMODE(file.stat().st_mode), 0o600)
            self.assertEqual(commands, [probe.PCI_INVENTORY, probe.PLATFORM_INVENTORY,
                                        probe.GPIO_INVENTORY, 'uname -a'])
            with self.assertRaises(FileExistsError):
                probe.collect(Fake(), output)


def test_kernel():
    source = (REPO / 'xnu-overlay/osfmk/arm64/b2l_wifi_diag.c').read_text()
    source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
    original = 'return *(volatile const uint32_t *)(region->mapped + offset);'
    if source.count(original) != 1:
        raise RuntimeError('Diagnostic MMIO accessor changed; update boundary fixture.')
    source = source.replace(original, 'return host_read(region->mapped, offset);')
    pre = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
typedef uintptr_t vm_offset_t;
typedef unsigned long vm_size_t;
#define VM_WIMG_IO 0x66
static _Alignas(16384) uint32_t memory[4][4096];
static const uintptr_t physical[] = {0x15860000, 0x10430000, 0x13c40000, 0x13c00000};
static unsigned maps, unmapped, reads, fail_map;
static int mode;
static char logs[4096];
static unsigned log_size;
static void b2l_early_trace(const char *fmt, ...) {
    va_list args; va_start(args, fmt);
    int n = vsnprintf(logs + log_size, sizeof(logs) - log_size, fmt, args);
    va_end(args); assert(n >= 0 && (unsigned)n < sizeof(logs) - log_size);
    log_size += (unsigned)n;
}
static int PE_parse_boot_argn(const char *name, void *value, unsigned size) {
    assert(!strcmp(name, "b2l_wifi_diag") && size == 4);
    if (mode == -1) return 0;
    *(uint32_t *)value = (uint32_t)mode; return 1;
}
static uintptr_t ml_io_map_unmappable(uintptr_t address, unsigned long size, unsigned flags) {
    assert(maps < 4 && address == physical[maps] && size == 0x4000 && flags == VM_WIMG_IO);
    unsigned i = maps++;
    return maps == fail_map ? 0 : (uintptr_t)memory[i];
}
static void ml_io_unmap(uintptr_t address, unsigned long size) {
    unsigned successful = maps - (fail_map && fail_map <= maps ? 1 : 0);
    assert(unmapped < successful && size == 0x4000);
    assert(address == (uintptr_t)memory[successful - ++unmapped]);
}
static uint32_t host_read(uintptr_t address, unsigned offset) {
    assert(maps == 4 && !fail_map && !unmapped && !(offset & 3));
    for (unsigned i = 0; i < 4; i++) if (address == (uintptr_t)memory[i]) {
        int allowed = (i == 0 && offset == 0x71c) ||
            (i == 1 && (offset == 0x80 || offset == 0x84)) ||
            (i == 2 && (offset == 0x20 || offset == 0x24)) ||
            (i == 3 && ((offset >= 0x2030 && offset <= 0x2044) ||
                        (offset >= 0x3034 && offset <= 0x3044)));
        assert(allowed); reads++; return memory[i][offset / 4];
    }
    assert(0); return 0;
}
'''
    post = r'''
static void reset(int requested, unsigned fail) {
    mode = requested; fail_map = fail; maps = unmapped = reads = log_size = 0;
    memset(memory, 0, sizeof(memory)); memset(logs, 0, sizeof(logs));
}
int main(void) {
    const int disabled[] = {-1, 0, 2, 3, -2};
    for (unsigned i = 0; i < sizeof(disabled)/sizeof(disabled[0]); i++) {
        reset(disabled[i], 0); b2l_wifi_diag_start();
        assert(!maps && !reads && !unmapped && strstr(logs, "disabled"));
    }
    for (unsigned i = 1; i <= 4; i++) {
        reset(1, i); b2l_wifi_diag_start();
        assert(maps == i && unmapped == i - 1 && !reads && strstr(logs, "map failed"));
    }
    reset(1, 0);
    memory[0][0x71c/4] = 1;
    memory[1][0x80/4] = 1u << 20; memory[1][0x84/4] = 1u << 5;
    memory[2][0x20/4] = (1u << 8) | 2u; memory[2][0x24/4] = 5;
    uint32_t before[4][4096]; memcpy(before, memory, sizeof(before));
    b2l_wifi_diag_start();
    assert(maps == 4 && unmapped == 4 && reads == 16 && !memcmp(before, memory, sizeof(before)));
    assert(strstr(logs, "isolation_bypass=1; WLAN_REG_ON function=1 level=1"));
    assert(strstr(logs, "PERST function=1 level=1; CLKREQ function=2 level=1"));
    assert(strstr(logs, "Wi-Fi remain unprobed"));
    reset(1, 0); memory[0][0x71c/4] = UINT32_MAX; b2l_wifi_diag_start();
    assert(reads == 5 && unmapped == 4 && strstr(logs, "undecoded"));
    assert(!strstr(logs, "isolation_bypass="));
    reset(1, 0); b2l_wifi_diag_start();
    assert(strstr(logs, "isolation_bypass=0") && reads == 16 && unmapped == 4);
    puts("Wi-Fi snapshot: no controller access, no mutations, mode gates and mapping cleanup passed");
}
'''
    with tempfile.TemporaryDirectory(prefix='b2l-wifi-') as temporary:
        path = Path(temporary) / 'wifi.c'
        path.write_text(pre + source + post)
        binary = Path(temporary) / 'wifi-test'
        compiler = shlex.split(os.environ.get('CC', 'cc'))
        subprocess.run(compiler + ['-std=c11', '-Wall', '-Wextra', '-Werror',
                                   '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                                   str(path), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    test_kernel()
    unittest.main()
