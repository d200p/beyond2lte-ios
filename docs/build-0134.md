# Build 0134: narrow ACPM dump

Status: booted once; the dump finished. Contains everything in
[build 0133](build-0133.md).

The dump now reads the framework header and channels 5 and 9 only, and follows
no offsets. Each step reaches the persistent log before the next, so a stall can
be dated to one group of reads. The plugin table is not read in this build.

## Device result

```
FW: plugins 5d5c/9 channels 5f0c/14 ap_max 11
CH 5: id5 field20 owner4 type2; len1 elem10 credit2 ap_poll0
CH 9: id9 field200 owner40 type1; len7 elem10 credit2 ap_poll1
```

- The table offsets were right. Channel 5 is a one-element buffer (type 2), not
  a queue: it is `acpm_noti_mif`, the firmware telling the AP about MIF. The
  device tree's `acpm_dvfs` node names it for that alone.
- Rate requests go out on the channel of the clock controller's node,
  `acpm-ipc-channel = <0>`. Channel 1 belongs to the DVFS manager. 0135 uses 0.
- 198 s without a freeze, 93 live draws, 0 failures, no GPU fault, no backboardd
  crash. The recovery chord worked.
