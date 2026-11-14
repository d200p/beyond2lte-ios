# Build 0146: batched GPU passes

Status: booted once. The hooks work; batches do not form yet. Contains everything
in [build 0145](build-0145.md).

Each live draw was its own GPU pass: clean the destination, copy it into the
tiles, draw, invalidate. Consecutive live draws into one destination now share a
pass. The copy-in ("keep") draw sits in slot 0 and is rewritten at commit with the
union of the batch's tile bounds; the clean and the invalidate happen once per
batch.

A batch ends when:

- a draw cannot join it: another destination, a different stride or size, a full
  pass, a draw that samples the destination, or any draw that runs on the CPU;
- Core Animation touches a destination outside `draw_elements`: `clear_region`,
  `copy_destination`, `blur_surface`, `make_buffer_current` and
  `finalize_surface` get pre-hooks that commit first, and so does the block in
  `IOMobileFramebuffer::swap_begin`, so no frame reaches the display with work
  still queued.

The pre-hooks save every argument register, run the commit under the render
lock, then execute the displaced entry words, as the `draw_elements` hook does.
Each entry's words are checked before any is patched; a mismatch installs none.

A render thread that finds a batch open waits for the lock rather than skipping
the GPU path, so a CPU draw on another thread cannot run ahead of queued GPU work.

## What can go wrong

Stale pixels: a CPU read of a batched destination through a path this build does
not hook. The symptom is a region that shows the previous frame until the next
one. The `X` lines will not show it, because live draws are not compared; the
screen will.

`P` lines carry `commits` and `commit_us`; `draws / commits` is the batch size.

| Sign | Meaning |
| --- | --- |
| `hook: 6 pre-hooks installed` | All six entries matched and were patched. |
| `hook: ... entry bytes differ` | The cache differs; nothing runs on the GPU this boot. |
| `draws / commits` near 1 | Batches are not forming; the flush points fire too often. |
| `gpu: batch ... stopping` | A batched pass faulted; GPU drawing is off for the boot. |

## Device result

All six pre-hooks matched and installed. 362 live draws, 351 commits: 1.03 draws
per batch, so something ends almost every batch after one draw, and the per-draw
cost stayed at 1.6 ms. Nothing visible was reported, no GPU fault, no backboardd
crash. One new report: locationd aborted on an uncaught exception at 190 s, with
no GPU involvement. [Build 0147](build-0147.md) counts the reasons batches end.
