# Build 0128: translator corrected, still in shadow mode

Status: booted once; see the result below. Contains everything
in [build 0127](build-0127.md). The UI still draws on the CPU.

## Fixes from 0127

- Destination fields are the rasterizer's own (0x9a8, 0x9b8, 0x9d0 to 0x9dc). The
  draw is clipped to those bounds and the scissor, with texture coordinates
  adjusted, and the window must lie inside the whole destination.
- A draw is refused if the context has a transform (0x568), which the rasterizer
  applies to vertices, or a separate second destination plane.

## Added

- Image textures (unit types 1 and 2), with their format in the statistics.
- `kind` 52, on trial.
- At most 12 samples per kind of draw in each 5 s, so solid fills cannot use up
  the budget.
- Comparison covers the whole of each covered tile, so drawing out of bounds
  shows as `out=` in the `X` line.

A texture stride that is not a multiple of 64 stays on the CPU: it raised
`DATA_INVALID_FAULT` in recovery.

## Device result

The chord worked again. Only 10 draws were compared: the new checks refused
everything else without saying why. Of those 10, full-screen fills differed in
runs of 64 bytes starting on cache-line boundaries, with `gpu=00000000`, the
value the CPU held before the job. That points at cache lines surviving
`sys_dcache_flush`, not at GPU output. See [0129](build-0129.md).
