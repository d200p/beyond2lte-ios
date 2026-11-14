# Build 0143: JavaScript conversion, stale JIT code, float images

Status: booted once with no crash reports. Contains everything in
[build 0142](build-0142.md).

## FJCVTZS

HeuristicInterpreter has crashed on every boot since [0107](build-0107.md) with
`EXC_BAD_INSTRUCTION` on `0x1e7e0013`, `fjcvtzs w19, d0`. It is the ARMv8.3
JavaScript conversion, compiled into iOS's arm64e JavaScriptCore, and these cores
lack it. The undefined-instruction path now emulates it, including PSTATE.Z.
`tests/test_fjcvtzs.py` checks it against an exact integer model on 60,000 values.

## Stale JIT code

WebContent died with SIGILL on `0x321703f0`, a valid `orr`, in JIT code. The
cache's `sys_icache_invalidate` is only `ic ivau`, with no data cache clean: Apple's
cores do not need one, and these do. So freshly written code can be fetched as the
bytes that were there before.

When an undefined instruction cannot be emulated, the kernel cleans the page through
its physical alias, invalidates the instruction caches on all CPUs and retries once
for that thread and address. A truly undefined instruction still raises its signal.
This only catches stale bytes that trap. Stale but valid old code would run silently;
fixing that needs a change to the root's `sys_icache_invalidate`.

## Image formats

- Format 13 has 8 bytes per texel, 204 pixels in 1632 bytes, so it is read as
  RGBA16F (Mali format 223) and staged at that size.
- Format 11 has red and blue swapped from BGRA, so it is read as RGBA8.
- Both stay in shadow mode until they compare clean.
- `MAX_QUADS` rises from 16 to 48: multi-quad draws were 7% of 0141's draw time.

## Reading the log

| Sign | Meaning |
| --- | --- |
| No HeuristicInterpreter report on `0x1e7e0013` | FJCVTZS works. |
| `B2L COMPAT: stale code retried`, no new WebContent SIGILL | The diagnosis was right. |
| `gpu: submit ... stopping` | A job faulted, probably on the float format. The GPU path is off for the boot. |
| `wr` near 0 for formats 11 and 13 | They can go live. |

## Device result

About 415 s, 218 live draws, 0 failures, no GPU fault, and no new crash report.

- **FJCVTZS.** HeuristicInterpreter started and did not crash, for the first time
  since 0107.
- **Formats.** Format 13: 56 of 56 within tolerance. Format 11: 86 exact and 22
  within tolerance of 108. No job faulted on RGBA16F. These samples were small,
  at 8 kpix.
- **Multi-quad draws.** The 48-quad limit removed `why=10` from the top rejects.
- **Stale code.** No WebContent started, so the retry was not exercised.
- **Format 9.** One byte per texel: 94 pixels in 96 bytes. It looks like an alpha
  mask, used with kind 3 for text and shapes, and it is 13% of draw time. It needs
  an R8 texture and a shader that multiplies by a full colour.
