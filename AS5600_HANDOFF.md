# AS5600 wireless scroll dial: engineering handoff

## 1. Current state and unresolved symptom

The project builds a Seeed XIAO nRF52840 plus AS5600 magnetic dial as a third
ZMK BLE split peripheral. The existing left and right keyboard halves connect
to a dedicated XIAO dongle, and the keyboard otherwise works. The AS5600 dial
firmware builds and flashes, but rotating the knob currently produces no
visible scroll.

The successful cloud build proves that Kconfig, devicetree, the custom driver,
the dial shield, the dongle, and all other images compile together. It does not
prove that the physical dial joined the dongle or that the AS5600 is responding
on the installed hardware.

There are three distinct stages to verify:

1. AS5600 -> dial XIAO over I2C.
2. Dial XIAO -> dongle over ZMK split BLE.
3. Dongle input listener -> host HID vertical wheel report.

Do not change scroll tuning until these three stages have been separated.

## 2. Repository and reproducible cloud build

- Repository: <https://github.com/362Ayaan/zmk-config-totem>
- Working branch: `codex/dongle-ssd1306`
- Latest functional firmware commit: `738939b34399eb2f4dcaba8a426851fc89ac3d92`
- Commit subject: `Move AS5600 SCL to D3 and add AGC diagnostics`
- Final successful Actions run:
  <https://github.com/362Ayaan/zmk-config-totem/actions/runs/32654223297>
- Merged artifact name: `firmware`
- Artifact SHA-256:
  `78c7cce30eb6216ac8812f61510386058fbaa5e508bd6d8e19d62ed86f33c418`
- Artifact expiry: 2026-11-21

The repository workflow is `.github/workflows/build.yml`. It delegates to
ZMK's reusable user-config workflow:

```yaml
jobs:
  build:
    uses: zmkfirmware/zmk/.github/workflows/build-user-config.yml@main
```

The actual ZMK source is not floating. `config/west.yml` pins it to:

```text
6e2ef41e022d555b10f116e395832913f71717b3
```

Pushing a commit to the branch starts the Actions matrix in `build.yaml`. A
valid final run must show success for all seven build jobs and for `Merge
Output Artifacts`. The matrix produces:

| Artifact | Target |
|---|---|
| `dongle-ssd1306.uf2` | Dedicated XIAO dongle and OLED |
| `left-peripheral.uf2` | Left keyboard half |
| `right-peripheral.uf2` | Right keyboard half |
| `as5600-dial-peripheral.uf2` | AS5600 dial XIAO |
| `settings-reset.uf2` | Clears bonds/settings on any XIAO |
| `rollback-left-central.uf2` | Restores old no-dongle architecture |
| `rollback-right-peripheral.uf2` | Matching rollback right half |

Only the first five are used for the dongle-plus-dial architecture. The two
rollback images are not part of normal flashing.

### What was required to make the module build

The repository is a Zephyr module, not just a collection of shield overlays:

- `zephyr/module.yml` registers the repository CMake, Kconfig, board root, and
  devicetree root.
- Root `CMakeLists.txt` and `Kconfig` include `drivers/`.
- `dts/bindings/input/ayaan,as5600-scroll.yaml` defines the custom compatible
  and all tuning properties.
- `drivers/input/Kconfig` enables the driver only when an enabled
  `ayaan,as5600-scroll` node exists.
- `drivers/input/CMakeLists.txt` compiles the driver only when selected.
- `Kconfig.shield`, `Kconfig.defconfig`, and `totem.zmk.yml` register
  `totem_dial` as a real sibling shield.
- The common `dial_split` device is disabled by default. It is enabled only in
  the dial and dongle overlays. This matters because a split peripheral with
  an enabled input-split node but no physical `device` property fails a ZMK
  build assertion; the left and right halves must therefore leave it disabled.
- The first dial build found one linker portability issue: the pinned Zephyr
  revision did not provide `ABS` as the expected macro. Commit `6d777d9`
  replaced it with explicit signed-magnitude arithmetic. The subsequent dial
  and full matrix builds succeeded.

The untracked local `artifacts/` and `refs/` directories are reference/build
material and were deliberately not committed.

## 3. Flashing and pairing procedure

`settings-reset.uf2` and an operational UF2 must be copied separately. The
board reboots after processing the first UF2, so copying both at once is not a
valid shortcut.

1. Power down or disconnect the complete system.
2. Enter the dongle bootloader and flash `settings-reset.uf2`.
3. Enter the left-half bootloader and flash `settings-reset.uf2`.
4. Enter the right-half bootloader and flash `settings-reset.uf2`.
5. Enter the dial bootloader and flash `settings-reset.uf2`.
6. Re-enter the dongle bootloader and flash `dongle-ssd1306.uf2`.
7. Leave the dongle powered over USB.
8. Re-enter the left bootloader and flash `left-peripheral.uf2`; power it and
   wait about 15 seconds.
9. Re-enter the right bootloader and flash `right-peripheral.uf2`; power it and
   wait about 15 seconds.
