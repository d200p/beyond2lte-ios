#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Disable launchd jobs whose hardware this port does not provide.

These daemons exit immediately on the S10+ port and KeepAlive/on-demand launch
restarts them every ~10 s for the whole boot (0105: about 50 launches each, plus
an xpcproxy, a dyld start and a 32 MiB shared-region unnest per launch):

  com.apple.bluetoothd     no Bluetooth controller driver
  com.apple.wifip2pd       no WiFi driver
  com.apple.fairplayd.H2   no secure enclave; exits with status 190

The script writes launchd's own override file,
/private/var/db/com.apple.xpc.launchd/disabled.plist, exactly what
`launchctl disable system/<label>` records. Apple job definitions, the launchd
cache and executables are not modified. Other keys in an existing file are kept.
Run as root on the mounted root image with ownership enabled; --dry-run first,
--verify afterwards, --undo to re-enable the jobs.
"""
import argparse
import os
import pathlib
import plistlib
import sys
import tempfile

JOBS = {
    'com.apple.bluetoothd': 'no Bluetooth controller driver',
    'com.apple.wifip2pd': 'no WiFi driver',
    'com.apple.fairplayd.H2': 'no secure enclave (exit 190)',
}
RELATIVE = 'private/var/db/com.apple.xpc.launchd/disabled.plist'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('root', type=pathlib.Path, help='mounted iOS root (ownership enabled)')
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument('--dry-run', action='store_true')
    mode.add_argument('--verify', action='store_true')
    mode.add_argument('--undo', action='store_true')
    a = ap.parse_args()
    launchd = a.root / 'System/Library/xpc/launchd.plist'
    if not launchd.is_file():
        sys.exit(f'not an iOS root: {a.root}')
    labels = {job.get('Label') for jobs in plistlib.load(open(launchd, 'rb')).values()
              if isinstance(jobs, dict) for job in jobs.values() if isinstance(job, dict)}
    missing = [j for j in JOBS if j not in labels]
    if missing:
        sys.exit(f'labels not in this root\'s launchd cache: {missing}')
    path = a.root / RELATIVE
    current = plistlib.load(open(path, 'rb')) if path.exists() else {}
    if not isinstance(current, dict):
        sys.exit(f'{RELATIVE} is not a dictionary')
    wanted = dict(current)
    for job in JOBS:
        if a.undo:
            wanted.pop(job, None)
        else:
            wanted[job] = True
    if a.verify:
        bad = [j for j in JOBS if current.get(j) is not True]
        st = path.stat() if path.exists() else None
        if bad or not st or (st.st_uid, st.st_gid) != (0, 0):
            sys.exit(f'not disabled: {bad or "ownership/missing"}')
        print(f'verified: {", ".join(JOBS)} disabled in {RELATIVE}')
        return
    for job, why in JOBS.items():
        print(f'{"enable " if a.undo else "disable"} {job:28s} {why}')
    if a.dry_run:
        print(f'dry run: would write {RELATIVE} with {len(wanted)} key(s)')
        return
    if os.geteuid() != 0:
        sys.exit('run as root (sudo) so the file is owned by root')
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temp = tempfile.mkstemp(prefix='.b2l-', dir=path.parent)
    try:
        with os.fdopen(fd, 'wb') as f:
            plistlib.dump(wanted, f, fmt=plistlib.FMT_BINARY)
            f.flush()
            os.fsync(f.fileno())
        os.chown(temp, 0, 0)
        os.chmod(temp, 0o644)
        os.replace(temp, path)
    finally:
        if os.path.exists(temp):
            os.unlink(temp)
    print(f'wrote {RELATIVE}')


if __name__ == '__main__':
    main()
