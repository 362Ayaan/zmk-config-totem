# Merry dongle pet

This directory contains the compact pet pack generated for the 240x240 ST7789
dongle display.

- Source reference: <https://codex-pets.net/#/pets/merry>
- Pet credit: **Merry by jeansolopreneur**
- Source format: Codex Pets v1 atlas, 1536x1872, 8 columns by 9 animation rows
- Dongle format: native 192x208, 6 bits per pixel, stable 63-colour RGB565 palette per animation
- Pack: 30 frames / 5 animations / 902,720 bytes

The converter discards RGB values hidden below fully transparent source pixels;
those pixels are black/transparent in the dongle pack. It uses a stable palette
per animation, explicit cyan/blue/green/white/plum anchors, and OKLab palette
matching. Frames remain at the atlas's native resolution, avoiding a damaging
non-integer downsample; firmware applies only the small final nearest-neighbour
enlargement to 202x220. `qa/contact-sheet.png` is the deterministic visual check
of every converted frame.

The five animations are `Idle` (6 frames), `Running` (6), `NeedsInput` (6),
`Completed` (4), and `Blocked` (8). They map cleanly to the minimal state set a
future PC-side Codex bridge needs.

The source page describes this as a community-submitted asset and does not grant
rights to third-party characters. Keep the credit above and assess the rights
needed before redistributing the art or firmware bundle.

## Raw display diagnostic

`dongle-st7789-diagnostic.uf2` is a temporary hardware-isolation image. It uses
the same D0/D1/D2/D8/D10 wiring as the Merry target, but bypasses LVGL, the
Zephyr ST7789/SPI drivers, QSPI storage, and pet code. On boot it flashes the
backlight three times, performs a conservative reset, and drives the
ZJY-IPS130-V2.0 directly with GPIO-generated SPI mode 3 before cycling the full
panel through red, green, blue, white, and black. Reflash
`dongle-st7789-merry.uf2` after the test.

## Updating without reflashing ZMK

After the ST7789 firmware has been flashed once, Windows exposes the XIAO's CDC
serial port alongside the normal keyboard HID device. Windows assigns the COM
number dynamically; it is not compiled into the firmware. Close any serial
monitor, identify the port by unplugging/reconnecting the dongle or with Device
Manager, then run:

```powershell
.\tools\upload_merry_pet.ps1 -Port COM12
```

Replace `COM12` with the dongle's serial port. The uploader erases only the
inactive 1 MB QSPI slot, sends 512-byte CRC-protected chunks, validates the full
pack, and writes the commit header last. Disconnecting USB during an upload leaves
the prior slot valid; the embedded static Merry frame is used if neither slot is
valid.

Display mode, animation, brightness, timeout, and pet position can also be
changed persistently through the same COM port:

```powershell
.\tools\configure_merry.ps1 -Port COM12 -Mode Auto -Animation Idle -Brightness 80 -TimeoutSeconds 30
```

To regenerate the pack from the original downloaded atlas:

```powershell
& "<bundled-python>" .\tools\make_merry_petpack.py <spritesheet.webp> .\assets\merry
```
