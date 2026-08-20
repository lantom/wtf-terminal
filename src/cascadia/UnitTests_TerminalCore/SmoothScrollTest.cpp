// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "../cascadia/TerminalCore/Terminal.hpp"
#include "../renderer/inc/DummyRenderer.hpp"
#include "MockTermSettings.h"

using namespace winrt::Microsoft::Terminal::Core;
using namespace Microsoft::Terminal::Core;
using namespace Microsoft::Console::Render;
using namespace ::Microsoft::Console::Types;

using namespace WEX::Common;
using namespace WEX::Logging;
using namespace WEX::TestExecution;

namespace TerminalCoreUnitTests
{
    class SmoothScrollTest;
};
using namespace TerminalCoreUnitTests;

class TerminalCoreUnitTests::SmoothScrollTest final
{
    static constexpr til::CoordType ViewWidth = 80;
    static constexpr til::CoordType ViewHeight = 32;
    static constexpr til::CoordType HistoryLength = 500;
    // The cell size the tests pin the Terminal to in MethodSetup. Every pixel
    // expectation below is expressed in terms of it.
    static constexpr til::CoordType CellHeight = 16;
    static constexpr til::CoordType CellWidth = 8;

    // One 120Hz frame. The animation is time based, so the tests drive it with an
    // explicit time step instead of the wall clock.
    static constexpr double Frame = 1.0 / 120.0;

    TEST_CLASS(SmoothScrollTest);

    TEST_METHOD(OffByDefaultInTheCore);
    TEST_METHOD(ScrollingIsUnchangedWhenDisabled);
    TEST_METHOD(AnimationConvergesOnTheTarget);
    TEST_METHOD(PixelShiftStaysWithinOneCell);
    TEST_METHOD(RenderViewportGrowsByExactlyOneRow);
    TEST_METHOD(PositionMatchesRowPlusShift);
    TEST_METHOD(HigherSpeedConvergesSooner);
    TEST_METHOD(FractionalTargetsSurvive);
    TEST_METHOD(TargetIsClampedToTheBuffer);
    TEST_METHOD(DisablingSnapsBackToAWholeRow);
    TEST_METHOD(SnapOnInputCancelsTheAnimation);
    TEST_METHOD(AltBufferNeverShifts);
    TEST_METHOD(SettingsAreClamped);
    TEST_METHOD(OutputWhileScrolledBackKeepsThePosition);

    TEST_METHOD_SETUP(MethodSetup)
    {
        _term = std::make_unique<Terminal>(Terminal::TestDummyMarker{});
        _renderer = std::make_unique<DummyRenderer>(_term.get());
        _term->Create({ ViewWidth, ViewHeight }, HistoryLength, *_renderer);

        // Pin the cell size: every pixel expectation below is expressed in terms of it.
        {
            const auto lock = _term->LockForWriting();
            _term->SetFontInfo(FontInfo{ DEFAULT_FONT_FACE, TMPF_TRUETYPE, 10, { CellWidth, CellHeight }, CP_UTF8, false });
        }
        VERIFY_ARE_EQUAL(CellHeight, _term->GetFontInfo().GetSize().height);
        return true;
    }

    TEST_METHOD_CLEANUP(MethodCleanup)
    {
        _renderer = nullptr;
        _term = nullptr;
        return true;
    }

private:
    // Fills the scrollback so that there is something to scroll through.
    void _fillScrollback(til::CoordType lines = 200)
    {
        auto& sm = *_term->_stateMachine;
        for (til::CoordType i = 0; i < lines; ++i)
        {
            sm.ProcessString(L"line\r\n");
        }
    }

    void _enableSmoothScrolling(double speed = 1.0)
    {
        _term->SetSmoothScrollingSettings(true, speed);
    }

    // Runs the animation until it settles, or gives up. Returns the number of frames it
    // took. The cap is deliberately generous; the assertions are about convergence, not
    // about a specific frame count.
    int _runToCompletion(int maxFrames = 4000)
    {
        auto frames = 0;
        while (frames < maxFrames && _term->_StepScrollAnimation(Frame))
        {
            ++frames;
        }
        return frames;
    }

    std::unique_ptr<Terminal> _term;
    std::unique_ptr<DummyRenderer> _renderer;
};

