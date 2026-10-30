# SniPin

A small Windows snipping tool that puts **WebP on the clipboard by default** — lossless, small, and
pasteable straight into anything that accepts a dropped file (Chromium, Slack, Discord, Explorer…),
while apps that only understand bitmaps still get a normal image.

One executable, no installer, no .NET, no DLLs, no runtime to install. The same exe is both the
tray/hotkey resident and the short-lived capture worker.

## Why

Windows' built-in snipping tool copies a bitmap; a 1080p region becomes ~8 MB on the clipboard and
turns into PNG or JPEG wherever it is pasted. SniPin encodes the snip as WebP (quality 100 is
lossless and still several times smaller), publishes it as a real file drop plus PNG, `CF_DIB` and a
`WebP` / `image/webp` format, and keeps the image on screen as a pin you can annotate before copying.

## What it does

- **PrintScreen** (or your own shortcut) freezes the screen, dims it and lets you drag a region.
  `Esc` cancels, `Enter` takes the monitor under the cursor, `Shift+Enter` takes every monitor.
- The snip appears as a **pin**: borderless, 3 px frame, always on top, offset from the capture.
  Several pins can be open at once; each is its own worker.
- **Editing**: pointer (move), box, ellipse, arrow, highlighter, freehand pen, freehand marker,
  redact, text and crop, with a hex colour box + six basic swatches, a size stepper, undo, save and
  copy. The toolbar is a fixed strip under the image with a second row for colour and size; the
  image has three handles (bottom = height, corner = keep ratio, right = width).
- **Copy** happens in the background after every stroke (and on `Ctrl+C`): WebP file drop
  (`CF_HDROP`), PNG, `CF_DIB`, `WebP` and `image/webp`, plus `Preferred DropEffect`.
- **Save As** to `webp`, `png`, `jpg` or `bmp` through the standard dialog.
- **Crop** creates a new pin of the selected area instead of replacing the current one.
- **Tray**: snip region, settings, exit. The settings dialog covers the hotkey recorder, WebP
  quality, clipboard mode, save format/folder, pin offset, pen/marker/text sizes, autostart and
  always-on-top; invalid values are repaired rather than saved.
- Nothing is written outside one folder next to the exe.

## How it works

```
SniPin.exe            resident: tray icon, global hotkey, settings, janitor, spawns workers
SniPin.exe --capture  worker:   overlay capture, then the pin window and its editor
```

The resident owns the mutex and the hotkey. A snip asks it to spawn a worker (fresh process, job
object with `KILL_ON_JOB_CLOSE`, foreground hand-off so the overlay gets the keyboard). The worker
freezes the desktop into a bitmap, dims it once, paints the rubber band from an off-screen back
buffer, and on release decodes the region into a `Document`. The pin window draws the image plus the
annotation shapes, and a background thread flattens, encodes WebP with libwebp and publishes the
clipboard formats.

Everything lives in `SniPin-data\` next to the exe (settings, log, clipboard temp files); if that
folder is read-only it uses `%LOCALAPPDATA%\SniPin`.

## Download

Grab the latest **`SniPin-<version>-win-x64.zip`** from the *Releases* page and extract
`SniPin.exe` anywhere you like — it is a single self-contained file.

The build is **not code-signed**, so the first run of a downloaded copy shows *"Windows protected
your PC"*: choose **More info → Run anyway**. Verify the download instead of trusting it:

```powershell
Get-FileHash .\SniPin-<version>-win-x64.zip -Algorithm SHA256
Get-Content .\SniPin-<version>-win-x64.zip.sha256
```

Windows 10/11 x64.

## Build

Requirements: Visual Studio 2022 with the C++ desktop workload, CMake 3.20+.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The exe lands in `build\Release\SniPin.exe`. libwebp and its sharp-YUV helper are vendored as
static libraries under `native\win-x64\`, so there is nothing to download and no DLL to ship.
The build uses the static CRT (`/MT`), link-time code generation, and no exceptions or RTTI.

## Command line

```
SniPin.exe                 tray + hotkey (this is what the autostart entry runs)
SniPin.exe --snip          ask the running resident to snip
SniPin.exe --capture       the capture worker (spawned by the resident)
SniPin.exe --settings      open the settings dialog
SniPin.exe --quit          stop the resident
SniPin.exe --selftest DIR  write a JSON self-test report to DIR
```

Pin keys: `Esc` back to pointer mode, `Ctrl+C` copy, `Ctrl+S` save, `Ctrl+Z` undo, arrow keys nudge
(Shift = 10 px), `Alt+F4` or the X island closes.

## Limits

- Lossless WebP of a full 1080p screen takes about a second on a modest machine (libwebp at quality
  100); lower the quality in settings for speed.
- The clipboard has no native WebP format on Windows; consumers that read `CF_HDROP`/`PNG` work,
  consumers that only ask for `CF_BITMAP` get the DIB.
- Pins are not persisted across restarts, and there is no image history.

## License

MIT — see [LICENSE](LICENSE). Third-party components: [THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt).
