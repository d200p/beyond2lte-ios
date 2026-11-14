# Build 0131: first draws on the GPU for real

Status: BOOT image built; see the result below once booted. Contains everything
in [build 0130](build-0130.md). **This is the first build where the GPU writes
UI pixels.**

## What goes live

Only `kind` 3, Sover, one surface texture, image function IDENTITY, covering at
least 200,000 pixels. That is the kind [0130](build-0130.md) proved: 65 of 65
within tolerance, 3.1 ms on the GPU against 6.7 ms on the CPU. Smaller draws stay
on the CPU, which is as fast there.

A live draw flushes the destination rows and the texture, runs the pass into
the real memory, invalidates those rows, then does what the CPU path does
afterwards: `marked_destination` and the draw counters.

The first 4 draws of the kind in each 5 s are still compared in shadow. If a job
fails, that draw falls back to the CPU and the GPU path stops for the boot.

## Also in shadow

Add blends, and `kind` 1 ignoring vertex colour.

`K` lines gain `live=`, `live_fail=`, `live_us=` (including cache work) and
`live_kpix=`. Booting 0130 again removes all of this.
