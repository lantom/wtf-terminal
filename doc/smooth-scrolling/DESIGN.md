# GPU-accelerated smooth, pixel-perfect scrolling

## Goal
"Paper-like" continuous vertical scrolling in Windows Terminal, driven on the GPU,
with a user-adjustable speed exposed in the Settings UI. Everything else about the
terminal is unchanged; when the feature is off the code paths behave exactly as before.

## Core idea
The terminal buffer is row-addressed, so the viewport top has always been an integer
row. We keep that, and add a **sub-row pixel shift**:

    visible viewport top (in pixels) = topRow * cellHeight + scrollPixelShift
    topRow            : integer buffer row (unchanged semantics)
    scrollPixelShift  : integer, 0 <= shift < cellHeight

The shift is an **integer number of device pixels**, never a fraction. That keeps
glyph rasterization identical to today (crisp, no sub-pixel blur) - "pixel perfect".

To fill the strip exposed at the bottom by the shift, the renderer paints one extra
row: `GetRenderViewport()` is `GetViewport()` grown by one row for as long as smooth
scrolling is on (and the buffer has a row to spare). It stays grown even on frames where
the shift happens to be zero, because a changing row count makes AtlasEngine reallocate
its row cache and re-shape every row - not something to do at 120 frames a second. `GetViewport()` itself is untouched, so
conpty size, scrollbar range, page-up size and hit testing keep their old meaning.

## Where the animation lives
`Microsoft::Terminal::Core::Terminal` owns the animation state:

    _smoothScrollCurrent  double, viewport offset now, in fractional rows
    _smoothScrollTarget   double, where we are heading
    _smoothScrollElapsed  double, seconds since the animation started
    _smoothScrollCurve    ScrollAnimationCurve, the browser curve
    _smoothScrollSpeed    double, user setting

Advancing happens on the **render thread**, inside `Renderer::_PaintFrame()` while the
terminal lock is held (`IRenderData::AdvanceScrollAnimation()`). If the animation is
still running, the renderer schedules another frame with `NotifyPaintFrame()`. AtlasEngine
presents with `Present1(1, ...)`, i.e. v-sync, so the animation is paced by the display
refresh rate for free - the same mechanism the custom-shader `RequiresContinuousRedraw`
path already uses. No UI-thread timer is involved.

## The curve: what a browser does

`src/cascadia/TerminalCore/ScrollAnimation.h` is a port of the two pieces of Chromium
that decide how a scroll moves, kept close enough to the originals to be diffed
against them:

    ui/gfx/geometry/cubic_bezier.cc                -> CubicBezier
    cc/animation/scroll_offset_animation_curve.cc  -> ScrollAnimationCurve

A wheel notch in Chromium becomes `ScrollType::kMouseWheel`, which is an ease-in-out
cubic bezier - control points `(0.42, 0)` and `(0.58, 1)` - run with
`DurationBehavior::kInverseDelta`. Three things follow from that, and all three are
things an exponential decay does not give you:

**The motion eases in and out.** Position is `initial + (target - initial) * bezier(t)`.
It starts slowly, is fastest in the middle, and slows to a stop. An exponential decay
is at its *fastest* on the very first frame and then has an infinite tail it never
actually reaches; that reads as a lurch followed by a drift.

**A longer scroll runs for less time.** kInverseDelta:

    frames = clamp(14 - distanceInPx / 60, 6, 12)     // at 60Hz
    seconds = frames / 60 / speed

200 ms for anything up to 120 px, ramping down to 100 ms at 480 px and beyond. So a
short hop is deliberate and a long flick is quick, instead of everything taking the
same time.

**A notch that arrives mid-animation preserves velocity.** This is the important one,
and it is `ScrollOffsetAnimationCurve::UpdateTarget()`. Rather than moving the target
and letting the position chase it, the curve is *rebuilt* from the position the view is
at, with its first control point scaled by the speed the view is already travelling at
(`EaseInOutWithInitialSlope`). The new segment therefore leaves the old one with no
discontinuity in either position or velocity. That is why a browser scrolled with five
quick notches looks like one continuous movement rather than five restarts.

