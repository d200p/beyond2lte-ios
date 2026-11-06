#!/usr/bin/env python3
"""Merge the KDK and KDK_SDK payloads; retain their real headers and libraries."""
import pathlib
import subprocess
import sys
import xml.etree.ElementTree as ET

expanded, destination = map(pathlib.Path, sys.argv[1:])
roots = []
for info in sorted(expanded.glob('**/PackageInfo')):
    payload = info.parent / 'Payload'
    location = ET.parse(info).getroot().get('install-location', '')
    print('KDK package:', info.parent.name, 'install-location:', location)
    if payload.is_dir() and ('.kdk' in location or (payload / 'System').is_dir()):
        roots.append(payload)
roots.extend(sorted(expanded.glob('**/Payload/Library/Developer/KDKs/*.kdk')))
if not roots:
    print('Expanded package entries:', [str(p.relative_to(expanded)) for p in expanded.glob('*/*')][:40])
    raise SystemExit('KDK layout not recognized; inspect expanded package before continuing')
destination.mkdir(parents=True, exist_ok=True)
for root in roots:
    print('Merging KDK payload:', root)
    subprocess.run(['rsync', '-a', str(root) + '/', str(destination) + '/'], check=True)
private = destination / 'System/Library/Frameworks/Kernel.framework/Versions/A/PrivateHeaders'
if not private.is_dir():
    raise SystemExit('Merged KDK private headers are missing')
print('KDK merged; SDK header directories:', sorted(p.name for p in private.iterdir())[:40])
