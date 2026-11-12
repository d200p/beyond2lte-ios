# Build 0123: CPU clock measurement

Status: BOOT image built offline; not yet booted. Contains everything in
[build 0122](build-0122.md).

The UI is limited by Core Animation drawing on the CPU (about 13 frames per
second at best). The bootloader sets the Exynos 9820 CPU clocks and nothing
in this port changes them, so the cores may be running below their maximum
(Cortex-A55 1.95 GHz, Cortex-A75 2.31 GHz, Exynos M4 2.73 GHz).

About 20 s after boot the persistent-log thread binds itself to each CPU in
turn and times a chain of dependent adds, which retire one per cycle on all
three core types (`b2l_cpu_clock.h`). The best of seven runs is logged:

```
B2L CPU CLOCK: cpu=N ran_on=N mhz=... (6400000 adds in ... ns)
```

The same loop measured 3212 MHz on an Apple M1 Pro performance core
(nominal 3228 MHz). No PMU, clock-controller or voltage register is touched.
