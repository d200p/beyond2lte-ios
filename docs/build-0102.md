# Build 0102: offline ARM64e precompilation and the precompiled-root kernel mode

Status: kernel, loader, firmware and BOOT image built locally and installed on the
second unit together with a precompiled root and the new SYSTEM
cache/log image, all three partitions readback-verified. The first boot result
is below; the root is being re-precompiled for the shared stub regions.

## Change

The runtime observed-instruction cache converts one trapped site at a time and
re-applies learned pages on the next boot. Every unconverted RETAB, BRAA/BLRAA
stub branch and PAC/AUT site first costs an exception, two VM execute lookups
and a learner observation. This build moves the conversion off the device and
lets the kernel skip the machinery that a converted root no longer needs.

1. **Host precompiler** `tools/b2l-precompile.c` rewrites every allowlisted
   instruction in a prepared root before flashing, through the kernel's own
   `b2l_nc_native()` converter. Section selection, shared-cache image/mapping
   resolution and the corecrypto exclusion follow the kernel's definitions.
   See [offline precompilation](arm64e-precompile.md).
2. **`b2l_precompiled=1`** (effective only with `b2l_arm64e_compat=1`):
   - `b2l_native_cache_before_init` returns before index allocation, the
     pre-init apply/reclaim/undo pass and worker start, logging
     `B2L NCACHE: precompiled root; pre-init apply, learning and workers skipped`.
     The SYSTEM cache file and USERDATA undo archive are left untouched, so a
     non-precompiled root can still use them by dropping the argument.
   - The trap wrapper no longer calls the learner after an emulation.
   - The execute check for a next PC on the same 4 KiB page as the trapped
     instruction is elided: the CPU fetched the trapping instruction from that
     page, execute permission is page-granular, and the hardware re-enforces
     the real permission on return to EL0. The instruction copyin of the target
     is kept. Branch targets and fall-throughs onto another page keep the
     full VM lookup. The current-PC check is covered by the same rule. The
     remaining PACGA/LDRA traps therefore pay the exception, decode and hash,
     but no VM map lock in the common case.
   - `B2L EMU PERF` gains `elided=` and `lookups=` counters.
3. The loader command line adds `b2l_precompiled=1` after `b2l_native_cache=1`.

## Verification performed here

- `scripts/test-precompile.sh` with the macOS 27 host arm64e cache: converter
  vectors, synthetic Mach-O/fat/dyld-cache fixtures under ASan/UBSan, and a
  report-only pass over 82 cache files / 4115 images with no errors.
- `tests/test-pacga-runtime.py` (ASan/UBSan and TSan) against the pinned
  `bsd/net/siphash.{h,c}`: the actual wrapper in both PACGA modes, plus the new
  precompiled-mode cases: opt-in gating, zero lookups for same-page return and
  fall-through, one lookup for a cross-page branch or page-crossing
  fall-through, retained execute denial/bounds/fault rejection, and restored
  behavior after leaving the mode.
- `tests/test-native-cache-precompiled.py`: the actual gating helpers and the
  first statements of `b2l_native_cache_before_init` with mock boot arguments.

Kernel and loader object compilation against the iPhoneOS 26.5 SDK/KDK was not
possible on this machine and remains part of the build step.

## First device result (second unit)

The owner unlocked a second S10+, flashed TWRP, and installed the precompiled
root, the new SYSTEM cache/log image and this BOOT image. The first boot reached
the Apple logo but not the lock screen within six minutes. The recovered log
confirmed `precompiled root mode`, the skipped cache pass and a mounted root,
but also 138.8 million traps in 367 seconds: the first trapped instruction was
`BRAA x16, x17` in a shared-cache stub. The precompiler had converted image
sections only, and the cache builder keeps stubs outside those sections (see
[offline precompilation](arm64e-precompile.md)). The tool now converts the
shared stub regions as well; the root must be re-precompiled and re-flashed
before timings mean anything. The speech-synthesizer audio units crashed with
SIGABRT in that boot; they are not related to the conversion.

## 0103 follow-up: cheaper PACGA traps

After the stub-island fix, the second boot's trap count flattened at about
61 million, nearly all PACGA in Foundation, CoreGraphics and CoreFoundation
(687 sites the converter cannot replace). Sampled cost per trap was 5.1 us:
hash 0.33 us, VM lookup 0.64 us, and 4.1 us of other wrapper work, so a PACGA
result cache would save little. In precompiled mode the wrapper now:

- skips both the VM lookup and the instruction re-read for a next PC on the
  trapped instruction's own 4 KiB page, which covers the current-PC check,
  fall-through after PACGA and the second PACGA of a pair; the trap path has
  already fetched that page, and hardware re-checks permission on return;
