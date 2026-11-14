# Build 0148: scanout from surfaces, draws join open batches

Status: BOOT image built; see the result below once booted. Contains everything
in [build 0147](build-0147.md).

## Scanout

The display copy cost 7.8 ms per full frame. [0147](build-0147.md) showed the
panel fed by DMA channel 0 into window 5. Now:

- Panel-sized BGRA surfaces, the first six, are allocated physically contiguous
  under 4 GB, the base register's reach. An ordinary allocation follows if that
  fails.
- A swap with one full-panel layer in such a surface cleans the surface's cache
  lines and points channel 0 at it: base, frame and image size, the window's
  position below the camera band, then a shadow update request for window 5. No
  pixel is copied. The surface stays referenced until two flips later.
- Any other swap points the channel back at the panel memory first and copies
  as before.
- `b2l_scanout=0` keeps the copy path.

The clean is the remaining cost: about 17 MB through `dc civac`, which this
hardware does at a few GB/s.

## Batches

0147 showed batches ending at CPU draws 445 times out of 523. A draw that is
exact on the GPU but cheaper on the CPU alone, a solid fill or a small textured
draw, now joins a batch that is already open to its destination, where it costs
no flush of its own.

## What can go wrong

The panel shows black, garbage, a wrong offset, or stays frozen on the last copied
frame: the register meaning differs from the public driver. Reboot; `b2l_scanout=0`
or the rollback, 0147, restores the copy. The cleaned-but-stale case shows as
tearing or old pixels within a frame.

| Sign | Meaning |
| --- | --- |
| `flips` near `swaps`, `copy_ms` near 0 | Scanout works; `clean_ms` is its cost. |
| `fallbacks` high | Swaps are not single full-panel layers, or surfaces are not contiguous. |
| `flips` 0 | No surface met the conditions: see the swap lines for sizes. |
| `draws / commits` above 1 | Batches are forming. |
