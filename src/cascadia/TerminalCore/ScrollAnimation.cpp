// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "ScrollAnimation.h"

#include <algorithm>
#include <cmath>

using namespace Microsoft::Terminal::Core;

namespace
{
    // ui/gfx/geometry/cubic_bezier.cc
    constexpr double BezierEpsilon = 1e-7;
    constexpr int MaxNewtonIterations = 4;

    // cc/animation/scroll_offset_animation_curve.cc. Durations are in frames at 60Hz;
    // kDurationDivisor turns them into seconds.
    constexpr double DurationDivisor = 60.0;
    constexpr double InverseDeltaRampStartPx = 120.0;
    constexpr double InverseDeltaRampEndPx = 480.0;
    constexpr double InverseDeltaMinDuration = 6.0;
    constexpr double InverseDeltaMaxDuration = 12.0;
    constexpr double InverseDeltaSlope = (InverseDeltaMinDuration - InverseDeltaMaxDuration) /
                                         (InverseDeltaRampEndPx - InverseDeltaRampStartPx);
    constexpr double InverseDeltaOffset = InverseDeltaMaxDuration - InverseDeltaRampStartPx * InverseDeltaSlope;

    // In pixels, and in pixels per second for the velocity guard.
    constexpr double CurveEpsilon = 0.01;

    // The "fudge factor to account for the ease out" from VelocityBasedDurationBound().
    constexpr double VelocityBoundFudge = 2.5;
}

#pragma region CubicBezier

CubicBezier::CubicBezier(const double p1x, const double p1y, const double p2x, const double p2y) noexcept
{
    // Polynomial coefficients; the first and last control points are implicitly
    // (0,0) and (1,1).
    _cx = 3.0 * p1x;
    _bx = 3.0 * (p2x - p1x) - _cx;
    _ax = 1.0 - _cx - _bx;

    _cy = 3.0 * p1y;
    _by = 3.0 * (p2y - p1y) - _cy;
    _ay = 1.0 - _cy - _by;

    // Sampled once so that _SolveCurveX() can start Newton from a good guess.
    const auto deltaT = 1.0 / (SplineSamples - 1);
    for (size_t i = 0; i < SplineSamples; i++)
    {
        _splineSamples[i] = _SampleCurveX(static_cast<double>(i) * deltaT);
    }
}

// Newton-Raphson with a bisection fallback, exactly as gfx::CubicBezier does it.
// The control points we use always have x in [0, 1], so the curve is monotonic in x
// and this has a single solution.
double CubicBezier::_SolveCurveX(const double x) const noexcept
{
    double t0 = 0.0;
    double t1 = 1.0;
    auto t2 = x;
    auto x2 = 0.0;

    const auto deltaT = 1.0 / (SplineSamples - 1);
    for (size_t i = 1; i < SplineSamples; i++)
    {
        if (x <= _splineSamples[i])
        {
            t1 = deltaT * static_cast<double>(i);
            t0 = t1 - deltaT;
            const auto span = _splineSamples[i] - _splineSamples[i - 1];
            t2 = span != 0.0 ? t0 + (t1 - t0) * (x - _splineSamples[i - 1]) / span : t0;
            break;
        }
    }

    for (auto i = 0; i < MaxNewtonIterations; i++)
    {
        x2 = _SampleCurveX(t2) - x;
        if (std::abs(x2) < BezierEpsilon)
        {
            return t2;
        }
        const auto d2 = _SampleCurveDerivativeX(t2);
        if (std::abs(d2) < BezierEpsilon)
        {
            break;
        }
        t2 = t2 - x2 / d2;
    }

    if (std::abs(x2) < BezierEpsilon)
    {
        return t2;
    }

    while (t0 < t1)
    {
        x2 = _SampleCurveX(t2);
        if (std::abs(x2 - x) < BezierEpsilon)
        {
            return t2;
        }
        if (x > x2)
        {
            t0 = t2;
        }
        else
        {
            t1 = t2;
        }
        t2 = (t1 + t0) * 0.5;
    }

    return t2;
}

double CubicBezier::Solve(const double x) const noexcept
{
    // gfx::CubicBezier extrapolates outside [0, 1] using the end-point gradients.
    // We never evaluate outside the animation's own duration, so clamping is enough.
    return _SampleCurveY(_SolveCurveX(std::clamp(x, 0.0, 1.0)));
}

double CubicBezier::Slope(const double x) const noexcept
{
    const auto t = _SolveCurveX(std::clamp(x, 0.0, 1.0));
    const auto dx = _SampleCurveDerivativeX(t);
    const auto dy = _SampleCurveDerivativeY(t);
    if (dx == 0.0)
    {
        return 0.0;
    }
    return dy / dx;
}

#pragma endregion

#pragma region ScrollAnimationCurve

// EaseInOutWithInitialSlope(): scale the first control point so the curve leaves the
// starting position at the speed the previous curve was travelling at.
void ScrollAnimationCurve::_SetEasing(const double slope) noexcept
{
    const auto clamped = std::clamp(slope, -1000.0, 1000.0);
    _easing = CubicBezier{ EaseInOutX1, EaseInOutX1 * clamped, EaseInOutX2, EaseInOutY2 };
}

