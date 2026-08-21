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

    _scrollPositionPx   double, current viewport top in pixels
    _scrollTargetPx     double, where we are heading
    _smoothScrollSpeed  double, user setting

Advancing happens on the **render thread**, inside `Renderer::_PaintFrame()` while the
terminal lock is held (`IRenderData::AdvanceScrollAnimation()`). If the animation is
still running, the renderer schedules another frame with `NotifyPaintFrame()`. AtlasEngine
presents with `Present1(1, ...)`, i.e. v-sync, so the animation is paced by the display
refresh rate for free - the same mechanism the custom-shader `RequiresContinuousRedraw`
path already uses. No UI-thread timer is involved.

Easing is exponential smoothing towards the target:

    alpha = 1 - exp(-dt / tau),  tau = kBaseTau / speed
    pos  += (target - pos) * alpha

Exponential smoothing is used because repeated wheel events simply move the target;
there is no queue of animations to reconcile, and the result is a critically damped,
paper-like glide. `kBaseTau = 90 ms` at speed 1.0.

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
the shift stays 0, the render viewport is not grown, and the target is applied
immediately.
