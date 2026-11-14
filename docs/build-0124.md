# Build 0124: Mali-G76 self test

Status: booted once; the self test passed. Contains everything in
[build 0123](build-0123.md). Root, firmware and loader are unchanged.

First stage of the [GPU translation layer](gpu-translation-layer.md). It draws
nothing on screen and does not change the UI path. The question it answers:
does the GPU work from XNU, in its own address space, with the power state the
boot chain leaves?

## Change

`B2LMali` starts one thread about 30 s after boot, which:

1. Reads `EMBEDDED_G3D_STATUS` and `G3D_STATUS`, and stops if the domain is off.
2. Reads the GPU id, stopping if it is not `0x72110000`.
3. Soft-resets the GPU and powers the L2, tiler and shader cores.
4. Measures the clock with the GPU cycle counter.
5. Sets up address space 0 with its own page table, then runs the fragment job
   proven on the device four times, checking all 1440x2904 pixels each time.
6. Powers the cores down and frees everything.

The PMU page is mapped for reading only. No PMU, clock, voltage or
secure-monitor request is made. Interrupts stay masked and every wait is
bounded. Boot argument `b2l_gpu=0` skips the stage.

## Reading the log

```
B2L GPU: pmu embedded_g3d=0x.. g3d=0x..
B2L GPU: reading GPU_ID
B2L GPU: id=0x72110000 status=... l2=... core=... tiler=... mem=... mmu=...
B2L GPU: as=... js=... shader=0x770077 tiler=... l2=... coherency=...
B2L GPU: cores ready shader=0x770077
B2L GPU: clock ... kHz (... cycles in ... ns)
B2L GPU: run=N job done ns=... bad_pixels=0 first=0xff2040c0
B2L GPU: self test passed
B2L GPU: stage finished
```

| Last line | Meaning |
| --- | --- |
| `power domain off, stopping` | The boot chain turned the GPU off; the CAL power-on sequence is needed. |
| `reading GPU_ID`, then the log ends | The block is powered but unclocked or secured, and the read stalled the bus. Reboot with the power button. |
| `... timed out` | That block did not respond; the rest of the boot continues. |
| `js_status=...` | The job faulted; the line carries the job, MMU and GPU fault registers. |
| `bad_pixels` above 0 with `job done` | The job ran but memory disagrees: suspect cache maintenance. |

A failure here must not affect the UI. The 30 s delay keeps it clear of early
boot.

## Device result

```
B2L GPU: pmu embedded_g3d=0x1 g3d=0x1
B2L GPU: id=0x72110000 status=0x0 l2=0x7120206 core=0x0 tiler=0x809 mem=0x301 mmu=0x2830
B2L GPU: as=0xff js=0x7 shader=0x770077 tiler=0x1 l2=0x1 coherency=0x0
B2L GPU: cores ready shader=0x770077
B2L GPU: clock 156000 kHz (1718693 cycles in 11017231 ns)
B2L GPU: run=0 job done ns=2002269 bad_pixels=0 first=0xff2040c0
B2L GPU: self test passed
```

- The boot chain leaves both G3D domains on and the GPU clocked, so no PMU, CMU
  or secure-monitor request was needed.
- The GPU runs at 156 MHz, its lowest level.
- `COHERENCY_FEATURES` is 0: the CPU must maintain caches for shared surfaces.
- Four jobs took 1.9 to 2.0 ms each, with every pixel correct and the target
  scattered across physical pages.
- The UI presented normally, with up to 141 swaps per 10 s. New reports since
  0123: dasd and webbookmarksd aborts, one Settings hang, one JetsamEvent and
  one hidden SiriSearchFeedback report, which was not parsed. None of the others
  mentions the GPU.

## Tests

`tests/test_mali_core.py` runs the bring-up against a simulated GPU that
enforces power ordering and reaches memory only through the page table it is
given, and decodes the job descriptors. Injected defects (a wrong render
target offset, a wrong page-table index) fail it. The job builder also ran on
the real GPU through `tools/b2l-mali-probe.c` in recovery.
