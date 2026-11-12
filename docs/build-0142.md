# Build 0142: network stack user client

Status: booted once. Settings no longer hangs. Contains everything in
[build 0141](build-0141.md).

configd's InterfaceNamer opens `IONetworkStack` as soon as it matches, and goes on
to `*STACK*` only if `IOServiceOpen` succeeds. Disassembly of its callback shows
`IOIteratorNext`, then `IOServiceOpen`, then a branch on the result. 0142 adds
`IONetworkStackUserClient`, which has no methods: configd uses the connection only
to register interfaces, and this port has none.

| Sign | Meaning |
| --- | --- |
| Settings opens right after boot, no `0x8BADF00D` | The fix works. |
| Reports still stop in `__wait_for_IOKit_to_quiesce` | The next step after `*STACK*` needs reading. |

## Device result

- **Settings.** Launched at about 165 s with no `0x8BADF00D`, in a boot of about
  565 s. Every earlier launch before about 5 minutes hung, so the stack's user
  client is the fix.
- **Heat.** No full hold. The clocks changed 25 times, and with load at 6 to 17%
  the big sensor read 68 to 72 C.
- **GPU.** 707 live draws, 0 failures, no GPU fault. Sampled draws averaged 1765 us
  at the same pixel mix: map 277, flush 679, submit 569 (tiler 145, fragment 380)
  and drop 240.
- **New report.** One `com.apple.WebKit.WebContent` SIGILL at 180 s, in a JIT page
  called from `ElementRuleCollector`, on word `0x321703f0`, which is a valid
  `orr`. No cause yet. Earlier boots rarely started WebContent at all.
