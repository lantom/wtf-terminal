// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// - ScrollAnimation.h
//
// Abstract:
// - The scroll animation a web browser runs, ported so that the terminal moves the
//   same way. Two pieces of Chromium, kept close enough to the originals that they
//   can be diffed against them:
//
//     ui/gfx/geometry/cubic_bezier.cc                -> CubicBezier
//     cc/animation/scroll_offset_animation_curve.cc  -> ScrollAnimationCurve
//
//   The part that matters, and the part a hand-rolled exponential decay does not
//   give you, is UpdateTarget(): when a second wheel notch arrives while the first
//   one is still animating, the curve is rebuilt from the position the view is at
//   *and the speed it is travelling at*, so the motion never has a visible kink.
//   That is why browser scrolling reads as one continuous movement instead of a
//   series of restarts.
//
// Units:
// - The curve is unit-agnostic; the terminal drives it in fractional buffer rows.
//   Chromium's duration and epsilon constants are in pixels though, so callers pass
//   a pixelsPerUnit (the cell height) along with every retarget.

#pragma once

#include <array>

namespace Microsoft::Terminal::Core
{
    // ui/gfx/geometry/cubic_bezier.h - a cubic bezier with implicit endpoints
    // (0,0) and (1,1), solved for y given x.
    class CubicBezier
    {
    public:
        CubicBezier(const double p1x, const double p1y, const double p2x, const double p2y) noexcept;

        // y at the given x. Both are progress values; x is clamped to [0, 1].
        double Solve(const double x) const noexcept;
        // dy/dx at the given x. Used to read the current speed off a running curve.
        double Slope(const double x) const noexcept;

    private:
        // 'ax t^3 + bx t^2 + cx t', expanded with Horner's rule.
        double _SampleCurveX(const double t) const noexcept { return ((_ax * t + _bx) * t + _cx) * t; }
        double _SampleCurveY(const double t) const noexcept { return ((_ay * t + _by) * t + _cy) * t; }
        double _SampleCurveDerivativeX(const double t) const noexcept { return (3.0 * _ax * t + 2.0 * _bx) * t + _cx; }
        double _SampleCurveDerivativeY(const double t) const noexcept { return (3.0 * _ay * t + 2.0 * _by) * t + _cy; }
        double _SolveCurveX(const double x) const noexcept;

        static constexpr size_t SplineSamples = 11;

        double _ax{ 0 };
        double _bx{ 0 };
        double _cx{ 0 };
        double _ay{ 0 };
        double _by{ 0 };
        double _cy{ 0 };
        std::array<double, SplineSamples> _splineSamples{};
    };

    // cc/animation/scroll_offset_animation_curve.cc, ScrollType::kMouseWheel:
    // an ease-in-out bezier whose duration shrinks as the distance grows.
    class ScrollAnimationCurve
    {
    public:
        // Begins a new animation from `initial` to `target`. Any running one is dropped.
        void Start(const double initial, const double target, const double pixelsPerUnit, const double speed) noexcept;

        // Chromium's ScrollOffsetAnimationCurve::UpdateTarget(). `elapsed` is the time in
        // seconds since Start(). Preserves the current velocity across the retarget.
        void UpdateTarget(const double elapsed, const double target, const double pixelsPerUnit, const double speed) noexcept;

        // Chromium's ApplyAdjustment(): the whole animation slides by `adjustment`
        // because the content underneath it moved, not because the user scrolled.
        void ApplyAdjustment(const double adjustment) noexcept;

        void Reset() noexcept;

        // The position at `elapsed` seconds. Outside the animation this is the target.
        double ValueAt(const double elapsed) const noexcept;
        // Units per second at `elapsed`.
        double VelocityAt(const double elapsed) const noexcept;

        double Target() const noexcept { return _target; }
        double Duration() const noexcept { return _totalDuration; }
        bool IsRunning() const noexcept { return _running; }
        bool IsFinishedAt(const double elapsed) const noexcept { return !_running || elapsed >= _totalDuration; }

    private:
        double _SegmentDuration(const double delta, const double pixelsPerUnit, const double speed) const noexcept;
        double _BoundedSegmentDuration(const double newDelta, const double elapsed, const double pixelsPerUnit, const double speed) const noexcept;
        void _SetEasing(const double slope) noexcept;

        // cc uses kEaseInOutControlPoints for anything that is not a programmatic scroll.
        static constexpr double EaseInOutX1 = 0.42;
        static constexpr double EaseInOutY1 = 0.0;
        static constexpr double EaseInOutX2 = 0.58;
        static constexpr double EaseInOutY2 = 1.0;

        CubicBezier _easing{ EaseInOutX1, EaseInOutY1, EaseInOutX2, EaseInOutY2 };
        double _initial{ 0.0 };
        double _target{ 0.0 };
        // Both are measured from the start of the animation, as in cc.
        double _totalDuration{ 0.0 };
        double _lastRetarget{ 0.0 };
        bool _running{ false };
    };
}
