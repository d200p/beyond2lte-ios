# Build 0110: battery status

Status: the driver runs on the device in build 0121 (two boots). Whether the
status bar shows the battery is still to be confirmed by the owner.

## How iOS finds a battery

powerd looks up `IOServiceMatching("IOPMPowerSource")` and
`IOServiceMatching("AppleSmartBattery")`, waits for the first match, and reads
the standard power-source properties (`CurrentCapacity`, `MaxCapacity`,
`IsCharging`, `ExternalConnected`, `FullyCharged`, `BatteryInstalled`,
`Voltage`, `Amperage`, `AppleRawCurrentCapacity`, `AppleRawMaxCapacity`,
`DesignCapacity`, `CycleCount`, `Temperature`, `TimeRemaining`). The status
bar and Settings get their battery state from powerd.

## Hardware

The stock device tree places the MAX77705 (`max77705@66`, with its
`max77705-fuelgauge`) on `hsi2c_45`: controller `0x15D60000` (CMGP USI03,
mode register `0x15C22030`), pins GPM12-0 and GPM13-0 in function 2, 200 MHz
input clock. Nothing in UEFI uses that bus.

## Driver

`B2LBattery` matches IOResources (patch 0060):

- **Bus.** If the pins, USI mode and fast-mode timing are already set, it
  reuses them. Otherwise it programs them exactly as the platform's UEFI
  `HsI2cDxe` would: pin function 2, USI mode I2C, `usi_con` cleared, a
  controller reset, 400 kHz timing (divider 33, timing bytes 0xE0 and 0xF0),
  master mode and auto mode. The boot log records the register state found.
- **Reads only.** Every 20 seconds it reads the fuel gauge at I2C 0x36
  (RepSOC, VCell, Current, AvgCurrent, RepCap, FullCapRep, DesignCap, Cycles,
  Temp, TTE, TTF) and the charger at 0x69 (CHG_INT_OK and CHG_DETAILS_00/01).
  No MAX77705 register is ever written.
- **Decoding** (`b2l_battery_model.h`): percentage from RepSOC, millivolts at
  78.125 uV per LSB, current and capacity scaled for the stock 5 mOhm sense
  resistor (`fuelgauge,fg_resistor = 2`). Charging means a valid charger input
  and a charge state of prequal, fast CC, fast CV or top-off. Full means state
  "done" or 100% while plugged in. Without a charger reading, an averaged
  current above 20 mA counts as charging.
- **Publishing.** It creates an `AppleSmartBattery` (an `IOPMPowerSource`)
  attached to itself, fills it, registers it after the first plausible
  reading, and calls `updateStatus()` on every poll so powerd is notified
  when a value changes.
- **Off switch.** Boot argument `b2l_battery=0` disables the driver.

The kernel log shows:

```
B2L BATTERY BUS: gpm12=... gpm13=... usi=... fs1=... fs2=... fs3=... conf=...
B2L BATTERY: HSI2C45 reused|configured
B2L BATTERY: 57% 3870mV -312mA avg=-298mA 2300/4000mAh design=4100mAh temp=2810 cycles=123 ext=0 charging=0 full=0 chg=1 int_ok=... d00=... d01=...
B2L BATTERY: AppleSmartBattery published
```

## Risks and open points

- If the CMGP USI03 clock were gated, the first controller access could stall
  the boot. S-Boot reads the same fuel gauge for its charging screen, so the
  clock is expected to be on. Reflashing the 0109 BOOT image recovers.
- Absolute mAh values assume the stock sense-resistor scaling; the
  percentage does not depend on it.
- The charger register meanings come from the MAX77705 family layout; the
  logged raw bytes allow checking them against plugged and unplugged states.

## Tests

`tests/test_battery_model.py` compiles the decoding header: the UEFI timing
formula for several clocks, unit conversions and rounding, every charger
state, the no-charger fallback, and plausibility limits.

## Device result (0121)

The bus was unconfigured at kernel start (pins in function 1, USI mode 0,
timing registers 0), so the driver configured it; every read succeeded.

| | Value |
| --- | --- |
| Charge | 99%, then 98% |
| Voltage | 4.22 to 4.27 V |
| Current | -0.4 to -0.9 A unplugged |
| Design capacity | 3958 mAh (cell rated 3930 to 4100 mAh) |
| Temperature | 31.3 °C |
| Charger plugged in | CHG_INT_OK 0x9f to 0xdf (CHGIN_OK), CHG_DETAILS_00 0x80 to 0xe0 (CHGIN valid), CHG_DTLS 0x8 to 0x1 (off to fast CC): `ext=1 charging=1` |

The charger register interpretation is confirmed. Two values look wrong:
the cycle count reads 0, and remaining capacity (RepCap) is about 1% above
full capacity (FullCapRep), so the mAh scaling needs a second look; the
percentage does not depend on it. powerd's battery-health reporting
(`BatteryHealthMetrics`, `poweruiagent`) appears in the unified log.
