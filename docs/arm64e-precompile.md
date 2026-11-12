# Offline ARM64e -> ARM64 precompilation

`tools/b2l-precompile.c` rewrites the convertible ARM64e instructions of a
prepared iOS root on the host, before the USERDATA image is flashed. It is the
static counterpart of the [observed-instruction cache](native-cache.md): the
device no longer has to trap, emulate, learn and re-apply every PAC/AUT/XPAC,
authenticated branch/return and LDAPR site one boot at a time.

The tool belongs to the explicitly selected stripped-pointer development ABI
([ARM64e runtime](arm64e-runtime.md)). It provides no pointer-authentication
security and does not make a binary native ARM64: files keep their ARM64e
`cputype`/`cpusubtype`, so the kernel still grades them through the enabled
compatibility service, and every instruction the converter rejects still enters
the existing emulator.

## What changes and what does not

The tool links `common/cache/b2l_cache_core.c` and calls the same
`b2l_nc_native()` converter that the kernel's learning workers and pre-init
apply path use. There is no second instruction table:

| Original | Native | Note |
| --- | --- | --- |
| PACIA/PACIB/PACDA/PACDB, AUTIA/AUTIB/AUTDA/AUTDB, the Z forms, XPACI/XPACD | NOP | identity in the stripped-pointer ABI |
| PACIASP/PACIBSP, AUTIASP/AUTIBSP, the AZ/BZ/1716 hint forms, XPACLRI | NOP | hint space; already a NOP on non-PAuth cores |
| RETAA/RETAB | RET | |
| BRAA/BRAB/BLRAA/BLRAB and the Z forms | BR/BLR Xn | `Rn == SP` is never converted |
| LDAPR/LDAPRB/LDAPRH | LDAR/LDARB/LDARH | stronger acquire ordering |

PACGA, LDRAA/LDRAB and every other instruction are counted but never changed.
PACGA has no single-instruction ARM64 equivalent that preserves its result; the
[0088 investigation](cache-performance-0088.md) shows it as the dominant
remaining trap family, and it stays a kernel-side problem.

Code words are selected with the kernel's own definition from
`b2l_apps_payload_compare.h`: a section flagged `S_ATTR_PURE_INSTRUCTIONS`
inside a segment whose initial protection includes execute. That covers
`__TEXT,__text`, `__stubs`, `__auth_stubs`, `__objc_stubs` and similar
sections, and leaves `__const`, `__cstring`, `__unwind_info`, `__eh_frame`,
all data segments, load commands, UUIDs, strings and code signatures untouched.
File sizes and inode identities are unchanged; hard links stay shared; original
access/modification times are restored after a write.

Shared-cache images are located through the main cache image table and each
section address is translated through exactly one executable mapping of the
owning cache or subcache file, as `b2l_cache_exclusion.h` does on the device.
Subcaches are opened through the main header's subcache array and their UUIDs
must match. `dyld` inside the cache is processed even though it is not a dylib
image. Fat files are processed slice by slice; only `CPU_SUBTYPE_ARM64E` slices
are touched.

dyld's cache builder moves every image's `__auth_stubs` and `__objc_stubs` into
shared stub regions and leaves the per-image section headers with size zero, so
image sections alone miss every `BRAA x16, x17` stub. The first 0102 boot showed
exactly that: the first trapped instruction was a stub branch and the trap rate
stayed near 380,000 per second. The tool therefore also scans the executable
cache mappings for bytes outside every image's executable segment. Each 4 KiB
page of such a gap is accepted only when every word is zero or one of ADRP,
ADD, LDR, B, BL, BR, BRK, NOP or an authenticated branch; cache header pages
fail that test and are reported as skipped gaps, accepted pages are converted
like any other code. Where the cache carries dyld's mapping-and-slide table,
only mappings flagged `DYLD_CACHE_MAPPING_TEXT_STUBS` are scanned, which is
dyld's own definition of those islands; older caches fall back to the page
test alone. `B2L_PRECOMPILE_DEBUG=1` lists each page decision on stderr. On the macOS 27 host cache this adds about 564,000 stub branches in 72
regions and skips 234 header pages.

`/usr/lib/system/libcorecrypto.dylib` `__TEXT,__text` is excluded by default,
matching the device's [integrity-measured code exclusion](native-cache.md).
Its stubs remain eligible, exactly as on the device. `--exclude-text PATH` adds
another image or root-relative path; `--no-default-exclusions` removes the
default. Further exclusions require evidence of self-measuring code.

## PACGA routines (`--pacga-trampolines`)

PACGA cannot be rewritten in place. With this option each shared-cache PACGA
becomes a branch to a keyless EL0 routine (`xnu-overlay/osfmk/arm64/b2l_pacga_keyless.h`)
placed in all-zero executable cache pages within branch range. XZR destinations
become NOP. SP operands, excluded sections and unreachable sites keep trapping,
and the kernel computes the same keyless function for them when booted with
`b2l_pacga_keyless=1`. A root built this way requires that kernel. See
[build 0102](build-0102.md) for the 0104 details and tests.

