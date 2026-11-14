# Build 0127: translator in shadow mode, GPU idle power-down

Status: BOOT image built; see the result below once booted. Contains everything
in [build 0126](build-0126.md). Root, firmware and loader are unchanged. The UI
still draws on the CPU; the screen cannot change because of this build.

## Why shadow mode

The destination is not always an IOSurface, rows may run bottom-up, and each
state byte comes from reading the binary. A wrong guess on the screen would look
like a random UI bug. So a sample of translatable draws, at most 100 per 5 s,
also runs on the GPU into a private copy of the destination's tiles. The CPU
then draws the real pixels and the two are compared.

Translated so far: single axis-aligned quads of at least 64x64 whole pixels, Copy
or Sover, either solid (`kind` 0) or a surface texture (`kind` 1 or 3, unit type
3, image function IDENTITY) with no tint.

```
B2L GPU UI: K kind=.. blend=../.. image=.. units=../.. draws=.. us=.. cmp=.. exact=.. close=.. wrong=.. fail=.. gpu_us=.. cpu_us=.. kpix=..
B2L GPU UI: X kind=.. rect=.. of WxH up=.. tex=WxH off=N/M at x,y gpu=.. cpu=..
```

`close` differs by at most 2 levels per channel. `X` records the first wrong
pixel of up to 40 draws. A failed job stops sampling for the boot. `gpu_us` and
`cpu_us` compare the same draws, the former including its submit.

## Kernel

- The GPU cores power up on the first job and down 50 ms after the last.
- New selector `MapRange` wires client memory into a slot, because a destination
  or texture may be plain memory. A range is reused only while its first and
  last pages are unchanged, so reused addresses do not reach old pages.

## Measured first

A full 1440x2904 surface flushes in under 1 ms and a 256x256 layer in about 9 us
(`b2l-mali-probe flush`, recovery kernel). Cache maintenance is not the
bottleneck that the coherency risk suggested, so each draw can go to the GPU on
its own without batching.

## Device result

Booted once. The chord worked (`volume keys held, syncing for recovery`,
`restarting into recovery`), and idle power-down caused no fault.

Most samples disagreed, and shadow mode kept that off the screen. The CPU
filled only part of draws that claimed the full surface, in about 0.4 ms. The
translator read the whole destination at 0x9e0 onwards, but the rasterizer draws
into a narrower window at 0x9a8 to 0x9dc. See [0128](build-0128.md).
