# Galaxy S10 (beyond1lte) audio bring-up plan

Status: research only (2026-11-13). Nothing here runs yet.

## Hardware (live device tree, read-only)

| Block | Where | Notes |
| --- | --- | --- |
| ABOX audio subsystem | `abox@18c50000`, GIC `18cf0000` | Two DSP cores (`abox-core@18c55000/080`) with SRAM/DRAM firmware, 12 RDMA and 8 WDMA channels, 4 UAIF (I2S) and one DSIF |
| Codec | Cirrus Logic CS47L93 on `spi@10470000` | Headset, microphones, receiver paths |
| Smart amplifiers | 2x Cirrus Logic CS35L40, I2C 0x40 and 0x41 | Speaker ("bot") and receiver ("rcv"), each with a speaker-protection DSP |

Android's mixer configuration on this phone states that **UAIF0 goes to the
codec and UAIF1 to the speaker amplifiers**: UAIF1 runs at 48 kHz, 32-bit, 4
channels, and "media-speaker" routes RDMA7 -> SIFS1 -> UAIF1. Plain speaker
playback therefore needs ABOX and the two CS35L40, not the codec.

Firmware on the phone's own vendor partition (never copied into this
repository or CI): ABOX `calliope_sram.bin` / `calliope_dram.bin` (+ `_2`
variants), CS35L40 `*-dsp1-spk-prot.wmfw` with tuning and calibration `.bin`
files for both amplifiers, CS47L93 `cs47l93-dsp1-trace.wmfw`.

## iOS side

iOS 26's audio server (audiomxd) loads HAL plug-ins from
`/System/Library/Audio/Plug-Ins/HAL` (BuiltinAudioPlugin, BTAudioHALPlugin,
usbaudiodxpc, AirPlayHalogen, ...). Instead of imitating Apple's built-in
audio drivers, the port can add its own `AudioServerPlugIn` bundle that
exposes one output device and hands PCM to a kernel ring.

## Steps

1. Kernel test tone: ABOX power/clocks, RDMA7 -> SIFS1 -> UAIF1 (I2S master,
   48 kHz, 32-bit) from a kernel buffer, CS35L40 init over I2C in PCM bypass
   at low volume. Find out whether RDMA/UAIF routing works without the ABOX
   DSP firmware.
2. Kernel `B2LAudio` user client: shared PCM ring, start/stop, volume.
3. `B2LAudio.driver` HAL plug-in in the root; audiomxd picks it as the
   default output.
4. Later: receiver amp, codec (headset, microphones), speaker protection.

Register facts come from public GPL sources (Samsung's exynos9820 kernel,
upstream Linux cs35l41 driver) and are cited where used; no code is copied.