- keeps every per-trap counter in per-CPU cache lines instead of shared
  atomics, and refreshes the last pid/pc diagnostic every 64th trap per CPU.

A paired PACGA now performs one user read instead of four. Cross-page branch
targets keep the full lookup and read. Non-precompiled behavior is unchanged.
BOOT image `out/0103/boot.img`.

## 0104: PACGA without traps

`--pacga-trampolines` replaces each PACGA in shared-cache image text with a
branch to a seven-instruction keyless routine plus a branch back. Routines live
in all-zero pages inside executable cache mappings that dyld's mapping table
describes, either stub-island padding or padding between images, always within
the +/-128 MiB branch range. The routine writes only the destination register,
sets no flags and uses no memory. `b2l_pacga_keyless=1`, which needs the
precompiled mode, makes the kernel emulator compute the same function for
anything still trapping: JIT code, SP operands and out-of-range sites. On this
root all 687 sites are routed; the verify pass reports `remaining pacga=0`.

**The root and kernel are a pair.** An earlier kernel computes SipHash with a
random key, so CoreFoundation's object checks would fail against the routines.
Flash them together, and roll back together.

Tests: the encoder matches the assembler for all 30,752 supported register
triples; the routine executes natively on an Apple Silicon host and matches the
kernel C function on 60,000 inputs including aliasing and XZR; the wrapper
harness checks the keyless mode gate, SP operands and that no entropy is drawn;
the precompiler fixture checks placement, both branch targets, range limits,
exclusions, NOP for XZR destinations and idempotence.
BOOT image `out/0104/boot.img`.

## 0105: touch firmware for the second unit

On the second unit touch never initialized: every boot logged `B2L TOUCH EFI:
locate=0x800000000000000e` (EFI_NOT_FOUND), zero DVDD/AVDD, and four
device-ID timeouts before input was disabled. The firmware workspace had been
built from the reference UEFI `master`, whose B2LTouchDxe neither installs
`gB2LTouchDiagnosticProtocolGuid` nor powers the controller through the PMIC
regulator protocol, and is force-loaded through APRIORI. The first unit used
branch `codex/touch-from-8549bfa` (pinned `48add5bb…`, no longer on the server).
0105 takes only the touch pieces from that branch's current tip `c7846ec`:
B2LTouchDxe sources and INF, the diagnostic protocol header (identical to the
loader's copy), `beyond2lte.dec` with its GUID, the S5E9820 HsI2c library, and
removal of B2LTouchDxe from APRIORI. Its USB/MAX77705 memory windows and test
applications are not taken. Kernel unchanged from 0104, so the
0104 root still matches. BOOT image `out/0105/boot.img`.

## Build

On the machine holding the pinned dependencies described in [build.md](build.md):

```sh
scripts/fetch-xnu.sh                       # pinned ac9718fb…
scripts/prepare-xnu.sh                     # patches + xnu-overlay (includes this change)
KDKROOT=/path/to/merged-kdk B2L_DEPSROOT=/path/to/deps scripts/build-kernel.sh CWARNFLAGS=-w CXXWARNFLAGS=-w
python3 scripts/check-native-cache-codegen.py out/xnu/obj/<object-root>
WORKSPACE=/path/to/disposable-mu PYTHON_COMMAND=/path/to/venv/bin/python CLANG_BIN=/path/to/llvm-tools/ scripts/build-boot-firmware.sh
python3 scripts/package-boot.py --mu "$MU_WORK_TREE" --firmware "$XNU_PROFILE_FD" --kernel "$B2L_XNU_MACHO" \
    --loader "$LOADER_EFI" --output-dir out/0102 --boot-partition-bytes 57671680
python3 scripts/package-boot.py --verify out/0102/boot.img
```


## Installation order

1. Precompile and verify the root image, then flash USERDATA
   ([offline precompilation](arm64e-precompile.md)).
2. Flash `out/0102/boot.img` to BOOT from root recovery after identity,
   alias and capacity checks, with prefix readback, as for every earlier build.
3. First boot evidence: the early `precompiled root mode` line, the
   `B2L NCACHE: precompiled root` line before launchd, `elided=` growing with
   `lookups=` staying small in `B2L EMU PERF`, and Apple-logo/lock-screen
   timings compared with 0092's roughly 60 s / 90 s.

Rollback: remove `b2l_precompiled=1` from the loader string (or boot the
installed 0100 image) to return to the cache lifecycle; the precompiled root
keeps working under it, with stale learned records reported as skipped.
