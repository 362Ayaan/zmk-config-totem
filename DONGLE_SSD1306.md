# MX Totem dongle + SSD1306

This branch preserves the Keymap Editor keymap in `config/totem.keymap` and adds a modular ZMK dongle plus a temporary 128x64 I2C SSD1306 status display.

## Known-good baseline

- Last successful source commit: `162867fe34637a9be463dee1db1234d0e3802c02`
- GitHub Actions run: 17, completed 2025-11-22
- Local safety tag: `known-good-2025-11-22`
- The original Actions artifact expired on 2026-02-20. This branch therefore builds fresh rollback images from the preserved keymap.
- ZMK is pinned to `6e2ef41e022d555b10f116e395832913f71717b3` (2026-08-11).

## Wiring

Confirm that the module is the four-pin I2C 128x64 variant and that its header is labelled `GND`, `VCC`, `SCL`, `SDA` before applying power.

| SSD1306 | Spare XIAO nRF52840 | XIAO pin |
| --- | --- | --- |
| GND | GND | GND |
| VCC | 3V3 | 3.3 V only |
| SCL | D5 | P0.05 / I2C SCL |
| SDA | D4 | P0.04 / I2C SDA |

The overlay expects the common I2C address `0x3C`. If the physical module uses `0x3D`, change `reg = <0x3c>;` in `xiao_ssd1306.overlay` before building.

The SSD1306 uses a dedicated ZMK display work queue so live layer and WPM events cannot be delayed by split/USB work on the system queue.

## Left EC11 encoder

The hand-wired encoder is enabled only on the left half. For the encoder's three rotation pins, connect one outer pin to XIAO `D6`, the center/common pin to `GND`, and the other outer pin to `D7`. The outer pins may be swapped if the direction is reversed. The reversible TOTEMX footprint mirrors these nets onto the PCB underside, so backside soldering is valid after confirming each pad by continuity to the corresponding XIAO castellated pin.

The firmware uses 20 detents per rotation and sends consumer volume up/down on every layer. The encoder push switch is not configured.

## Firmware artifacts

| Artifact | Device | Purpose |
| --- | --- | --- |
| `dongle-ssd1306.uf2` | Spare XIAO only | BLE split central, USB keyboard output, SSD1306 status screen |
| `left-peripheral.uf2` | Left keyboard half | Converts left half from central to BLE peripheral |
| `right-peripheral.uf2` | Right keyboard half | BLE peripheral firmware |
| `settings-reset.uf2` | Each XIAO, one at a time | Clears old Bluetooth/split bonds |
| `rollback-left-central.uf2` | Left keyboard half | Restores the original left-central architecture |
| `rollback-right-peripheral.uf2` | Right keyboard half | Restores the matching right peripheral |

## Safe flashing order

Do not flash either keyboard half until the dongle firmware builds and the OLED has initialized on the spare XIAO.

1. Wire the OLED with all devices unplugged.
2. Flash `settings-reset.uf2` to the **spare XIAO**, then flash `dongle-ssd1306.uf2` to that same spare XIAO.
3. Verify that USB enumerates and the OLED shows the built-in ZMK status screen. At this stage the existing keyboard is unchanged.
4. Turn both keyboard halves off.
5. Flash `settings-reset.uf2`, then `left-peripheral.uf2`, to the **left keyboard half**.
6. Flash `settings-reset.uf2`, then `right-peripheral.uf2`, to the **right keyboard half**.
7. Disconnect/reconnect the dongle. Power on the left half first and wait for it to connect, then power on the right half.

## Rollback

1. Turn off both halves and unplug the dongle.
2. Flash `settings-reset.uf2`, then `rollback-left-central.uf2`, to the left half.
3. Flash `settings-reset.uf2`, then `rollback-right-peripheral.uf2`, to the right half.
4. Power on the left half first, then the right half, and re-pair the left half with the computer if required.

The corrected EC11 implementation was added in commit `a0d570e` and validated by GitHub Actions run 41.
