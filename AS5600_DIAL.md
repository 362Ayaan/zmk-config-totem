# AS5600 scroll dial

This repository builds a standalone Seeed XIAO nRF52840 + AS5600 dial as the
third BLE peripheral of the existing TOTEM dongle. The sensor becomes a Zephyr
input device, emits accelerated vertical-wheel events, and uses ZMK's standard
input-split transport to reach the dongle.

## Wiring

Power both boards off before connecting wires.

| AS5600 module | XIAO nRF52840 | Purpose |
|---|---|---|
| VCC | 3V3 | 3.3 V power |
| GND | GND | Ground |
| SDA | D4 / SDA | I2C data |
| SCL | D5 / SCL | I2C clock |
| DIR | leave open | Direction is handled in firmware |
| OUT | leave open | The firmware uses I2C, not analog/PWM output |

Use 3.3 V, not 5 V. Many AS5600 breakout boards pull SDA and SCL up to VCC, so
powering such a board at 5 V can put 5 V on the XIAO's 3.3 V GPIO. The fixed
AS5600 I2C address is `0x36`.

Mount a diametrically magnetized magnet centered over the AS5600. If the dial
does not respond or behaves erratically, magnet placement and air gap are the
first things to check.

## Firmware artifacts

The build matrix now produces:

- `as5600-dial-peripheral.uf2` for the dial XIAO
- `dongle-ssd1306.uf2` updated to accept three split peripherals
- the existing left and right peripheral images
- `settings-reset.uf2` for clearing stale bonds

Because the dongle's peripheral count changed from two to three, clear the old
split bonds before pairing the new topology:

1. Flash `settings-reset.uf2` to the dongle, left half, right half, and dial.
2. Flash the new `dongle-ssd1306.uf2` to the dongle.
3. Flash the new left and right peripheral images to their matching halves.
4. Flash `as5600-dial-peripheral.uf2` to the dial XIAO.
5. Power the dongle first, then the two halves and dial; allow time for all
   three peripherals to pair.

Enter the XIAO bootloader by quickly pressing reset twice, then copy the
matching UF2 to the mounted `XIAO-SENSE`/UF2 drive. The exact drive label may
vary with bootloader version.

## Tuning

Defaults live in `config/boards/shields/totem/totem_dial.overlay`:

- `counts-per-scroll = <32>` gives about 128 low-speed wheel units/revolution.
- `acceleration-threshold = <1800>` enables 2x gain at about 0.44 rev/s.
- `fast-threshold = <7000>` enables 4x gain at about 1.7 rev/s.
- `poll-interval-ms = <4>` samples at 250 Hz.

Increase `counts-per-scroll` for slower/finer scrolling. Reduce the multipliers
or raise the thresholds for gentler acceleration. If rotation is backwards,
add the boolean property `invert-scroll;` inside the `as5600@36` node.
