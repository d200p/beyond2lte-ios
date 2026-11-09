# ACPM panic diagnosis and FVMAP decoding

The diagnostic S10+ BOOT with `b2l_dvfs=0` reached Home, confirmed by the owner.
That path starts no DVFS thread and accesses neither ACPM SRAM nor its mailbox.
The SRAM-only trial (`b2l_dvfs=1`) subsequently reached Home and found FVMAP at
SRAM+`0x29000`, validating all three CPU tables without mailbox access. The
owner still reports very slow interaction. The next trial selects
`b2l_dvfs=2` with the bounded policy below; its physical result is pending.
The older SRAM-scanning build's exact failing access remains unconfirmed.

The platform-error fallback now captures PC, LR, ESR and FAR through the typed
first-fault recorder before the native panic. It labels synchronous and
asynchronous errors separately. For asynchronous errors the saved PC may follow
the failing transaction; FAR is the value supplied by the exception path and
is not necessarily a valid failing address. The existing fatal behavior remains.

## Firmware layout

The decoder follows the framework's plugin array instead of searching SRAM:

1. Read the plugin array offset and count from framework offset `0x7000`.
2. Decode 48-byte plugin descriptors. Require one attached, staying-attached
   plugin with a printable, terminated firmware name containing `dvfs`.
3. Clear the entrypoint's Thumb bit and add the device's `fvmap_offset=0xa400`.
   Require the entire 8 KiB FVMAP region to fit inside the 192 KiB SRAM mapping.
4. Decode the CPU-domain headers at indices 2, 3 and 4. Each header is 24 bytes:
   `o_members` is at byte 18, **`o_ratevolt` is at byte 20**. The preceding
   implementation read the member offset as a frequency-table offset.
5. Reject unaligned or out-of-range tables, invalid levels/initial levels,
   mismatched domains, and zero, non-kHz, ascending, duplicate or excessive rates.
   Publish the three ascending CPU tables together only after all are valid.

Every SRAM load in this path is an aligned volatile 32-bit load, including
firmware-name and subword-header decoding. No byte or unaligned MMIO loads are
needed. No fallback probes the old fixed location or other SRAM addresses.
Descriptor bounds cannot establish that every address is physically accessible
on every firmware. The SRAM-only phone trial verified this descriptor layout
and these reads on the current S10+ firmware.

Protocol references, inspected at revision
`a412cfaf86b980ce35cae96a35f678c8ea551575` of Samsung's Exynos 9820 kernel
published by LineageOS:

- [Plugin discovery and FVMAP base](https://github.com/LineageOS/android_kernel_samsung_exynos9820/blob/a412cfaf86b980ce35cae96a35f678c8ea551575/drivers/soc/samsung/acpm/acpm.c)
- [Plugin descriptor fields](https://github.com/LineageOS/android_kernel_samsung_exynos9820/blob/a412cfaf86b980ce35cae96a35f678c8ea551575/drivers/soc/samsung/acpm/fw_header/common.h)
- [Framework and IPC channel fields](https://github.com/LineageOS/android_kernel_samsung_exynos9820/blob/a412cfaf86b980ce35cae96a35f678c8ea551575/drivers/soc/samsung/acpm/fw_header/framework.h)
- [FVMAP header and rate/voltage fields](https://github.com/LineageOS/android_kernel_samsung_exynos9820/blob/a412cfaf86b980ce35cae96a35f678c8ea551575/drivers/soc/samsung/cal-if/fvmap.h)

## Boot modes and verification

- `b2l_dvfs=0`: disabled, including all ACPM MMIO. The owner confirmed that this
  diagnostic baseline reached Home.
- `b2l_dvfs=1`: follow SRAM descriptors, validate CPU tables, measure core clocks,
  and return. The phone reached Home and validated the tables. Do not map or
  write the mailbox or send temperature requests.
- `b2l_dvfs=2`: experimental clock-control path, selected for the next trial.
  It requires all CPU tables
  before mapping the mailbox. Channel pointers, element alignment, interrupt
  IDs and queue indices are checked before use. Its phone behavior remains
  unverified.

## Bounded CPU policy for the next trial

The raw FVMAP contains rates above the current phone's DT limits. Do not use
its highest entries as operating limits. The current profile caps A55 at
1950000 kHz, M4 at the DT's 2080000 kHz limit, and A75 at 2002000 kHz, below
the DT levels that require an additional MIF floor. The DT also requires
A55 >= 1053000 kHz at this A75 target. Reject a profile whose A55 floor cannot
maintain that requirement. Boot arguments can lower ceilings, but cannot
raise them. No MIF, PLL, voltage-margin or regulator programming is added.
Frequency requests use the firmware's existing
[ACPM rate API](https://github.com/LineageOS/android_kernel_samsung_exynos9820/blob/a412cfaf86b980ce35cae96a35f678c8ea551575/drivers/soc/samsung/cal-if/acpm_dvfs.c).

Before requesting any CPU rate, require all four temperature readings, each
in 0..125 C, with the hottest below 80 C. Match every measured boot clock to
a FVMAP entry within 5 percent. Build governor rungs from that entry up to
the capped target, preserving the observed boot floor. An explicit lower
cap can lower that floor, subject to the A55 dependency check above.

Request little, middle and big targets in sequence. After each, measure the
cluster clock and read temperatures again. A failed transport, frequency
mismatch outside 10 percent, missing sensor or hottest-zone reading at 80 C
attempts restoration of changed clusters in reverse order and stops the driver.
The `sent_mhz` fields describe requests, not confirmed runtime frequencies.
The before/after measurements establish whether the first requests took effect.

During operation, read temperatures before the next requests once per second.
At 80 C use the middle rate; at 90 C restore the boot rate. Hysteresis releases
those steps below 74/84 C. Missing readings attempt restoration and stop the
driver. Restoration failures are logged, so a request is never evidence that
hardware accepted it. This is an experimental policy, not a validated device
power/thermal stack.

Framebuffer telemetry now reports successful copy count, cumulative elapsed
copy-path time and the longest copy since the last snapshot. That elapsed
interval includes the framebuffer lock and the scanout copy/barrier, not pure
CPU time. A gap between submissions is a separate measurement and does not
by itself identify why userspace stopped presenting frames.

Frame/touch telemetry, PACGA mask ABI and native-cache behavior remain enabled.
These changes do not establish a clock or interface performance improvement.

`tests/test_acpm_diagnostics.py` runs the actual parser and driver control flow
with host-backed MMIO boundaries under ASan/UBSan. An independently laid-out
fixture distinguishes the member and rate offsets. Tests exercise all 16-bit
table offsets that must be rejected, malformed/ambiguous descriptors, partial
table failure, and the absence of mailbox access in disabled/SRAM-only modes.
The control tests simulate ACPM responses and clock measurements to exercise
board ceilings, boot floors, sensor failures, initial mismatches, reverse
restoration and thermal fallback. `tests/test_fb_timing.py` exercises the actual
elapsed-time counters, reset behavior and concurrent copy accounting.
`tests/test_platform_error.py` executes the patched fallback for null, 32-bit
and 64-bit saved states, both error sources, and non-B2L behavior.
The existing renderer test verifies first-fault retention and visible panic
rendering with quiet console enabled. Host tests cannot reproduce an ACPM bus
error or prove the runtime layout on the phone.