`EaseInOutBoundedSegmentDuration()` also caps the new segment at the time it would take
to coast into the new target at the current speed (times 2.5, "to account for the ease
out"). Without that, a fast flick that ends on a short hop overshoots and rubber-bands.

`speed` is our own knob, and it simply divides the duration: 2.0 is twice as quick.

### Absolute time, not per-frame integration
`ValueAt()` takes the total time elapsed since the animation started, so a frame the
render thread missed lands *further along the same path* instead of bending it. The
shape of the motion is identical at 60, 120 and 165 Hz, and a stall does not change
where the scroll ends up. `Terminal::_StepScrollAnimation(dt)` only advances the clock;
it does not accumulate position.

### The clock is the display's refresh grid
A browser advances its scroll animations by the compositor's frame time, and that frame
time *is* a vsync timestamp - every sample of the curve lands on an exact
refresh-interval grid.

`AdvanceScrollAnimation()` is called at the top of `Renderer::_PaintFrame()` instead, and
that moment drifts: by how long the previous frame took to build, by when the frame
latency waitable object fired, by how long the console lock was held. Presenting is
perfectly paced either way - `Present1(1, ...)` on a waitable swap chain with
`SetMaximumFrameLatency(1)` sees to that - but the *positions* handed to it were sampled
off a jittery clock. After the shift is rounded to whole device pixels that turns a clean
2,3,2,3 px cadence into a random 1,4,2,3, and the motion shimmers.

So `Terminal::_SnapToRefreshGrid()` asks DWM where the grid is
(`DwmGetCompositionTimingInfo`: `qpcVBlank`, `qpcRefreshPeriod`, re-read a few times a
second) and rounds the timestamp back onto it, and the step is then taken in whole
refresh intervals - one normally, more when frames were missed, capped at eight so a
stall cannot teleport the animation.

### How far one notch goes
`ControlInteractivity::_browserRowsPerNotch()`. `SPI_GETWHEELSCROLLLINES` is a number of
*lines*, three by default. A terminal has always read that as three buffer rows; a
browser reads it as three of its own lines, and a browser line is a fixed 100/3 DIPs
(Chromium's `kScrollbarPixelsPerLine`), so the same setting moves about twice as far.

That difference matters for more than reach. Covering half the distance in the same
100-200 ms halves the per-frame step, and since the shift is rounded to whole device
pixels, a smaller step makes that rounding a larger fraction of it - at ~2 px a frame a
half-pixel is a visible wobble, at ~4 px it is not. With smooth scrolling on we therefore
use the browser's definition of a line; with it off we count buffer rows, as before.

### Distances are in device pixels
Chromium's ramp constants are CSS pixels; we feed the curve device pixels
(`rows * cellHeight`). On a 100% display those are the same. At 150% a given scroll is
nominally 1.5x further along the ramp than it would be in a browser, which moves the
duration by tens of milliseconds at most - not worth plumbing the scale factor down
into the core for.

## Input that is already continuous is not animated
A browser animates a wheel *notch* because a notch is a discrete step - "go three
lines" - with no information about how to get there. It does **not** animate a
scrollbar drag, a touch pan, or precision-touchpad deltas (`kScrollByPrecisePixel`):
those are continuous streams that already describe the motion, and animating after them
only puts the view behind the user's hand.

`Terminal::SmoothScrollToRow(viewTop, animate)` and
`ControlInteractivity::UpdateScrollbarImmediate()` carry that distinction:

| Input | Animated |
|---|---|
| Wheel notch (\|delta\| >= WHEEL_DELTA) | yes |
| Sub-notch wheel delta (hi-res wheel, precision touchpad) | no |
| Scrollbar drag | no |
| Touch pan | no |

The un-animated paths still land on a fractional row, so they get the pixel shift and
move by less than a row at a time - they are smooth because the *input* is smooth.

## The wheel accumulator

`ControlInteractivity` accumulates fractional rows across wheel events so that precision
trackpads, whose deltas are smaller than a row, still add up (GH#9955.b). It resynchronises
that accumulator whenever it disagrees with the core, in case something else moved the
viewport underneath it.

That comparison has to be against where the core is *heading*, not against what is on
screen. Those are the same thing without smooth scrolling, but with it they differ for
the whole length of the animation - so comparing against the rendered row makes every
notch after the first throw away what the previous ones accumulated. Five notches then
move one notch worth. `ControlCore::ScrollTargetRow()` exists for this.

## GPU side
`AtlasEngine::UpdateScrollPixelShift()` carries the shift into the rendering payload.
* `BackendD3D` - the vertex shader translates every quad except the full-screen
  background quad by `positionOffset`; the pixel shader offsets the background cell
  lookup by the same amount. One extra float2 per constant buffer, zero extra draw calls.
* `BackendD2D` - equivalent `SetTransform()` translation.
When the shift changes, the frame is presented with a full dirty rect and the
swap-chain scroll optimisation (`scrollDeltaY`) is disabled for that frame, because
the previous frame's pixels are no longer aligned to the new ones. Row shaping caches
are *not* invalidated - only rows scrolled in are re-shaped, exactly as before.

## Settings
Global (window) settings, shown on the Settings > Interaction page:

    "smoothScrolling":      bool,   default true
    "smoothScrollingSpeed": double, default 1.0, clamped to [0.1, 5.0]

`smoothScrolling: false` restores the original integer-row behaviour bit for bit:
the shift stays 0, the render viewport is not grown, and every scroll lands on a whole
row the moment it arrives.
