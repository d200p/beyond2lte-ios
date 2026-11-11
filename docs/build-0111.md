# Builds 0111 and 0112: synchronous dirty-span present

Status: BOOT images built offline; not yet booted. Root, firmware and loader
are unchanged.

## What 0109 did on the device

The kernel log of the 0109 boot shows the present path working but
backboardd almost silent:

| | 0108 | 0109 |
| --- | ---: | ---: |
| First frame on the panel | about 136 s | about 100 s |
| Swaps per 10 s once the UI is up | 13 to 108 | 0 or 1 |
| Time to copy a full frame | about 16 ms | 10 ms full, 3 ms unchanged |
| Bytes written for an unchanged frame | 17 MB | 0 |

After the first frame, backboardd presented one more identical frame, and the
log ends about 25 seconds later with no panic. The dirty-span copy behaved as
designed. The difference that remains is the contract: in 0109 SwapEnd
returned before the copy. IOMobileFramebuffer has no swap-complete
notification in this driver, so Core Animation may treat an early return
differently. The cause is not proven.

## Change

The copy runs inside SwapEnd again (`fCopyCall` stays NULL), with the shadow
and dirty spans kept. Unchanged pixels are still never written to the panel,
so a frame costs the compare plus the changed rows instead of a full 17 MB
write.

## Images

- **0111**: 0110 sources with the battery driver stubbed out in the build
  tree only, to test the display change on its own.
- **0112**: the full sources, including the battery driver from
  [build 0110](build-0110.md).

Boot 0111 first. If the screen comes up, boot 0112.