10. Re-enter the dial bootloader and flash `as5600-dial-peripheral.uf2`; power
    it and wait 30-60 seconds.

The dongle is configured with
`CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS=3`, `BT_MAX_CONN=8`, and
`BT_MAX_PAIRED=8`. A settings reset of only the dial is not sufficient when
changing the central's split topology; all four devices must have mutually
compatible fresh bonds.

## 4. Final wiring and AS5600 bring-up facts

The installed XIAO has a failed D5 pin. The overlay contains an actual nRF
TWIM pin-control override; this is not merely a documentation change. SCL is
routed to D3 and SDA remains on D4. The bus is configured for 400 kHz.

| AS5600 module | XIAO nRF52840 |
|---|---|
| VCC | 3V3 |
| GND | GND |
| SDA | D4 |
| SCL | D3 |
| DIR | **GND — must NOT be left floating** |
| OUT | leave open |

**DIR must be tied, not left open.** Leaving it floating was the cause of
the erratic scrolling. The AS5600 DIR pin selects rotation direction: low
gives `angle`, high gives `4095 - angle`. Floating, it acts as an antenna,
picks up 50 Hz mains hum, and flips the reported angle between a value and
its 12-bit complement 50 times a second.

The signature is unmistakable once you look for it: the two alternating
states always sum to 4095. Measured pairs were 3546/549 and 2790/1305 on
the bench and 3306/790 and 3249/847 over ZMK. It also explains why the
swing amplitude appeared to change when the magnet moved -- the span is
`|2A - 4095|`, so it varies with resting angle, which is easy to misread as
the magnet getting better or worse.

AGC is what settles it. With DIR floating and AGC a healthy 78, the angle
still flipped full-scale. A weak magnet cannot read 78, so field strength
was never the fault. Tie DIR to GND (or to 3V3 to reverse direction).

Important bring-up rules from the Arduino/Claude investigation:

- The AS5600 address is fixed at `0x36`.
- It acknowledges at `0x36` even when no magnet is present. No ACK therefore
  means power, ground, SDA, SCL, a solder joint, or the module itself; it is not
  caused by magnet placement.
- It exposes a 12-bit absolute angle: 0-4095 counts per revolution.
- 400 kHz I2C worked and is within the intended operating mode.
- AGC is the field-strength trust metric. In 3.3 V mode it spans 0-128; the
  practical target used here is 30-90.
- The hand-held assembly measured AGC 38. That is promising but not final.
  Measure again after the magnet is clamped into the printed case because
  centering and vertical air gap directly change field strength.
- Apparent stationary drift at roughly 50 Hz is a diagnostic hint to inspect
  AGC and magnet gap/alignment first. It is not evidence of an electrical I2C
  fault when the device continues to ACK normally.
- Multi-turn count is reconstructed in firmware by accumulating wrapped
  differences between successive 12-bit samples. That accumulated turn count
  is lost on reset unless explicitly persisted. This scroll implementation
  does not require persistent turn count.

The standard magnetic arrangement is a centered rotating field above the
sensor. A diametrically magnetized cylindrical magnet centered on the rotation
axis is the simplest arrangement. An axially magnetized magnet can only be
used when the mechanical mounting/orientation makes the rotating transverse
field seen by the chip appropriate; the Scott Bezek reference design used a
mechanical arrangement for an axial magnet because that was the magnet on
hand. In either case, AGC and stable angle readings are more useful than the
magnet label alone.

## 5. Firmware architecture and scroll algorithm

This implementation intentionally uses the modern Zephyr input path:

```text
AS5600 I2C angle
  -> ayaan,as5600-scroll input driver
  -> INPUT_REL_WHEEL event
  -> zmk,input-split reg 0 on dial peripheral
  -> ZMK split BLE input characteristic
  -> zmk,input-split reg 0 proxy on dongle
  -> zmk,input-listener on dongle
  -> HID vertical wheel report to host
```

The dial driver reads raw angle registers `0x0C/0x0D`. It computes a signed
wrapped delta in the range approximately -2048 to +2048, so crossing
4095 -> 0 remains continuous. It then applies speed-dependent gain and keeps a
remainder accumulator so sub-wheel-unit motion is not discarded.

Current tuning from `totem_dial.overlay`:

| Setting | Value | Meaning |
|---|---:|---|
| Poll interval | 4 ms | 250 samples/second |
| Counts per scroll unit | 32 | About 128 low-speed units/revolution |
| Acceleration threshold | 1800 counts/s | About 0.44 rev/s |
| Acceleration multiplier | 2x | Used from 1800 through 6999 counts/s |
| Fast threshold | 7000 counts/s | About 1.71 rev/s |
| Fast multiplier | 4x | Used at or above 7000 counts/s |

The dongle enables `CONFIG_ZMK_POINTING=y` and
`CONFIG_ZMK_POINTING_SMOOTH_SCROLLING=y`, allowing hosts that support the HID
resolution multiplier to use smoother wheel reports.

