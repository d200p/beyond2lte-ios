# Build 0108: sandbox identity fix and spawn containers

Status: BOOT image built offline; not yet booted. The root image is the 0107
root (with the Core Animation patch). Firmware and loader are unchanged.

## Entitlement lookup bug

`b2l_sandbox_identity.h` fetched entitlements with
`b2l_amfi_proc_copy_dict`, which returns a plain dictionary, and queried it
with `b2l_ent_bool` and `b2l_ent_copy_object`, which only accept the
entitlement object and return false for anything else. Every process read as
unentitled, so Sandbox operations 2 and 4 kept their old answers. The new
`b2l_amfi_proc_copy_ent` returns the retained entitlement object, and the
identity code uses it. The host test now mocks only that accessor, and
`test-lifecycle.py` checks it against the real AMFI source.

## Container assigned at spawn

On iOS a sandboxed process's container is not in its environment when it
starts. launchd passes it to the policy named "Sandbox" as spawn attributes
(libsandbox `sandbox_spawnattrs_setcontainer`: path length at offset 12, path
at offset 80, under 1024 bytes). The B2L Sandbox policy now records it in
`mpo_proc_check_launch_constraints`, which XNU calls for every policy with
that policy's own attributes, and forgets it at exit
(`b2l_sandbox_container.h`). Operation 4 returns the caller's recorded
container first and falls back to `HOME`. A process with a recorded container
also reads as sandboxed for operation 2. Nothing is enforced.

The kernel log shows at most 32 lines of the form
`B2L Sandbox: spawn attributes ... container recorded|absent` and
`B2L Sandbox: container query answered from spawn|HOME`, with sizes only.

## Present-path measurement

The software renderer's frames reach the panel through the framebuffer
driver: each swap copies every updated layer, unscaled, into the
write-combined panel memory of the 1440x3040 display, inside the swap call.
There was no timing for this path. The driver now logs, every 10 seconds for
the first 10 minutes:

```
B2L FB PERF: vsync=... swaps=... copies=... full=... kpix=... copy_ms=... max_copy_us=...
```

These numbers decide whether presenting or rendering limits the frame rate.

## Device checks

- The speech-synthesizer audio units and MessagesBlastDoorService launch
  without crash reports.
- The kernel log shows containers recorded and container queries answered.
- backboardd stays free of aborts after Home presses.
- `B2L FB PERF` lines appear; compare swaps per 600 vsyncs and copy time.