## Risk accepted relative to the runtime cache

The runtime cache rewrites only words that were actually executed and trapped.
The precompiler rewrites every matching word in a pure-instruction section by
linear sweep. Apple's ARM64 toolchain keeps literals out of code sections, and
the accepted encodings are narrow, so a data word that aliases one of them
inside an instruction section is unlikely but not impossible. A report-only run
with `--report` lists per-file counts for inspection before anything is written.

## Workflow

Build once on the host:

```sh
scripts/build-precompile.sh            # -> out/b2l-precompile
```

After [preparing the root](ios-userspace-root.md) and running
`scripts/prepare-ios-root.py`, with the HFS image still mounted read/write and
ownership enabled:

```sh
sudo out/b2l-precompile --report /private/local-precompile.json /path/to/mounted/root
sudo out/b2l-precompile --write --report /private/local-precompile.json /path/to/mounted/root
sudo out/b2l-precompile --verify /path/to/mounted/root
```

The first command only reports. `--write` reads every code section, converts it
in memory, writes it back with `pwrite(2)` only when something changed, syncs
the file and reads the written bytes back before counting them. Any parse error
in a file or cache group stops before that file or group is written. `--verify`
exits 2 when convertible words remain, 1 on any error and 0 when the tree is
fully converted. The walk does not follow symlinks and stays on the root's
device; pass `--no-xdev` to cross mount points. Then unmount, check HFS, hash
and flash the image as before.

Running the tool over the macOS 27 host cache in report mode as a format check
processed 82 cache files and 4115 images with no errors; about 4.9% of all
code words were convertible, most of them PAC identity, hint, return and stub
branches, with a few hundred PACGA sites left for the emulator.

## Kernel precompiled-root mode

`b2l_precompiled=1`, effective only together with `b2l_arm64e_compat=1`, tells
the kernel that the root was converted offline ([build 0102](build-0102.md)):

- `b2l_native_cache_before_init` returns immediately: no index allocation, no
  pre-init apply/reclaim/undo pass over learned records, no learner workers.
  The SYSTEM cache file and the USERDATA undo archive are left as they are.
- The trap wrapper does not enqueue observations.
- A next PC on the same 4 KiB page as the trapped instruction skips the VM
  execute lookup. The CPU fetched the trapping instruction from that page,
  execute permission is page-granular, and returning to EL0 re-enforces the
  real permission in hardware; the target instruction copyin is retained.
  Branch targets and fall-throughs onto another page keep the full lookup.
  `B2L EMU PERF` reports `elided=` and `lookups=`.

The loader's default command line now includes the argument. Dropping it
returns to the cache lifecycle; a precompiled root still boots under it.

## Interaction with the device cache

- A precompiled root has no learnable instructions left: the worker's converter
  rejects PACGA/LDRA at enqueue time, so learning stays idle and the cache file
  mostly serves as a fallback. `b2l_native_cache=1` can stay enabled.
- Records learned against an unconverted root do not match a precompiled page:
  the page equals neither the recorded original, the recorded native page nor a
  bytewise mixture of both, so pre-init apply skips them. They are harmless but
  never reclaimed. Reinitialise the SYSTEM cache with
  `scripts/init-native-cache.py` when flashing a precompiled root so the
  pre-init pass has nothing stale to compare.
- The undo archive in `/private/var/db/.b2l-native-undo-v1` lives inside the
  root image. A freshly prepared root starts without one.
- `b2l_apps_payload_compare.h` already accepts `b2l_nc_native()` differences
  between a source payload and an installed executable, so precompiled app
  binaries still match their source images.

## Verification

```sh
scripts/test-precompile.sh                       # fixtures under ASan/UBSan
scripts/test-precompile.sh /path/to/dyld_shared_cache_arm64e   # plus a report-only real cache pass
```

The test first runs the shared converter's existing vectors, then compiles the
actual tool with sanitizers and drives it over synthetic fixtures whose words
are assembled by the host toolchain: a thin ARM64e Mach-O with text, stubs,
code-shaped data in `__const`, and a pure-instruction section in a
non-executable segment; a fat file with x86_64, ARM64e and plain ARM64 slices; a
two-file dyld cache group with executable and data mappings, a subcache image,
the excluded corecrypto image and an in-cache dyld. It checks that report mode
writes nothing, that write mode produces exactly the independently modelled
bytes, idempotence, `--verify` exit codes, hard-link deduplication, symlink
non-traversal, timestamp preservation, the exclusion options, and that
truncated load commands, oversize sections, a section outside an executable
mapping and a missing subcache fail closed without writing.

Passing these checks establishes the file transformation. Startup time, trap
counts and UI responsiveness on the S10+ must be measured on the device with a
precompiled root and a reinitialised cache.
