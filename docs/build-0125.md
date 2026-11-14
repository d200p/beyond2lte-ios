# Build 0125: Mali-G76 render pass under XNU

Status: BOOT image built offline; not yet booted. Contains everything in
[build 0124](build-0124.md). Root, firmware and loader are unchanged.

[0124](build-0124.md) ran clear jobs only. Everything that draws has so far run
through kbase under the recovery kernel. This build runs a real pass with
XNU's own GPU setup: page tables, job slots and cache maintenance.

## Change

After the four clear jobs, the self test draws one pass into a 1440x2904 BGRA
target cleared to green:

| Layer | Shader | Where |
| --- | --- | --- |
| 256x256 BGRA ramp, copied | `textured` | (100,200) |
| Red rectangle | `solid` | (400,800), 100x100 |
| White at opacity 0.5, Sover | `modulated` | (600,1000), 256x256 |

The tiler chain runs on job slot 1, then the fragment job on slot 0, each
polled. Every pixel is compared, within 2 levels per channel.

New in the driver: a 16 MiB tiler heap, a code page mapped executable (every
other page stays no-execute), and CPU cache cleaning of descriptors, shaders
and textures before the jobs. The builder no longer uses `float`, because the
kernel has no floating point; `b2l_mali_draw.h` moved into the kernel tree.

The stage still reads the PMU only, keeps interrupts masked, bounds every wait
and powers the cores down afterwards. Boot argument `b2l_gpu=0` skips it.

## Reading the log

```
B2L GPU: address space built, N of 32 tables
B2L GPU: run=N job done ns=... bad_pixels=0 first=0xff2040c0
B2L GPU: pass tiler_ns=... fragment_ns=... bad_pixels=0
B2L GPU: self test passed
```

| Line | Meaning |
| --- | --- |
| `tiler job status=...` | The tiler job failed; the next line has the fault registers. |
| `fragment job status=...` | Tiling worked and shading or memory did not. 0x50 to 0x58 are shader or descriptor faults. |
| `bad_pixels` above 0 | The jobs ran. `first bad x= y= got= want=` shows which layer; suspect cache maintenance first. |

The earlier table in [0124](build-0124.md) still applies to the steps before
the pass.

## Tests

`tests/test_mali_core.py` covers the tiler slot and execute permissions on the
simulated GPU. `tests/test_mali_shaders.py` checks the pass builder and the
integer-to-float conversion against the compiler's floats. The pass itself is
the one `b2l-mali-probe draw` verifies on the GPU under the recovery kernel,
apart from the XNU page tables and job submission this build exists to test.
