# Build 0130: rasterizer destination, multi-quad draws

Status: booted once; see the result below. Contains everything
in [build 0129](build-0129.md). The UI still draws on the CPU.

- The destination is taken only from the rasterizer's fields (0x9a8, 0x9b8, 0x9d0
  to 0x9dc). The window fields are no longer used.
- The library calls `prepare_destination_` under the same test `draw_elements`
  uses, so a draw is translated against the surface it will really land on.
- Draws of up to 16 quads, such as nine-slice images, are translated as one pass.
  All quads must share a colour.

`why` codes 3 and 10 changed meaning: 3 is now a misaligned destination, 10 more
than 16 quads.

## Device result

Ran 24 minutes with no GPU fault and no backboardd crash. Per draw, GPU time is
the job and its submit; it leaves out the cache work of a real draw.

| Draw | Compared | Wrong | GPU | CPU |
| --- | ---: | ---: | ---: | ---: |
| `kind` 3, Sover, surface | 65 | 0 | 3.1 ms | 6.7 ms |
| `kind` 0, Copy | 268 | 0 | 3.7 ms | 0.8 ms |
| `kind` 0, Sover | 77 | 0 | 2.4 ms | 3.4 ms |
| `kind` 3, Copy, surface | 41 | 0 | 0.7 ms | 0.3 ms |
| `kind` 1, Copy, surface | 117 | 38 | 1.3 ms | 4.6 ms |
| `kind` 1, Sover, surface | 72 | 7 | 0.6 ms | 1.6 ms |
| `kind` 52, Sover, surface | 84 | 37 | 1.4 ms | 10.9 ms |

Plain copies and fills stay on the CPU, which is close to `memcpy` there.
`kind` 1 samples the texture alone: the CPU ignored vertex colour, so opacity 0
still drew the image. `kind` 52 does something not yet identified.

Still refused: `kind` 1 with blend 57 (SDFUnion, 13.6% of draw time, the rounded
shapes), Add (6.8%, fixed-function, added next), blend flags beyond bit 0, and
image rows not aligned to 64 bytes.

After this boot's chord restart TWRP came up without USB until it was restarted
from its own menu. The three earlier chord restarts did not do this.
