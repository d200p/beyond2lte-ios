# Build 0138: platform UUID without corecrypto

Status: booted once. The UUID is published; Settings still hangs. Contains
everything in [build 0136](build-0136.md) and the fix from [build 0137](build-0137.md).

The UUID is mixed with plain arithmetic, because corecrypto is not registered
when the platform expert starts. `tests/test_platform_uuid.py` now fails if SHA1
or crypto returns to that path.

## Reading the log

```
B2L: IOPlatformUUID published, unit-specific
```

| Sign | Meaning |
| --- | --- |
| Log and data volume unchanged after the boot | It still dies before storage: flash 0136 back. |
| About opens, no new `0x8BADF00D` for Settings | The fix works. |
| Settings hangs and configd is not in `gethostuuid` | A second cause sits behind this one. |

## Device result

`IOPlatformUUID published, unit-specific` is in the log. 299 s, 31 live draws, 0
failures, no GPU fault. Settings was still killed with `0x8BADF00D` at 230 s and
340 s, at the same place: `__wait_for_IOKit_to_quiesce`. Removing `gethostuuid`
was necessary or not, but it was not enough. These reports carry no whole-system
stackshot, so configd's state is unknown; [build 0139](build-0139.md) records it.
