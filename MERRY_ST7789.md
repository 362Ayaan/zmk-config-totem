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
`Idle`, `Running`, `NeedsInput`, `Completed`, and `Blocked`. In automatic mode
the live Codex bridge chooses the animation; the configured animation remains
the manual selection used by `-Mode Pet`. Brightness accepts 0-100, the timeout
accepts 1-3600 seconds, X accepts -40 to 40, and Y accepts -33 to 33.

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

The v4 pack contains 30 frames in five animations. Each frame retains Merry's
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

Pack v4 and settings v2 are deliberate compatibility breaks. Flash this
firmware before uploading the v4 pack: older firmware rejects it, and the new
firmware rejects v2/v3 packs and safely uses its embedded fallback. Existing v1
runtime settings are ignored after the upgrade so obsolete animation IDs cannot
select the wrong state; the documented defaults are restored.

Runtime QSPI frame reads are split into aligned 512-byte transfers. A failed
animation descriptor or frame transfer quarantines the active pack in RAM and
immediately renders the embedded idle frame instead of leaving a blank image.
Boot checks only committed-slot structure; the expensive complete CRC pass is
performed before the uploader commits a slot, keeping USB and display startup
out of the multi-megabyte validation path.

Inspect pack health after an upload or clear both external slots explicitly:

```powershell
.\tools\inspect_merry_store.ps1 -Port COM24
.\tools\inspect_merry_store.ps1 -Port COM24 -Clear
```

Clearing affects only the two QSPI pet slots. It does not erase ZMK, pairings,
or display settings, and the embedded pet remains available.

The upload uses the inactive QSPI slot and commits it only after all chunks and
the full-pack CRC pass. An interrupted upload leaves the previous slot usable.

Backlight brightness is a persistent but completely reversible runtime setting:

```powershell
.\tools\configure_merry.ps1 -Port COM24 -Brightness 85
.\tools\configure_merry.ps1 -Port COM24 -Brightness 100
```

## Codex Desktop bridge

The Windows bridge reads task-status metadata from the running Codex Desktop
app and sends only a five-state value to the dongle. It does not add another
USB interface and does not write the state to flash. Start it with automatic
dongle discovery:

```powershell
.\tools\merry_codex_bridge.ps1
```

Specify the port only if automatic discovery is ambiguous:

```powershell
.\tools\merry_codex_bridge.ps1 -Port COM24
```

The mapping is `active` to Running, approval or user-input attention to
NeedsInput, a successful finish to an eight-second Completed pulse, and a
failed/system-error finish to a twenty-second Blocked pulse. Multiple tasks are
combined by priority: Blocked, NeedsInput, Running, Completed, then Idle.

The bridge refreshes a 12-second watchdog. If Codex Desktop, the bridge, the
USB cable, or Windows serial connection disappears, firmware falls back to
Idle automatically. Codex activity changes only the animation; keyboard
activity still controls dashboard-to-pet timing and the five-minute AFK screen
blanking, and a background task never wakes an already blanked display.

Test the PC side without touching the dongle, or test its internal protocol
logic without connecting to Codex:

```powershell
.\tools\merry_codex_bridge.ps1 -DryRun -Once
.\tools\merry_codex_bridge.ps1 -SelfTest
```

The bridge keeps the COM port open for stable heartbeats. Stop it with Ctrl+C
before running the pack uploader, configuration tool, or store inspector, then
start it again afterward. It automatically rediscovers the Codex Desktop pipe
after an app restart. Because that local app-tools pipe is versioned with the
desktop app rather than a public cross-version API, rerun `-DryRun -Once` after
a major Codex Desktop update before relying on unattended startup.
