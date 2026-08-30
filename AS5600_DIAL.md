# AS5600 scroll dial

For repository provenance, implementation details, and a staged diagnosis of a
non-scrolling dial, see [AS5600_HANDOFF.md](AS5600_HANDOFF.md).

This repository builds a standalone Seeed XIAO nRF52840 + AS5600 dial as the
third BLE peripheral of the existing TOTEM dongle. The sensor becomes a Zephyr
input device, emits accelerated vertical-wheel events, and uses ZMK's standard
input-split transport to reach the dongle.

## Wiring

Power both boards off before connecting wires.

**Fault and fix:** D5 is dead on this XIAO, so the firmware intentionally moves
SCL to D3. If a build reports no device at `0x36`, check this wiring first.

| AS5600 module | XIAO nRF52840 |
|---|---|
| VCC | 3V3 |
| GND | GND |
| SDA | D4 |
| SCL | D3 |
| DIR | GND (do not leave floating) |
| OUT | leave open |

Use 3.3 V, not 5 V. Many AS5600 breakout boards pull SDA and SCL up to VCC, so
powering such a board at 5 V can put 5 V on the XIAO's 3.3 V GPIO. The fixed
AS5600 I2C address is `0x36`. It acknowledges at that address even when no
magnet is present, so a missing ACK is always a wiring or power problem. The
bus runs at 400 kHz, which the sensor supports.

Mount a diametrically magnetized magnet centered over the AS5600. If the dial
does not respond or behaves erratically, magnet placement and air gap are the
first things to check.

## Bring-up diagnostics

The AS5600 reports a 12-bit absolute angle. Magnet quality is judged with AGC,
which spans 0-128 at 3.3 V; the useful target is 30-90. The hand-held test read
38, which is good, but re-check it after the magnet is clamped in the printed
case because the final gap can change it.

If a stationary angle alternates between two complementary values that sum to
4095, verify that DIR is firmly tied to GND. A floating DIR pin can pick up
mains noise and repeatedly reverse the reported angle even when AGC and magnet
alignment are healthy.

Multi-turn position is accumulated by firmware from consecutive 12-bit angle
samples. That accumulated turn count is lost on reset; an application that
needs absolute turn count to survive reboot must persist it separately. The
scroll-dial firmware does not need persistent turn count.

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