// EaseInOutSegmentDuration() with DurationBehavior::kInverseDelta - the behaviour cc
// picks for ScrollType::kMouseWheel. Longer scrolls run *shorter*: 200ms for anything
// up to 120px, ramping down to 100ms at 480px and beyond.
double ScrollAnimationCurve::_SegmentDuration(const double delta, const double pixelsPerUnit, const double speed) const noexcept
{
    const auto deltaPx = std::abs(delta) * pixelsPerUnit;
    auto duration = InverseDeltaOffset + deltaPx * InverseDeltaSlope;
    duration = std::clamp(duration, InverseDeltaMinDuration, InverseDeltaMaxDuration);
    duration /= DurationDivisor;
    // Our own knob. speed > 1 shortens the animation, which is what "faster" means here.
    return duration / std::max(0.1, speed);
}

// VelocityBasedDurationBound() folded into EaseInOutBoundedSegmentDuration(): when we
// are already moving quickly and the new target is close, use the time it would take to
// coast there instead of the nominal duration. Without this a fast flick that ends on a
// short hop overshoots and rubber-bands back.
double ScrollAnimationCurve::_BoundedSegmentDuration(const double newDelta, const double elapsed, const double pixelsPerUnit, const double speed) const noexcept
{
    const auto nominal = _SegmentDuration(newDelta, pixelsPerUnit, speed);

    const auto newDeltaPx = newDelta * pixelsPerUnit;
    if (std::abs(newDeltaPx) < CurveEpsilon)
    {
        return 0.0;
    }

    const auto velocityPx = VelocityAt(elapsed) * pixelsPerUnit;
    if (std::abs(velocityPx) < CurveEpsilon)
    {
        return nominal;
    }

    const auto bound = (newDeltaPx / velocityPx) * VelocityBoundFudge;
    // A negative bound means we are travelling away from the new target; there is
    // nothing to coast into, so let the nominal duration stand.
    return bound < 0.0 ? nominal : std::min(nominal, bound);
}

void ScrollAnimationCurve::Start(const double initial, const double target, const double pixelsPerUnit, const double speed) noexcept
{
    _SetEasing(0.0);
    _initial = initial;
    _target = target;
    _lastRetarget = 0.0;
    _totalDuration = _SegmentDuration(target - initial, pixelsPerUnit, speed);
    _running = true;
}

void ScrollAnimationCurve::Reset() noexcept
{
    _running = false;
    _initial = 0.0;
    _target = 0.0;
    _totalDuration = 0.0;
    _lastRetarget = 0.0;
    _SetEasing(0.0);
}

void ScrollAnimationCurve::ApplyAdjustment(const double adjustment) noexcept
{
    _initial += adjustment;
    _target += adjustment;
}

double ScrollAnimationCurve::ValueAt(const double elapsed) const noexcept
{
    const auto duration = _totalDuration - _lastRetarget;
    const auto t = elapsed - _lastRetarget;

    if (duration <= 0.0 || t >= duration)
    {
        return _target;
    }
    if (t <= 0.0)
    {
        return _initial;
    }

    const auto progress = _easing.Solve(t / duration);
    return _initial + (_target - _initial) * progress;
}

double ScrollAnimationCurve::VelocityAt(const double elapsed) const noexcept
{
    const auto duration = _totalDuration - _lastRetarget;
    if (duration <= 0.0)
    {
        return 0.0;
    }

    // The timing function only gives the slope of the progress curve; scale it into
    // units per second.
    const auto slope = _easing.Slope((elapsed - _lastRetarget) / duration);
    return slope * ((_target - _initial) / duration);
}

void ScrollAnimationCurve::UpdateTarget(const double elapsed, const double target, const double pixelsPerUnit, const double speed) noexcept
{
    if (!_running)
    {
        Start(ValueAt(elapsed), target, pixelsPerUnit, speed);
        return;
    }

    const auto t = std::max(elapsed, _lastRetarget);

    // cc leaves an ease-in-out animation alone when the target has not really moved:
    // the duration grows as the distance shrinks, so restarting would make the
    // animation *longer* than the one already running.
    if (std::abs((_target - target) * pixelsPerUnit) < CurveEpsilon)
    {
        _target = target;
        return;
    }

    const auto current = ValueAt(t);
    const auto newDelta = target - current;

    // Already there.
    if (std::abs(newDelta * pixelsPerUnit) < CurveEpsilon)
    {
        _lastRetarget = t;
        _totalDuration = t;
        _target = target;
        return;
    }

    // The previous segment had no duration to speak of, so there is no velocity to
    // carry over - start a plain new one.
    if (_totalDuration - _lastRetarget <= 0.0)
    {
        _initial = current;
        _target = target;
        _lastRetarget = t;
        _totalDuration = t + _SegmentDuration(newDelta, pixelsPerUnit, speed);
        _SetEasing(0.0);
        return;
    }

    const auto newDuration = _BoundedSegmentDuration(newDelta, t, pixelsPerUnit, speed);
    if (newDuration < CurveEpsilon)
    {
        _target = target;
        _totalDuration = t;
        return;
    }

    // This is the whole point of the exercise: rebuild the curve so that it leaves
    // `current` at the speed we are already travelling at. Everything else is a
    // straightforward restart; this is what keeps repeated notches feeling like one
    // continuous movement instead of a stutter.
    const auto velocity = VelocityAt(t);
    const auto newSlope = velocity * (newDuration / newDelta);

    _SetEasing(newSlope);
    _initial = current;
    _target = target;
    _totalDuration = t + newDuration;
    _lastRetarget = t;
}

#pragma endregion
