# Loupe

A fast, modern replacement for the Windows 11 Photos app.

Double-click a photo and it's on screen almost instantly. Flip through the folder with the arrow keys or the mouse wheel, and the next and previous images are already decoded, so you never see a blank frame. Zoom right to the pixel, compare two shots side by side, check the camera settings, make a quick crop, and get out of the way. Part of the larosa023 suite of lightweight Windows apps.

- **Opens everything:** JPEG, PNG, GIF, WebP, AVIF, HEIC, BMP, TIFF, ICO and SVG, plus RAW previews from Canon (CR2, CR3), Nikon (NEF), Sony (ARW) and DNG files
- **Fast:** one small native exe, no runtime. JPEGs are decoded straight to screen size, then sharpened to full resolution in the background when you zoom in
- **Quick edits** that never overwrite anything without asking, and **Ctrl+Z** undoes a save, a delete or a rename

## Features

**Viewing**
- Arrow keys, Page Up/Down or the mouse wheel flip through every image in the folder; the neighbors are preloaded
- Sort by name, date taken, date modified or size (ascending or descending)
- Zoom to fit, 100% (pixel-exact), scroll-to-zoom at the cursor, click-drag to pan, double-click toggles fit and zoom. Smooth and GPU-accelerated (Direct2D)
- Animated GIF, WebP and APNG playback, with pause (**P**) and frame stepping (**,** and **.**)
- Filmstrip of thumbnails along the bottom (**F**) and a grid of the whole folder (**G**)
- Info panel (**I**): dimensions, file size, date taken, camera, lens, ISO, shutter, aperture, focal length, GPS with **Open in maps**
- Compare (**C**): two images side by side with synced zoom and pan
- Full-screen slideshow (**Space**, **F11**) with adjustable timing and shuffle
- Embedded color profiles (Adobe RGB, Display P3) are converted for your screen; EXIF orientation is applied

**Editing**
- Crop (free or 1:1, 4:3, 3:2, 16:9, 5:4, 7:5, original), rotate, flip, straighten
- Exposure, contrast, saturation and warmth, previewed live on the GPU
- **Save** asks before replacing the original (and keeps a backup so Ctrl+Z can put it back), **Save a copy**, or **Revert**
- Camera metadata is kept when saving

**Files**
- Copy the image (**Ctrl+C**) or its path (**Ctrl+Shift+C**), show it in File Explorer (**Ctrl+E**) or open the folder in Strata (**Ctrl+Shift+E**)
- Set as desktop background (**Ctrl+B**), Open with…, Print
- Delete to the Recycle Bin (**Del**) with undo, rename (**F2**)
- Drag images (or a folder) into the window

**The rest**
- The same 14 themes as Hangar and the rest of the suite (Dark with Mica is the default), **Match Hangar**, the suite's accent colors or any color, and custom theme files in Hangar's format. Themes switch live
- Image background follows the theme, or black, gray or a checkerboard
- The toolbar hides itself so the photo fills the window; everything works from the keyboard (**F1** lists the shortcuts, **Ctrl+,** opens Settings)
- Built-in updater with Stable and Beta channels, SHA-256 verification and automatic rollback
- No accounts, no telemetry, no background services, no admin rights. Portable mode with an empty `portable` file next to `Loupe.exe`

## Download

