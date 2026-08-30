# Merry ST7789 dongle screen

The `xiao_st7789_merry` dongle firmware drives the 240x240
ZJY-IPS130-V2.0/ST7789 panel in SPI mode 3. It shows the L/D/R peripheral
batteries, the current layer, and active Ctrl/Alt/Shift modifiers while the
keyboard is active. In automatic mode it switches to Merry after 20 seconds
and blanks the panel and backlight five minutes later.

## Runtime settings

The display settings are stored persistently on the XIAO and can be changed
through its Windows COM port without rebuilding or reflashing ZMK. The COM
number is assigned by Windows; find it by unplugging and reconnecting the
dongle in Device Manager.

Read the current settings:

```powershell
.\tools\configure_merry.ps1 -Port COM24
```

Set a 30-second pet timeout, 80% backlight, and a small position adjustment:

```powershell
.\tools\configure_merry.ps1 -Port COM24 -TimeoutSeconds 30 -Brightness 80 -PetX 3 -PetY -2
```

Keep a particular screen or animation selected:

```powershell
.\tools\configure_merry.ps1 -Port COM24 -Mode Dashboard
.\tools\configure_merry.ps1 -Port COM24 -Mode Pet -Animation Waving
.\tools\configure_merry.ps1 -Port COM24 -Mode Off
```

Valid modes are `Auto`, `Dashboard`, `Pet`, and `Off`. Valid animations are
`Idle`, `RunningRight`, `RunningLeft`, `Waving`, `Jumping`, `Failed`, `Waiting`,
`Running`, and `Review`. Brightness accepts 0-100, the timeout accepts 1-3600
seconds, X accepts -40 to 40, and Y accepts -33 to 33.

Restore the defaults:

```powershell
.\tools\configure_merry.ps1 -Port COM24 -Reset
```

Each command reads and verifies the dongle response with CRC32. A setting is
written only after the complete structure passes firmware-side validation.

## Pet packs

Upload a regenerated animation pack without reflashing ZMK:

```powershell
.\tools\upload_merry_pet.ps1 -Port COM24
```

The v2 pack contains 57 frames in nine animations. Each stored 160x174 frame
uses 5-bit pixel indices. Firmware decodes it directly into one 184x200 RGB565
render surface, with no second framebuffer and no larger QSPI pack.

The current generator uses one stable 31-color palette per animation to avoid
frame-to-frame color shimmer. Reserved cyan, blue, emerald, deep-green,
off-white, and plum anchors keep important colors from being displaced by the
ship's many browns. A sharpened box downsample and perceptually weighted color
matching preserve fine details before the on-device nearest-neighbour upscale.

The upload uses the inactive QSPI slot and commits it only after all chunks and
the full-pack CRC pass. An interrupted upload leaves the previous slot usable.

Backlight brightness is a persistent but completely reversible runtime setting:

```powershell
.\tools\configure_merry.ps1 -Port COM24 -Brightness 85
.\tools\configure_merry.ps1 -Port COM24 -Brightness 100
```
