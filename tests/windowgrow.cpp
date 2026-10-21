#include "gtest/gtest.h"
#include "compat.h"

#include <WindowMgr.h>
#include <OSEvent.h>
#include <ToolboxEvent.h>
#include <vdriver/vdriver.h>
#include <wind/yorebridge.h>

// Run the WASM test binary under Node with YORE_ROOTLESS_WINDOWS=1.
// Real queued mouse-up events exercise
// GrowWindow's synchronous contract, including the release's final position.
struct RootlessGrow : public testing::Test
{
    WindowPtr window = nullptr;
    Rect content = {50, 50, 250, 350};
    Rect limits = {100, 120, 500, 600};
    Point start = {248, 348};

    void SetUp() override
    {
        if(!yoreRootlessEnabled())
            GTEST_SKIP();
        window = NewWindow(nullptr, &content, PSTR("Grow test"), false,
                           0, (WindowPtr)-1, true, 0);
        FlushEvents(everyEvent, 0);
    }

    void TearDown() override
    {
        if(window)
            DisposeWindow(window);
    }

    LONGINT releaseAt(Point point)
    {
        EventSink::instance->mouseMoved(point.h, point.v);
        EventSink::instance->mouseButtonEvent(false);
        EventSink::instance->pumpEvents();
        return GrowWindow(window, start, &limits);
    }
};

TEST_F(RootlessGrow, ReturnsReleaseSizeWithoutResizingTheApplication)
{
    const Rect before = window->portRect;
    LONGINT result = releaseAt({298, 428});
    EXPECT_EQ(380, result & 0xffff);
    EXPECT_EQ(250, (result >> 16) & 0xffff);
    EXPECT_EQ(before.right, window->portRect.right);
    EXPECT_EQ(before.bottom, window->portRect.bottom);

    // The caller owns the eventual resize; the next grow starts there.
    SizeWindow(window, result & 0xffff, (result >> 16) & 0xffff, true);
    start = {298, 428};
    result = releaseAt({278, 388});
    EXPECT_EQ(340, result & 0xffff);
    EXPECT_EQ(230, (result >> 16) & 0xffff);
    EXPECT_EQ(380, window->portRect.right - window->portRect.left);
    EXPECT_EQ(250, window->portRect.bottom - window->portRect.top);
}

TEST_F(RootlessGrow, NoMovementReturnsZero)
{
    EXPECT_EQ(0, releaseAt(start));
}

TEST_F(RootlessGrow, RespectsMinimumAndMaximumSize)
{
    LONGINT result = releaseAt({-1000, -1000});
    EXPECT_EQ(119, result & 0xffff); // classic PinRect uses min - 1
    EXPECT_EQ(99, (result >> 16) & 0xffff);
    result = releaseAt({30000, 30000});
    EXPECT_EQ(599, result & 0xffff); // right/bottom are exclusive
    EXPECT_EQ(499, (result >> 16) & 0xffff);
}
