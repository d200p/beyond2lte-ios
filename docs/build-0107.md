# Build 0107: sandbox identity and the backboardd abort

Status: booted once with the patched root. The backboardd fix works; the
sandbox identity change had no effect because of a bug fixed in
[build 0108](build-0108.md). Firmware and loader are unchanged from 0105.

## Sandbox identity (kernel)

Two services refused to run on the unconfined Sandbox policy:

- MessagesBlastDoorService asserts "BlastDoor cannot run unsandboxed." unless
  `sandbox_check(getpid(), NULL, 0)` reports a sandbox.
- The speech-synthesizer audio units (ExtensionKit appex) abort in PlugInKit
  with "plugin has invalid sandbox or container" when
  `sandbox_container_path_for_pid` fails.

Sandbox operation 2 now reports "sandboxed" for an identity query (NULL
operation) when the target's entitlements request a sandbox. Operation 4
returns the caller's own `HOME` directory, which launchd points at the
container, under the same entitlement condition. Permission queries still
answer "allowed" and nothing is enforced. This is an owner-approved exception
to the no-fabrication rule; see [sandbox-policy.md](sandbox-policy.md).

## backboardd abort (root patch)

backboardd asserted `found != slist->end ()` in
`CA::OGL::Context::push_surface` after Home presses (three times per boot in
0105 and 0106), and each abort restarted SpringBoard. The cause is in Core
Animation's software renderer, which this port uses because there is no GPU:
backboardd loads neither Metal nor OpenGL ES.

- `CA::OGL::AsynchronousNode::retain_surface` creates an IOSurface, wraps it
  through context vtable slot 92, and immediately calls `push_surface`.
- `push_surface` requires the wrapped surface to be on the context's surface
  list.
- The Metal context's slot-92 wrapper links the new surface onto that list.
  The software context's wrapper (IOSurfaceLock plus base address) does not.
  None of the five slot-92 call sites inserts the surface itself.
- Removing a surface already unlinks listed surfaces, so a listed software
  surface is handled exactly like a Metal one.

The earlier IOSurface purgeable-state fix (0106) was a wrong hypothesis and is
not involved.

`scripts/patch-ca-software-surface.py` makes the software wrapper behave like
the Metal one. The wrapper's entry instruction branches to a 92-byte routine
that calls the unchanged wrapper body and, for a non-NULL result, calls
QuartzCore's own list-insert function unless the surface is already listed.
The routine lives in an all-zero 4 KiB page outside every image in a text
mapping, the rule `b2l-precompile` uses for its PACGA routines. The script
accepts only the precompiled iOS 26.6.2 (23G90) cache: the cache UUID and both
functions' bytes must match. It runs after `b2l-precompile --write`:

```sh
python3 scripts/patch-ca-software-surface.py /Volumes/<root> --dry-run
python3 scripts/patch-ca-software-surface.py /Volumes/<root>
python3 scripts/patch-ca-software-surface.py /Volumes/<root> --verify
python3 scripts/patch-ca-software-surface.py /Volumes/<root> --undo
```

`tests/test_ca_software_surface.py` runs patch, verify, re-patch, undo and the
refusal cases on a synthetic cache, and checks the routine's disassembly.

## Device result

| | 0106 | 0107 |
| --- | ---: | ---: |
| Home presses | 9 | 8 |
| backboardd aborts | 3 | 0 |
| MessagesBlastDoorService crashes | 6 | 5 |
| Speech-synthesizer audio unit crashes | 32 | 32 |

The speech units still logged "invalid sandbox or container: error 2", and
BlastDoor still stopped at its sandbox assertion. The identity code passed the
plain entitlement dictionary to queries that only accept the entitlement
object, so no process ever read as sandboxed. Build 0108 fixes this.

Two new SIGILL crashes appeared once WebKit ran: JavaScriptCore executes
`fjcvtzs` (ARMv8.3 JavaScript conversion), which the Exynos cores lack and the
kernel emulator does not handle, and a WebContent JIT page faulted on an
ordinary instruction that still needs investigation.