// The core must not opt anybody into smooth scrolling on its own - that is the
// settings layer's job. Everything has to behave exactly as before until then.
void SmoothScrollTest::OffByDefaultInTheCore()
{
    VERIFY_IS_FALSE(_term->IsSmoothScrollingEnabled());
    VERIFY_ARE_EQUAL(0, _term->GetScrollPixelShift());
    VERIFY_IS_FALSE(_term->AdvanceScrollAnimation(), L"nothing to animate");
    VERIFY_ARE_EQUAL(_term->GetViewport().Dimensions(), _term->GetRenderViewport().Dimensions());
}

void SmoothScrollTest::ScrollingIsUnchangedWhenDisabled()
{
    _fillScrollback();
    const auto bottom = _term->GetScrollOffset();

    _term->UserScrollViewport(bottom - 10);
    VERIFY_ARE_EQUAL(bottom - 10, _term->GetScrollOffset(), L"the viewport moves immediately");
    VERIFY_ARE_EQUAL(0, _term->GetScrollPixelShift());
    VERIFY_ARE_EQUAL(_term->GetViewport().Dimensions(), _term->GetRenderViewport().Dimensions());

    // Even the fractional entry point has to round and apply straight away.
    _term->SmoothScrollToRow(bottom - 20.4);
    VERIFY_ARE_EQUAL(bottom - 20, _term->GetScrollOffset());
    VERIFY_ARE_EQUAL(0, _term->GetScrollPixelShift());
    VERIFY_IS_FALSE(_term->AdvanceScrollAnimation());
}

void SmoothScrollTest::AnimationConvergesOnTheTarget()
{
    _fillScrollback();
    _enableSmoothScrolling();

    const auto bottom = _term->GetScrollOffset();
    const auto target = bottom - 10;

    _term->SmoothScrollToRow(target);
    VERIFY_ARE_NOT_EQUAL(target, _term->GetScrollOffset(), L"the viewport must not jump to the target");

    const auto frames = _runToCompletion();
    VERIFY_IS_GREATER_THAN(frames, 1, L"the move has to take more than a single frame");
    VERIFY_IS_LESS_THAN(frames, 200, L"...but it must not crawl");

    VERIFY_ARE_EQUAL(target, _term->GetScrollOffset());
    VERIFY_ARE_EQUAL(0, _term->GetScrollPixelShift(), L"an integer target settles on a whole row");
    VERIFY_IS_FALSE(_term->AdvanceScrollAnimation(), L"a settled animation stops asking for frames");
}

void SmoothScrollTest::PixelShiftStaysWithinOneCell()
{
    _fillScrollback();
    _enableSmoothScrolling();

    const auto bottom = _term->GetScrollOffset();
    _term->SmoothScrollToRow(bottom - 25);

    auto sawNonZeroShift = false;
    for (auto i = 0; i < 2000 && _term->_StepScrollAnimation(Frame); ++i)
    {
        const auto shift = _term->GetScrollPixelShift();
        VERIFY_IS_GREATER_THAN_OR_EQUAL(shift, 0);
        VERIFY_IS_LESS_THAN(shift, CellHeight, L"the shift is always a sub-cell amount");
        sawNonZeroShift = sawNonZeroShift || shift > 0;
    }

    VERIFY_IS_TRUE(sawNonZeroShift, L"a smooth scroll has to spend time between two rows");
}

void SmoothScrollTest::RenderViewportGrowsByExactlyOneRow()
{
    _fillScrollback();
    _enableSmoothScrolling();

    const auto bottom = _term->GetScrollOffset();
    _term->SmoothScrollToRow(bottom - 25);

    auto checked = 0;
    for (auto i = 0; i < 2000 && _term->_StepScrollAnimation(Frame); ++i)
    {
        const auto viewport = _term->GetViewport();
        const auto renderViewport = _term->GetRenderViewport();

        VERIFY_ARE_EQUAL(viewport.Origin(), renderViewport.Origin(), L"only the height may differ");
        VERIFY_ARE_EQUAL(viewport.Width(), renderViewport.Width());

        // The extra row is present for the whole animation, not only on the frames
        // where the shift happens to be non-zero.
        VERIFY_ARE_EQUAL(viewport.Height() + 1, renderViewport.Height());

        // The extra row must exist in the buffer, otherwise the renderer would read
        // past the end of it.
        VERIFY_IS_LESS_THAN_OR_EQUAL(renderViewport.BottomExclusive(), _term->GetBufferHeight());
        ++checked;
    }

    VERIFY_IS_GREATER_THAN(checked, 0);
}