Grab `Loupe.exe` from the [latest release](https://github.com/larosa023/Loupe/releases/latest), or install it from Hangar. It runs from anywhere.

To make it your photo viewer, open **Settings → Files → Default apps…** (or the welcome screen's button). Loupe registers itself for every format it opens, per user and without admin rights; Windows then lets you pick it in Default apps. HEIC and AVIF need Windows' free HEIF / AV1 extensions from the Microsoft Store (Loupe links you to them if they're missing).

Windows may show a SmartScreen warning the first time because the exe isn't code-signed yet: **More info → Run anyway**.

Command line: `Loupe.exe "photo.jpg"` · `--quit` · `--uninstall` · `--register`.

## Keyboard

| | |
|---|---|
| **Browse** | ← → / PgUp PgDn / wheel · Home End · **G** grid · **F** filmstrip · **S** sort |
| **Zoom** | + − ↑ ↓ / Ctrl+wheel (at the cursor) · **0** fit · **1** 100% · double-click · drag to pan |
| **Look** | **I** info · **C** compare (Tab switches sides) · **Space** slideshow · **F11** full screen · **P** , . animation |
| **Edit** | **E** edit · **R** / Shift+R rotate · **H** / **V** flip · Ctrl+S save (asks) · Ctrl+Shift+S save a copy · Esc cancel |
| **Files** | Del delete · F2 rename · Ctrl+Z undo · Ctrl+C copy · Ctrl+Shift+C copy path · Ctrl+E Explorer · Ctrl+Shift+E Strata · Ctrl+B wallpaper · Ctrl+O open |
| **App** | F1 shortcuts · Ctrl+, settings · Ctrl+W close |

The mouse wheel flips images by default; **Settings → Viewing → Mouse wheel → Zooms** swaps it with Ctrl+wheel.

## How it's built

Native C++17 with Win32, Direct2D, DirectWrite and the Windows Imaging Component, cross-compiled with Zig: the same stack as Hangar, Strata and Foyer. WebP comes from a bundled libwebp (decoder only), SVG from Direct2D's own renderer, and RAW previews from Loupe's own TIFF/ISO-BMFF reader, which also reads EXIF for every format.

- **Startup:** the first image starts decoding on a worker thread before the window exists. JPEGs decode at 1/2, 1/4 or 1/8 scale inside the decoder when that still covers the screen. `loupe.log` records the time from process start to first pixels.
- **Flipping:** a small cache keeps the current image and two on each side as GPU bitmaps; CPU copies are freed once uploaded. Full resolution is decoded only for the image on screen, when you zoom in (or after it's been shown a moment).
- **Opening from Explorer:** with **Open images in the same window** on (the default), a second double-click hands the path to the running window and exits without starting anything.

## Building

```bash
pip install ziglang
./build.sh            # dist/Loupe.exe
```

Or on Windows with Visual Studio 2022: `cmake -S . -B build -A x64 && cmake --build build --config Release`.

## Releasing

Bump `LOUPE_VERSION` in `src/common.h`, update `CHANGELOG-latest.md` and push to `main`. GitHub Actions builds `Loupe.exe` with Zig, then on Windows runs a cold-start test, the end-to-end smoke test (every format and feature, with screenshots on the `ci-screenshots` branch), the single-window hand-over and `--quit`, and file-type registration. If that version has no release yet it publishes `v<version>` with `Loupe.exe`, a zip and `SHA256SUMS.txt`, then checks that an old build updates itself to it. Versions with a `-` (like `0.2.0-beta.1`) are published as pre-releases for the Beta channel.

## How the updater works

On launch (at most once an hour) and every three hours, Loupe looks for a newer release of `larosa023/Loupe` on your channel, reading Hangar's release index first and the GitHub API second. It downloads `Loupe.exe` in the background and checks its SHA-256 against GitHub's asset digest and `SHA256SUMS.txt`. A small **Update ready · Restart** prompt appears; restarting renames the running exe to `Loupe.exe.old`, puts the new one in place and starts it on the same image. If the new version doesn't report that it started within 20 seconds, Loupe puts the old exe back, carries on and skips that version. With automatic updates on, a ready update also installs this way when you close Loupe. The two previous versions are kept for **Settings → Updates → Roll back**.

Settings live in `%LOCALAPPDATA%\Loupe\settings.json` (or `.\data\` in portable mode). Custom themes go in `%LOCALAPPDATA%\Loupe\themes` (Hangar's themes folder is read too); see Hangar's `docs/THEMES.md` for the format.

## License

MIT — see [LICENSE](LICENSE). libwebp is BSD-licensed (see `third_party/libwebp/COPYING`).
