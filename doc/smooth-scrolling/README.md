# Smooth, pixel-perfect scrolling

This fork of [microsoft/terminal](https://github.com/microsoft/terminal) adds one thing:
the terminal scrolls **continuously**, a device pixel at a time, instead of jumping a
whole text row at a time. Everything else about Windows Terminal is untouched, and with
the feature turned off the code takes exactly the paths it always did.

## Using it

Settings → Interaction:

| Setting | JSON key | Default | Range |
| --- | --- | --- | --- |
| Smooth scrolling | `smoothScrolling` | `true` | `true` / `false` |
| Smooth scrolling speed | `smoothScrollingSpeed` | `1.0` | `0.1` – `5.0` |

Both are global (window) settings, so they can also be set by hand in `settings.json`:

```json
{
    "smoothScrolling": true,
    "smoothScrollingSpeed": 1.0
}
```

**Speed** is how quickly the view catches up with where you scrolled to. At `1.0` the
view covers ~63% of the remaining distance every 90 ms, which reads as a gentle glide.
Turn it up for something snappier, down for something more languid. It does not change
how far one wheel notch scrolls - that stays under the control of the Windows
"lines to scroll" mouse setting, exactly as before.

It applies to every way of scrolling: the wheel, a precision trackpad, dragging the
scrollbar, and keyboard scrolling. Precision trackpads benefit the most, because their
sub-row deltas used to be rounded away.

## How it works

See [DESIGN.md](DESIGN.md) for the full picture. The short version:

* The viewport top is still an integer buffer row, plus a **whole number of device
  pixels** of leftover (`0 <= shift < cellHeight`). Because the shift is a whole pixel,
  glyphs are rasterized exactly as before - no sub-pixel blur. That is the "pixel
  perfect" part.
* The renderer paints one extra row while the shift is non-zero, to fill the strip the
  shift exposes at the bottom.
* The shift is applied **on the GPU**: `AtlasEngine`'s vertex shader translates every
  quad, and its pixel shader offsets the background cell lookup by the same amount. No
  extra draw calls, no extra passes, no re-shaping of cached rows.
* The animation is advanced on the render thread, once per presented frame. AtlasEngine
  presents with a sync interval of 1, so the motion is paced by the display refresh rate
  without any timer of its own.

## Building

Prerequisites: Visual Studio 2022 (or the 2022 Build Tools) with the C++ workload,
the Windows 11 SDK 10.0.26100, and the .NET desktop / UWP build tools. See
[.vsconfig](../../.vsconfig).

```powershell
# vcpkg (a full clone - its versioning needs the history)
git clone https://github.com/microsoft/vcpkg.git dep\vcpkg
dep\vcpkg\bootstrap-vcpkg.bat

# packages.config dependencies
nuget restore .nuget\packages.config   -PackagesDirectory packages
nuget restore build\packages.config    -PackagesDirectory packages
nuget restore dep\nuget\packages.config -PackagesDirectory packages

# Visual Studio 2022 ships the v143 toolset; the overlay triplets default to v145.
$env:VCPKG_PLATFORM_TOOLSET = 'v143'
$env:MSBUILDENABLESLNXSUPPORT = '1'

msbuild terminal.slnf -p:Configuration=Release -p:Platform=x64 -m
```

`terminal.slnf` is `OpenConsole.slnx` minus three `net8.0-windows` projects (the
embeddable WPF control and a stress tool) whose targeting packs are not on the
repository's NuGet feed. Nothing in Windows Terminal itself depends on them.

## Packaging

```powershell
.\build\scripts\New-SmoothScrollingRelease.ps1
```

writes to `dist\`:

* a **signed MSIX** plus the `.cer` it was signed with, and `Install-WtfTerminal.ps1`
  which trusts the certificate and installs the package (run it elevated),
* a **portable** folder and zip that needs no install and, thanks to the `.portable`
  marker, keeps its settings next to the executable.

## Tests

`src\cascadia\UnitTests_TerminalCore\SmoothScrollTest.cpp` covers the animation and the
row/pixel split: convergence, the shift always staying inside one cell, the render
viewport growing by exactly one row and never past the end of the buffer, fractional
targets surviving, clamping at both ends of the scrollback, the speed setting actually
changing the rate, and the feature being inert when disabled or in the alternate buffer.
`DeserializationTests::SmoothScrolling*` covers the two settings.