Direction can be reversed by adding `invert-scroll;` inside the `as5600@36`
node. Direction is not the current problem: a reversed mapping would scroll in
the wrong direction, whereas the reported symptom is no event at all.

## 6. Why the keymap editor shows no new sensor

This is expected and does not indicate a failed build or failed connection.

The two EC11 encoders are legacy Zephyr sensor devices listed under
`zmk,keymap-sensors`; their actions are selected through `sensor-bindings` in
the keymap. The AS5600 is different: it is registered as a Zephyr input device
that emits `INPUT_REL_WHEEL`, then is forwarded using `zmk,input-split` and
consumed by a `zmk,input-listener` on the dongle.

Input devices do not become keymap `sensor-bindings` entries, and the keymap
editor is not a split-peripheral connection monitor. No AS5600 entry is
expected in that UI, and no keymap binding is required for its vertical scroll
output.

## 7. How to prove whether the dial was adopted

The current OLED is not sufficient. Its custom screen reads battery slots 0
and 1 only and labels them left and right. It does not display peripheral slot
2, the dial, nor a connected-child count. Seeing normal left/right batteries
therefore proves nothing about dial adoption.

The most decisive check is a temporary USB-logging build. Enable logging on
both the dial and dongle while keeping the normal release images unchanged:

```ini
CONFIG_ZMK_USB_LOGGING=y
CONFIG_ZMK_LOG_LEVEL=4
```

Connect each board by USB and inspect its CDC serial log. In the pinned ZMK
source, useful dongle messages include:

- `Found the split service`
- `Found an input characteristic`
- `All devices are connected, scanning is unnecessary`
- `Got peripheral event for 0!`
- `Failed to subscribe to input notifications ...` when subscription fails

The first three prove discovery/adoption. `Got peripheral event for 0!` proves
that the dongle received an input event from the registered dial input split.

The existing custom dial driver logs these one-time diagnostics when logging is
enabled:

- `AS5600 did not ACK at 0x36; check 3V3/GND, SDA D4, and SCL D3`
- `AS5600 ACKed at 0x36, but no magnet is detected`
- `AS5600 AGC=... (3.3 V target 30-90)`
- An out-of-range AGC warning

For the fastest full proof, temporarily add one debug log containing raw
angle, wrapped delta, and emitted wheel value. Do not leave a 250 Hz log in the
release firmware; rate-limit it or log only when a nonzero wheel event is
emitted.

## 8. Diagnosis decision tree

### A. Dial does not ACK at `0x36` in the known-good Arduino test

This is hardware, not BLE or ZMK. Check 3V3, common ground, SDA D4, and SCL D3
first. Confirm the operational wiring still matches the test sketch. A missing
magnet cannot cause no-ACK.

### B. Dial ACKs and raw angle changes in Arduino, but ZMK dial logs no ACK

Compare the generated Zephyr devicetree with `totem_dial.overlay`. Confirm it
contains the custom default/sleep pinctrl states and 400000 Hz clock. Confirm
the flashed dial UF2 came from run `32654223297`, not the earlier D5 build.

### C. Dial ZMK logs valid AGC/angle, but dongle never reports the third child

This is split bonding/adoption. Flash `settings-reset.uf2` to all four XIAOs
again, one device at a time, then reflash and power in the documented order.
Inspect the dongle log for three connections and for the input characteristic.

### D. Dongle sees three children but never says `Found an input characteristic`

The dial may be running the wrong image, or the input-split feature was absent
from that build. Confirm the dial job's generated Kconfig contains
`CONFIG_ZMK_POINTING=y`, `CONFIG_INPUT=y`, and `CONFIG_ZMK_INPUT_SPLIT=y`.
Confirm the dial devicetree has enabled `dial_split@0` with
`device = <&as5600_scroll>`.

### E. Dongle finds the input characteristic, but receives no peripheral event

The BLE connection is working. Instrument the dial driver's raw angle/delta and
`input_report_rel()` call. If angle is static, investigate the AS5600 and
magnet. If wheel events are emitted locally but absent centrally, instrument
`split_input_handler_0` and the peripheral transport return path.

### F. Dongle logs `Got peripheral event for 0!`, but the host does not scroll

The sensor and BLE child are proven. Inspect `dial_listener` on the dongle,
the selected USB/BLE output endpoint, and HID mouse reports. Test over USB
first to remove host Bluetooth profile/cache behavior. Remove/re-enumerate the
host device if its old HID report descriptor was cached before pointing support
was enabled.

## 9. Most likely next action

Do not infer adoption from the keymap editor or existing OLED. Build two
temporary diagnostic images—dial and dongle—with USB logging enabled. First
confirm the dial prints a valid AGC and changing raw angle. Then confirm the
dongle prints three connected peripherals, discovers the input characteristic,
and receives register-0 input events. That single test will identify the
failing stage without changing acceleration, scroll resolution, or magnet
mechanics prematurely.
