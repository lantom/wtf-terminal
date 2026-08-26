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

The motion is the one a web browser uses - the curve itself is ported from Chromium
(`cc/animation/scroll_offset_animation_curve.cc`). A wheel notch eases in and out over
100-200 ms, longer scrolls take *less* time than short ones, and a notch that arrives
while the view is still moving picks up the current speed instead of restarting, so a
run of quick notches reads as one continuous movement.

**Speed** scales that duration: `1.0` is what a browser does, `2.0` is twice as quick,
`0.5` half.

One notch also travels as far as it would in a browser. Windows tells us the preference
in *lines* (three by default); a terminal has always read that as three buffer rows,
while a browser reads it as three browser lines of 100/3 DIPs each - about 100 DIPs
either way it is set, roughly twice a terminal's. Your Windows "lines to scroll" setting
is still what decides; only the length of a line changed. Turning smooth scrolling off
goes back to counting buffer rows.

Input that is already continuous is *not* animated, which is also what browsers do:
dragging the scrollbar, panning with touch, and the sub-notch deltas a precision
trackpad or a high-resolution wheel sends are applied the moment they arrive, so the
view stays under your hand. They still move by fractions of a row - the pixel shift
applies either way, which is what precision trackpads gain here.

## How it works

See [DESIGN.md](DESIGN.md) for the full picture. The short version:

* The viewport top is still an integer buffer row, plus a **whole number of device
  pixels** of leftover (`0 <= shift < cellHeight`). Because the shift is a whole pixel,
  glyphs are rasterized exactly as before - no sub-pixel blur. That is the "pixel
  perfect" part.
* The renderer paints one extra row, to fill the strip the shift exposes at the bottom.
  It does so for as long as smooth scrolling is on rather than only while the shift is
  non-zero, because a changing row count would make AtlasEngine re-shape every row.
* The shift is applied **on the GPU**: `AtlasEngine`'s vertex shader translates every
  quad, and its pixel shader offsets the background cell lookup by the same amount. No
  extra draw calls, no extra passes, no re-shaping of cached rows.
* The animation is advanced on the render thread, once per presented frame. AtlasEngine
  presents with a sync interval of 1, so the motion is paced by the display refresh rate
  without any timer of its own.

## Building

**Visual Studio 2026 (18.x) is required**, as it is upstream: `build/pipelines/ci.yml`
builds on `SHINE-VS18-Latest` and `src/common.build.pre.props` selects the **v145**
toolset whenever `VisualStudioVersion >= 18.0`. Visual Studio 2022 does not work - its
XAML targets combined with the Windows SDK 10.0.26100 XAML compiler fail on
`Microsoft.Terminal.Settings.Editor` with an internal `WMC9999` error, on unmodified
upstream sources as much as on this fork.

The Build Tools are enough; get them from
<https://aka.ms/vs/18/stable/vs_BuildTools.exe> and install the C++, .NET desktop and
UWP build-tools workloads plus the Windows 11 SDK 10.0.26100. See
[.vsconfig](../../.vsconfig) for the full component list.

```powershell
# vcpkg - a full clone, its versioning needs the history
git clone https://github.com/microsoft/vcpkg.git dep\vcpkg
dep\vcpkg\bootstrap-vcpkg.bat

# packages.config dependencies (PackageReference restore does not cover these)
nuget restore .nuget\packages.config    -PackagesDirectory packages
nuget restore build\packages.config     -PackagesDirectory packages
nuget restore dep\nuget\packages.config -PackagesDirectory packages

$env:MSBUILDENABLESLNXSUPPORT = '1'
msbuild terminal.slnf -p:Configuration=Release -p:Platform=x64 -m
```

`terminal.slnf` is `OpenConsole.slnx` minus three `net8.0-windows` projects (the
embeddable WPF control, its test app and a stress tool) whose targeting packs are not on
the repository's NuGet feed. Nothing in Windows Terminal itself depends on them.

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
