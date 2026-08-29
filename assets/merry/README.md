# Merry dongle pet

This directory contains the compact pet pack generated for the 240x240 ST7789
dongle display.

- Source reference: <https://codex-pets.net/#/pets/merry>
- Pet credit: **Merry by jeansolopreneur**
- Source format: Codex Pets v1 atlas, 1536x1872, 8 columns by 9 animation rows
- Dongle format: 160x174, 4 bits per pixel, private 15-colour RGB565 palette per frame
- Pack: 57 frames / 9 animations / 795,832 bytes

The converter discards RGB values hidden below fully transparent source pixels;
those pixels are black/transparent in the dongle pack. `qa/contact-sheet.png`
is the deterministic visual check of every converted frame.

The source page describes this as a community-submitted asset and does not grant
rights to third-party characters. Keep the credit above and assess the rights
needed before redistributing the art or firmware bundle.

## Updating without reflashing ZMK

After the ST7789 firmware has been flashed once, Windows exposes a separate CDC
serial port alongside the normal keyboard HID device. Close any serial monitor,
then run:

```powershell
.\tools\upload_merry_pet.ps1 -Port COM12
```

Replace `COM12` with the dongle's serial port. The uploader erases only the
inactive 1 MB QSPI slot, sends 512-byte CRC-protected chunks, validates the full
pack, and writes the commit header last. Disconnecting USB during an upload leaves
the prior slot valid; the embedded static Merry frame is used if neither slot is
valid.

To regenerate the pack from the original downloaded atlas:

```powershell
& "<bundled-python>" .\tools\make_merry_petpack.py <spritesheet.webp> .\assets\merry
```
