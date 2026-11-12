# Build 0137: platform UUID

Status: did not boot; replaced by [build 0138](build-0138.md). Contains everything
in [build 0136](build-0136.md).

## Settings > General > About froze

About asks whether the device has Ethernet, which reaches
`_SCNetworkInterfaceCopyAllWithPreferences` and waits on the main thread for
configd's InterfaceNamer to report. FrontBoard then kills Settings with
`0x8BADF00D`.

configd never reports. All 25 stackshots kept on the device, from builds 0107 to
0126, show `configd`, `timed` and `rapportd` asleep in syscall 142, `gethostuuid`.
That call reaches `IOBSDGetPlatformUUID`, which waits with no timeout for the
`IOPlatformUUID` resource. XNU derives it only from `unique-chip-id` and `chip-id`
in `/chosen`, which iBoot provides and this boot chain does not, so it was never
published. configd's main thread serves SCDynamicStore, so every client stalled
behind it.

## Change

`B2LPlatformExpert` publishes `IOPlatformUUID` when XNU has not. It is a hash of
the Wi-Fi address in `/chosen`, so it stays the same across reboots, and is a
constant if that address is missing. The UUID is not logged. `/chosen` is left
alone, so MobileGestalt sees no new chip id.

Releasing configd may change other behaviour, because this path has been stuck
since the first boots.

```
B2L: IOPlatformUUID published, unit-specific
```

## Tests

`tests/test_platform_uuid.py` runs the derivation and checks that the property
and the resource are both published, after the superclass start.

## Device result

The image never reached storage: the persistent log region and the data volume
were byte-identical to what 0136 left, and recovery started about 3 minutes
later. No log exists, so the cause below is read from the source, not observed.

The hash used `SHA1Init`, which dereferences `g_crypto_funcs`. That pointer is
NULL until corecrypto registers, and the platform expert starts before that;
patch 0003 guards MD5 for the same reason. The host test could not see this,
because it supplied its own SHA-1.