// The whole point of the feature: the position actually rendered is
// (row * cellHeight + shift) pixels, and it has to track the animation faithfully.
void SmoothScrollTest::PositionMatchesRowPlusShift()
{
    _fillScrollback();
    _enableSmoothScrolling();

    const auto bottom = _term->GetScrollOffset();
    _term->SmoothScrollToRow(bottom - 25);

    for (auto i = 0; i < 2000 && _term->_StepScrollAnimation(Frame); ++i)
    {
        // The first row painted is GetScrollOffset(); lifting the frame by
        // GetScrollPixelShift() puts the top edge of the control that far into it.
        const auto renderedTopInPx = static_cast<double>(_term->GetScrollOffset()) * CellHeight + _term->GetScrollPixelShift();
        const auto animatedTopInPx = _term->GetSmoothScrollCurrentRow() * CellHeight;

        // Rounding to a whole device pixel is the only permitted deviation.
        const auto errorInPx = std::abs(renderedTopInPx - animatedTopInPx);
        VERIFY_IS_TRUE(errorInPx <= 0.5 + 1e-9,
                       String().Format(L"rendered top is off by %f px", errorInPx));
    }
}

void SmoothScrollTest::HigherSpeedConvergesSooner()
{
    _fillScrollback();

    const auto measure = [this](double speed) {
        _term->SetSmoothScrollingSettings(false, 1.0);
        _term->UserScrollViewport(_term->GetScrollOffset());
        _term->SetSmoothScrollingSettings(true, speed);

        const auto bottom = _term->GetScrollOffset();
        _term->SmoothScrollToRow(bottom - 30);
        return _runToCompletion();
    };

    const auto slow = measure(0.5);
    const auto normal = measure(1.0);
    const auto fast = measure(4.0);

    Log::Comment(String().Format(L"frames: slow=%d normal=%d fast=%d", slow, normal, fast));
    VERIFY_IS_LESS_THAN(fast, normal, L"a higher speed setting must settle sooner");
    VERIFY_IS_LESS_THAN(normal, slow, L"a lower speed setting must settle later");
}

// A precision trackpad produces deltas smaller than a row. Those must not be rounded
// away - that is what made the old code jump a whole line at a time.
void SmoothScrollTest::FractionalTargetsSurvive()
{
    _fillScrollback();
    _enableSmoothScrolling();

    const auto bottom = _term->GetScrollOffset();
    const auto target = bottom - 4.5;

    _term->SmoothScrollToRow(target);
    _runToCompletion();

    // A viewport top of bottom-4.5 means row bottom-5 is the first one painted, with
    // its upper half scrolled off the top of the control.
    VERIFY_ARE_EQUAL(bottom - 5, _term->GetScrollOffset());
    VERIFY_ARE_EQUAL(CellHeight / 2, _term->GetScrollPixelShift(), L"half a row lands as half a cell of shift");
}

void SmoothScrollTest::TargetIsClampedToTheBuffer()
{
    _fillScrollback();
    _enableSmoothScrolling();

    // Far past the top of the scrollback.
    _term->SmoothScrollToRow(-1000.0);
    _runToCompletion();
    VERIFY_ARE_EQUAL(0, _term->GetScrollOffset(), L"cannot scroll above the first row");
    VERIFY_ARE_EQUAL(0, _term->GetScrollPixelShift());

    // Far past the bottom.
    _term->SmoothScrollToRow(1e9);
    _runToCompletion();
    VERIFY_ARE_EQUAL(_term->ViewStartIndex(), _term->GetScrollOffset(), L"cannot scroll below the last row");
    VERIFY_ARE_EQUAL(0, _term->GetScrollPixelShift(), L"resting at the bottom leaves no shift behind");
}

