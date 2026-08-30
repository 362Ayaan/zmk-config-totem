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
.\tools\configure_merry.ps1 -Port COM24 -Mode Pet -Animation Completed
.\tools\configure_merry.ps1 -Port COM24 -Mode Off
```

Valid modes are `Auto`, `Dashboard`, `Pet`, and `Off`. Valid animations are
`Idle`, `Running`, `NeedsInput`, `Completed`, and `Blocked`. These are the five
states intended for a future PC-side Codex bridge. Brightness accepts 0-100,
the timeout accepts 1-3600 seconds, X accepts -40 to 40, and Y accepts -33 to
33.

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

The v3 pack contains 30 frames in five animations. Each frame retains Merry's
native 192x208 pixels and uses 6-bit indices into a private 63-colour RGB565
palette. Firmware decodes it directly into one 202x220 RGB565 render surface,
with no second framebuffer and no larger QSPI pack. The complete pack is
902,720 bytes, leaving 141,760 bytes free in its 1 MiB QSPI slot.

The current generator uses one stable 63-colour palette per animation to avoid
frame-to-frame color shimmer. Reserved cyan, blue, emerald, deep-green,
off-white, and plum anchors keep important colors from being displaced by the
ship's many browns. Offline OKLab palette matching improves perceptual colour
selection. Keeping the source at its native size avoids the earlier non-integer
downsample; the device performs only the final nearest-neighbour enlargement to
202x220, which preserves hard pixel-art edges.

The display bus runs at 8 MHz. A full 202x220 RGB565 transfer takes about 89 ms
on the wire before command and software overhead, versus about 178 ms at 4 MHz.
Reducing the pack to 30 frames saves QSPI space and conversion work, but does
not by itself raise the playback frame rate; bus transfer and the configured
animation delays determine that.

Pack v3 and settings v2 are deliberate compatibility breaks. Flash this
firmware before uploading the v3 pack: older firmware rejects it, and the new
firmware rejects an old v2 pack and safely uses its embedded fallback. Existing
v1 runtime settings are ignored after the upgrade so obsolete animation IDs
cannot select the wrong state; the documented defaults are restored.

The upload uses the inactive QSPI slot and commits it only after all chunks and
the full-pack CRC pass. An interrupted upload leaves the previous slot usable.

Backlight brightness is a persistent but completely reversible runtime setting:

```powershell
.\tools\configure_merry.ps1 -Port COM24 -Brightness 85
.\tools\configure_merry.ps1 -Port COM24 -Brightness 100
```
