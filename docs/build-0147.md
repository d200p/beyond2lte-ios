# Build 0147: display controller dump, batch end reasons

Status: booted once; both measurements landed. Contains everything in
[build 0146](build-0146.md).

The `B` line counts why batches ended: a CPU draw, another destination, a full
pass, a self-sampling draw, a texture that could not be prepared, and one count
per pre-hook. In 0146 nearly every batch held one draw.

The display copy costs 7.8 ms per full swap ([0144](build-0144.md)) and is not
short of memory bandwidth ([0145](build-0145.md)). The only way to remove it is
to have the display controller scan out a surface directly. Samsung's public dpu20
driver shows the per-frame sequence: write the window's DMA base address, request
a shadow update for that window, and let the hardware trigger send the frame.

Before any register is written, this build reads how the boot chain left DECON_F
(0x19030000) and the six input DMA channels (0x19071000 to 0x19076000), 45 s after
boot, in groups that each reach the log before the next. `b2l_decon_dump=0` skips
it. Nothing is written.

```
B2L DECON: global=.. shadow_req=.. trig=.. path0=.. path2=.. panel_pa=ca000000
B2L DECON: winN control=.. .. start=.. end=..
B2L DECON: dmaN enable=.. in_con=.. src=.. offset=.. img=.. base=..
B2L DECON: dump done
```

The channel whose `base` equals `panel_pa` is the one the panel shows, and its
window is the one whose control word names that channel. Scanout from a surface
also needs physically contiguous surface memory or SysMMU programming; the
surfaces are not contiguous today.

| Sign | Meaning |
| --- | --- |
| `reading global`, then the log ends | A register read stalled the bus: take the rollback, 0146. |
| No `base` equals `panel_pa` | The panel is fed another way, perhaps through the SysMMU. |

## Device result

```
B2L DECON: global=00000113 shadow_req=00000000 trig=00001001 path0=00100000 path2=000000b1 panel_pa=ca000000
B2L DECON: win5 control=00000102 00000000 start=00000000 end=0bdf059f
B2L DECON: dma0 enable=00000004 in_con=02000000 src=0be005a0 offset=00000000 img=0be005a0 base=ca000000
```

- **Display.** DECON_F is running (`RUN_STATUS`) with the hardware trigger on.
  Window 5 is the only one enabled (`path0` bit 20), covering 0,0 to 1439,3039, and
  DMA channel 0 (IDMA GF0, 0x19071000) feeds it from 0xCA000000 at 1440x3040,
  format field 0: ARGB8888, which is what the copy writes today. Scanout from a
  surface means writing that channel's base, size and the window's position, then
  a shadow update for window 5.
- **Batches.** 530 live draws in 523 commits. They ended at a CPU draw 445 times,
  at another destination 64 times, and at the pre-hooks 14 times in all. So the
  pre-hooks are not the problem; interleaved CPU draws are.
- No stall, no GPU fault. One known webbookmarksd abort at 900 s.