void SmoothScrollTest::DisablingSnapsBackToAWholeRow()
{
    _fillScrollback();
    _enableSmoothScrolling();

    const auto bottom = _term->GetScrollOffset();
    _term->SmoothScrollToRow(bottom - 7.5);
    _runToCompletion();
    VERIFY_IS_GREATER_THAN(_term->GetScrollPixelShift(), 0);

    _term->SetSmoothScrollingSettings(false, 1.0);

    VERIFY_ARE_EQUAL(0, _term->GetScrollPixelShift(), L"turning the feature off must not leave a partial row on screen");
    VERIFY_ARE_EQUAL(_term->GetViewport().Dimensions(), _term->GetRenderViewport().Dimensions(), L"and the renderer goes back to painting exactly the viewport");
    VERIFY_IS_FALSE(_term->AdvanceScrollAnimation());
}

void SmoothScrollTest::SnapOnInputCancelsTheAnimation()
{
    _fillScrollback();
    _enableSmoothScrolling();

    const auto bottom = _term->GetScrollOffset();
    _term->SmoothScrollToRow(bottom - 20);
    _term->_StepScrollAnimation(Frame);
    VERIFY_ARE_NOT_EQUAL(bottom, _term->GetScrollOffset());

    _term->TrySnapOnInput();

    VERIFY_ARE_EQUAL(bottom, _term->GetScrollOffset(), L"typing jumps straight back to the bottom");
    VERIFY_ARE_EQUAL(0, _term->GetScrollPixelShift());
    VERIFY_IS_FALSE(_term->AdvanceScrollAnimation(), L"and leaves no animation running");
}

// The alternate buffer has no scrollback, so there is nothing to scroll smoothly
// through and no spare row to paint into.
void SmoothScrollTest::AltBufferNeverShifts()
{
    _fillScrollback();
    _enableSmoothScrolling();

    auto& sm = *_term->_stateMachine;
    sm.ProcessString(L"\x1b[?1049h");
    VERIFY_IS_TRUE(_term->_inAltBuffer());

    _term->SmoothScrollToRow(0);
    VERIFY_ARE_EQUAL(0, _term->GetScrollPixelShift());
    VERIFY_IS_FALSE(_term->AdvanceScrollAnimation());
    VERIFY_ARE_EQUAL(_term->GetViewport().Dimensions(), _term->GetRenderViewport().Dimensions());

    sm.ProcessString(L"\x1b[?1049l");
    VERIFY_IS_FALSE(_term->_inAltBuffer());
}

void SmoothScrollTest::SettingsAreClamped()
{
    _term->SetSmoothScrollingSettings(true, 0.0);
    VERIFY_ARE_EQUAL(0.1, _term->_smoothScrollSpeed, L"a zero speed would stall the animation forever");

    _term->SetSmoothScrollingSettings(true, -3.0);
    VERIFY_ARE_EQUAL(0.1, _term->_smoothScrollSpeed, L"a negative speed would run the animation backwards");

    _term->SetSmoothScrollingSettings(true, 1000.0);
    VERIFY_ARE_EQUAL(5.0, _term->_smoothScrollSpeed);

    _term->SetSmoothScrollingSettings(true, 2.5);
    VERIFY_ARE_EQUAL(2.5, _term->_smoothScrollSpeed);
}

// Output arriving while the user is scrolled back must not drag the view along with it.
// The offset counting rows above the viewport grows by the same amount the viewport
// moved down, and the sub-row shift has to survive that untouched.
void SmoothScrollTest::OutputWhileScrolledBackKeepsThePosition()
{
    _fillScrollback();
    _enableSmoothScrolling();

    const auto bottom = _term->GetScrollOffset();
    _term->SmoothScrollToRow(bottom - 6.5);
    _runToCompletion();

    const auto topRowBefore = _term->GetScrollOffset();
    const auto shiftBefore = _term->GetScrollPixelShift();
    VERIFY_ARE_EQUAL(CellHeight / 2, shiftBefore);

    auto& sm = *_term->_stateMachine;
    for (auto i = 0; i < 5; ++i)
    {
        sm.ProcessString(L"more output\r\n");
    }

    VERIFY_ARE_EQUAL(topRowBefore, _term->GetScrollOffset(), L"the same buffer row is still on top");
    VERIFY_ARE_EQUAL(shiftBefore, _term->GetScrollPixelShift(), L"and at the same sub-row offset");
}
