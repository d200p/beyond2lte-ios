# ACPM panic diagnosis and FVMAP decoding

The S10+ loader selects `b2l_dvfs=0` while investigating a kernel panic on
the SRAM-scanning build. That path starts no DVFS thread and accesses neither
ACPM SRAM nor its mailbox. The preceding BOOT reached Home again after rollback.
The failing boot's persistent log ended around DVFS startup, but did not retain
the panic registers. The exact failing access remains unconfirmed.

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
on this firmware. A future SRAM-only phone trial is still required to establish
that the firmware matches the reviewed descriptor ABI.

Protocol references, inspected at revision
`a412cfaf86b980ce35cae96a35f678c8ea551575` of Samsung's Exynos 9820 kernel
published by LineageOS:

- [Plugin discovery and FVMAP base](https://github.com/LineageOS/android_kernel_samsung_exynos9820/blob/a412cfaf86b980ce35cae96a35f678c8ea551575/drivers/soc/samsung/acpm/acpm.c)
- [Plugin descriptor fields](https://github.com/LineageOS/android_kernel_samsung_exynos9820/blob/a412cfaf86b980ce35cae96a35f678c8ea551575/drivers/soc/samsung/acpm/fw_header/common.h)
- [Framework and IPC channel fields](https://github.com/LineageOS/android_kernel_samsung_exynos9820/blob/a412cfaf86b980ce35cae96a35f678c8ea551575/drivers/soc/samsung/acpm/fw_header/framework.h)
- [FVMAP header and rate/voltage fields](https://github.com/LineageOS/android_kernel_samsung_exynos9820/blob/a412cfaf86b980ce35cae96a35f678c8ea551575/drivers/soc/samsung/cal-if/fvmap.h)

## Boot modes and verification

- `b2l_dvfs=0`: disabled, including all ACPM MMIO. This is the current loader
  default for the userspace diagnostic BOOT.
- `b2l_dvfs=1`: follow SRAM descriptors, validate CPU tables, measure core clocks,
  and return. Do not map or write the mailbox or send temperature requests.
- `b2l_dvfs=2`: experimental clock-control path. It now requires all CPU tables
  before mapping the mailbox. Channel pointers, element alignment, interrupt
  IDs and queue indices are checked before use. This mode is not selected by
  the diagnostic loader and its phone behavior remains unverified.

Frame/touch telemetry, PACGA mask ABI and native-cache behavior remain enabled.
These changes do not establish a clock or interface performance improvement.

`tests/test_acpm_diagnostics.py` runs the actual parser and driver control flow
with host-backed MMIO boundaries under ASan/UBSan. An independently laid-out
fixture distinguishes the member and rate offsets. Tests exercise all 16-bit
table offsets that must be rejected, malformed/ambiguous descriptors, partial
table failure, and the absence of mailbox access in disabled/SRAM-only modes.
`tests/test_platform_error.py` executes the patched fallback for null, 32-bit
and 64-bit saved states, both error sources, and non-B2L behavior.
The existing renderer test verifies first-fault retention and visible panic
rendering with quiet console enabled. Host tests cannot reproduce an ACPM bus
error or prove the runtime layout on the phone.
