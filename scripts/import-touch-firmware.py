#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Import only the touch pieces of the reference UEFI port's touch branch.

The port's master B2LTouchDxe neither powers the sec_ts controller (PMIC LDO19
DVDD, LDO27 AVDD) nor installs gB2LTouchDiagnosticProtocolGuid, so XnuLoader's
handoff reports EFI_NOT_FOUND and XNU sees a silent controller. The touch
branch's driver does both. Its USB/MAX77705 memory windows and test
applications are deliberately not imported. Idempotent; --check only verifies.
"""
import argparse
import pathlib
import re
import subprocess
import sys

FILES = [
    'Platforms/Samsung/beyond2ltePkg/Drivers/B2LTouchDxe/B2LHsI2c.c',
    'Platforms/Samsung/beyond2ltePkg/Drivers/B2LTouchDxe/B2LHsI2c.h',
    'Platforms/Samsung/beyond2ltePkg/Drivers/B2LTouchDxe/B2LTouchDxe.c',
    'Platforms/Samsung/beyond2ltePkg/Drivers/B2LTouchDxe/B2LTouchDxe.inf',
    'Platforms/Samsung/beyond2ltePkg/Include/Protocol/B2LTouchDiagnostic.h',
    'Platforms/Samsung/beyond2ltePkg/beyond2lte.dec',
    'Silicon/Samsung/S5E9820Pkg/Library/HsI2cLib/HsI2c.c',
]
APRIORI = 'Platforms/Samsung/beyond2ltePkg/Include/APRIORI.inc'
APRIORI_LINE = '  INF beyond2ltePkg/Drivers/B2LTouchDxe/B2LTouchDxe.inf'


def code_only(text):
    """Header contents without comments or whitespace, for a layout comparison."""
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    text = re.sub(r'//[^\n]*', '', text)
    return re.sub(r'\s+', '', text)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--uefi', type=pathlib.Path, required=True, help='reference beyond2lte-uefi clone')
    ap.add_argument('--commit', required=True, help='touch branch commit to import from')
    ap.add_argument('--mu', type=pathlib.Path, required=True, help='disposable Mu-Silicium tree')
    ap.add_argument('--loader-header', type=pathlib.Path, required=True, help='uefi/XnuLoader/TouchDiagnostic.h')
    ap.add_argument('--check', action='store_true', help='verify only, write nothing')
    a = ap.parse_args()
    if a.mu.name == 'UEFI_beyond2lte' or not (a.mu / 'Mu_Basecore').is_dir():
        ap.error('--mu must be a disposable Mu-Silicium tree')
    mismatches = []
    for rel in FILES:
        data = subprocess.run(['git', '-C', str(a.uefi), 'show', f'{a.commit}:mu-silicium-overlay/{rel}'],
                              capture_output=True, check=True).stdout
        target = a.mu / rel
        if rel.endswith('B2LTouchDiagnostic.h') and code_only(data.decode()) != code_only(a.loader_header.read_text()):
            sys.exit('touch branch protocol differs from the loader copy; refusing to import')
        if target.exists() and target.read_bytes() == data:
            continue
        mismatches.append(rel)
        if not a.check:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
    apriori = a.mu / APRIORI
    lines = apriori.read_text().splitlines(keepends=True)
    kept = [l for l in lines if l.rstrip('\n') != APRIORI_LINE]
    if kept != lines:
        mismatches.append(APRIORI + ' (B2LTouchDxe must not be force-loaded)')
        if not a.check:
            apriori.write_text(''.join(kept))
    if a.check:
        if mismatches:
            sys.exit('touch import incomplete:\n  ' + '\n  '.join(mismatches))
        print(f'touch firmware matches {a.commit}')
    else:
        print(f'touch firmware imported from {a.commit}: {len(mismatches)} change(s)')


if __name__ == '__main__':
    main()
