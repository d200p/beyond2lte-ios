# Build 0129: direct cache flush, rejection reasons

Status: booted once; see the result below. Contains everything
in [build 0128](build-0128.md). The UI still draws on the CPU.

- The library flushes with `dc civac` and `dsb sy` directly, as
  `b2l-mali-probe flush` measured, in place of `sys_dcache_flush`.
- Every refused draw records why: a line `R` of 16 counters per 5 s, `why=` per
  kind of draw, and raw values on lines `T` (transform), `Q` (not a quad), `G`
  and `N` (destination), `S` (texture stride).

| `why` | Meaning |
| ---: | --- |
| 1 | Not one indexed quad, or a clip plane |
| 2 | Blend mode |
| 3 | Destination not prepared |
| 4 | Detached layers pending |
| 5 | Not 32 bits per pixel |
| 6 | Transform set |
| 7 | Kind or unit not translated |
| 8 | Not an axis-aligned quad with one colour |
| 9 | Destination geometry |
| 10 | Bounds are not a window on the destination |
| 11 | Fractional edges |
| 12 | Under 64x64 after clipping |
| 13 | Texture unusable |
| 14 | Texture stride |
| 15 | Tinted |
| 16 | Rotated or sheared texture |

## Device result

Stale cache lines were the cause: all 9 sampled solid fills now match the CPU
exactly. `sys_dcache_flush` did not do what the GPU path needs; direct `dc civac`
does.

A full-screen solid fill took the GPU 5.4 ms against 1.4 ms on the CPU, which is
effectively a `memset`. Solid fills should stay on the CPU.

Refusals, by count: geometry 4,991, multi-quad 1,837, destination not prepared
1,529, kind 649, blend flags 475, perspective quads 57. The geometry samples show
why: the rasterizer draws into an offscreen surface with stride 5760 while the
fields at 0x9e0 describe the window, stride 5888. Composition happens offscreen
and is copied to the window later. See [0130](build-0130.md).
