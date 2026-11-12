# GPU translation layer

Goal: Core Animation's frames drawn by the Mali-G76 instead of the CPU. The
UI peaks at about 13 frames per second because `CA::OGL::SWContext` rasterises
1440x2904 frames on the CPU ([build 0113](build-0113.md)); presenting them
takes only about 5% of that time.

Status: the GPU runs jobs under XNU, and textured, blended composition with
this project's own shaders works on the device under the recovery kernel. **iOS
does not use any of it yet**: the UI is still drawn by the CPU. See
[What exists](#what-exists).

## Decision

Translate at `CA::OGL::SWContext::draw_elements`, with the CPU renderer kept as
the fallback for every draw the GPU path does not handle. Do not implement
Metal.

| Option | Why not, or why |
| --- | --- |
| Metal driver bundle for Mali | Covers apps too, but `CA::OGL::MetalContext` alone has about 290 methods (heaps, memoryless textures, tile pipelines, shared events), and every shader arrives as Metal IR that would need a translator to Bifrost. It is all-or-nothing: the screen is black until it all works. |
| `CA::OGL::GLESContext` over a GLES driver | The class ships, but this has not been shown to be reachable, and iOS's own GLES runs on Metal, so a full GLES stack would still have to be supplied. |
| **`SWContext::draw_elements`** | One function that is handed everything: `(PrimitiveMode, count, const unsigned short *indices, const Vertex *vertices, unsigned, unsigned, ...)`, with blend and texture state on the context. A draw the layer cannot translate falls through to the CPU code, so the UI keeps working throughout and coverage grows one draw type at a time. |

The cost of this choice: it accelerates only Core Animation compositing. Apps
that use Metal directly, and anything iOS draws through Metal, stay
unsupported, and offsets are pinned to the exact 23G90 cache, as
`scripts/patch-ca-software-surface.py` already is.

## Layers

```
backboardd
  CA::OGL::SWContext::draw_elements      entry patched in memory by the library
    userspace/b2l-gpu.c                  measures today
    translator (planned)                 Vertex + state -> b2l_mali_quad
      supported: quads with Sover/Copy blending, texture, solid fill
      otherwise: wait for the GPU, run the original CPU body
    b2l_mali_draw.h                      a pass: tiler chain, fragment job
    b2l_mali_shaders.h                   generated Bifrost binaries
  ---- user client: map IOSurface, submit, wait ----
kernel
  B2LMali.cpp + b2l_mali_core.h          power check, reset, MMU, job slots
Mali-G76 MP12 at 0x18500000
```

There is no vertex shader and no shader compiler. The CPU writes window-space
positions and texture coordinates straight into the buffers the tiler and
fragment stages read. `scripts/gen-mali-shaders.py` encodes the few fragment
programs needed. A textured draw needs only a one-clause `BLEND` shader: the
RSD message preload fetches the texel into r0..r3 before the shader starts.

## Hardware facts

From the stock kernel running on this unit (TWRP, kbase r32p1) unless noted.

| | |
| --- | --- |
| GPU | Mali-G76 MP12, arch 7.2.1 r0p0, Job Manager |
| Registers / IRQs | `0x18500000`, 0x5000 bytes; SPI 288 job, 287 MMU, 289 GPU (stock DT) |
| Shader core mask | `0x770077` |
| Clock and voltage | 156 to 702 MHz, 543.75 to 681.25 mV, set through ACPM (`dvfs_g3d`) |
| Power domain | `pd-embedded_g3d`, status at PMU `+0x2104`; the stock kernel finds it **on** at probe |
| Errata for r0p0 | TMIX_8133, TSIX_1116, TSIX_2033, TNOX_1194, TTRX_921, GPU2017_1336, TTRX_3464 |

Should the domain ever be found off, the sequence is in Samsung's public CAL
tables (`flexpmu_cal_local_exynos9820.h`, `embedded_g3d_on`) plus a D-TZPC
restore SMC. Stage 1 does not use it.

## What exists

| Piece | State |
| --- | --- |
| Fragment job, page table, reset, power, job slot (`b2l_mali_core.h`) | **Verified under XNU** on [0124](build-0124.md) |
| Bifrost shaders and encoder | **Verified on the GPU** (recovery kernel, kbase); host-checked |
| Render pass: many layers, BGRA target, texture, Copy and Sover (`b2l_mali_draw.h`) | **Verified on the GPU** (recovery kernel, kbase) |
| Keeping old pixels in a partial redraw | **Verified on the GPU**: draw the target onto itself first |
| Tiler jobs, shaders and textures under XNU's own MMU setup | **Verified under XNU** on [0125](build-0125.md) |
| IOSurface mapping and user client, polled | **Verified under XNU** on [0126](build-0126.md) between real IOSurfaces |
| Hook and measurement in backboardd | **Verified** on [0126](build-0126.md): 61,601 draws |
| Interrupt-driven submission | Not written; jobs are polled |
| Layer opacity and linear filtering for scaled layers | **Verified on the GPU** (recovery kernel, kbase) |
| Translator | Not written |
| GPU idle power-down | Not written: the GPU stays on while backboardd runs |

All device tests compare every pixel of the 1440x2904 target. Rates below are
1440x2904 full-screen layers with texturing and Sover blending, through kbase,
on one pass, measured with the GPU capped at 156 MHz, the clock XNU finds:

| Layers | Time | Rate |
| ---: | ---: | ---: |
| 1 | 4.3 ms | 0.96 Gpix/s |
| 4 | 5.5 ms | 3.0 Gpix/s |
| 16 | 24 ms | 2.8 Gpix/s |
| 64 | 101 ms | 2.7 Gpix/s |

At 572 MHz, which Android's DVFS picks under load, 16 layers take 7.2 ms. Under
XNU a full-screen clear takes 2.0 ms at 156 MHz. A plain single-threaded C
blend of one such layer took 11 to 31 ms in recovery (0.13 to 0.38 Gpix/s),
depending on which CPU and clock it landed on. Neither figure predicts UI
frame rates: real layers scale, filter and add cache maintenance, and the first
jobs include submission latency.

Recovery runs need no flashing: `scripts/build-mali-probe.sh`, push to `/tmp`,
`chmod 755`, run (add `draw` for the render tests). The clock can be capped
through `/sys/kernel/gpu/gpu_max_clock`.

## Open risks

- **Cache coherency.** Confirmed: `COHERENCY_FEATURES` reads 0 under XNU, so
  the CPU must clean each dirty IOSurface before the GPU samples it, and clean
  and invalidate the target around a pass. The cost is unmeasured and could
  take back much of the gain.
- **Clock under XNU.** Measured at 156 MHz, the lowest level. Raising it means
  talking to ACPM; so would raising the CPUs, which 0123 measured at 1052, 1221
  and 1377 MHz (A55, A75, M4) against maximums of 1950, 2310 and 2730 MHz.
- **Software state.** `SWContext` keeps blend and texture state in structures
  whose layout must be recovered from the binary, as with earlier cache
  patches.
- **Mixed rendering.** A CPU-drawn draw after GPU draws must wait for the GPU.
  Frames that alternate heavily may gain little.
- **Texture stride.** A stride of 8 bytes raised `DATA_INVALID_FAULT`; 64 and its
  multiples work. IOSurfaces already use 64-byte rows.
- **Limited gain.** On 0126 draws took 59 to 76% of the time in the busiest
  windows, and about 70% of attributed draw time is in kinds and blends the GPU
  path can cover so far. Moving all of that to the GPU would give roughly 1.5x
  to 2x in those windows, before cache maintenance. Blurs and work outside
  `draw_elements` stay on the CPU.
- **Unsupported draw types.** Masks, blurs and vibrancy blends stay on the CPU, so how much of a real frame
  can move is unknown until `draw_elements` is instrumented.
