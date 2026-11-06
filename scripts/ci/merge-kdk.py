#!/usr/bin/env python3
"""Merge the KDK and KDK_SDK payloads; retain their real headers and libraries."""
import pathlib
import subprocess
import sys

expanded, destination = map(pathlib.Path, sys.argv[1:])
roots = sorted(expanded.glob('**/Payload/Library/Developer/KDKs/*.kdk'))
if not roots:
    raise SystemExit('KDK layout not recognized; inspect expanded package before continuing')
destination.mkdir(parents=True, exist_ok=True)
for root in roots:
    print('Merging KDK payload:', root)
    subprocess.run(['rsync', '-a', str(root) + '/', str(destination) + '/'], check=True)
private = destination / 'System/Library/Frameworks/Kernel.framework/Versions/A/PrivateHeaders'
if not private.is_dir():
    raise SystemExit('Merged KDK private headers are missing')
print('KDK merged; SDK header directories:', sorted(p.name for p in private.iterdir())[:40])
