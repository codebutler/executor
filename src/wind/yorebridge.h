#pragma once

/* Yore rootless-windows bridge (wasm host integration, not upstream).
 *
 * When enabled (env YORE_ROOTLESS_WINDOWS=1, set from the embedding page via
 * Module.ENV), Mac windows get host-compositor backing so each guest window
 * can be mounted as its own host window. Window private buffers published to
 * the host are always 32bpp (big-endian XRGB); the screen itself may be any
 * depth:
 *
 *   - 32bpp screen: every window (color or B&W) gets a PRIVATE 32bpp buffer.
 *     ROMlib_new_window_common points the fresh port's bits at the buffer
 *     with a BIASED baseAddr, so the classic dual-duty portBits.bounds
 *     (pixel addressing AND LocalToGlobal) keeps working unchanged.
 *
 *   - Depth == 1 (classic 1-bit): CGrafPort / color windows still get private
 *     32bpp buffers (QD blitters treat those baseAddrs as 32bpp winbufs).
 *     B&W GrafPort windows stay SCREEN-BACKED — portBits remain on
 *     screenBits so BufToScrn / ScreenRow smashers keep working; a separate
 *     32bpp display buffer is filled from the screenBits CONTENT crop on
 *     publish for the host compositor, while WDEF frame chrome is redirected
 *     into that display buffer (struct regions often extend above the screen).
 *
 *   - CalcVis stops subtracting the windows above: nothing occludes
 *     anything, each window always owns its full content pixels (private
 *     buffers) or draws into the shared screen (screen-backed B&W).
 *   - The QuickDraw dirty-rect funnel routes window-buffer damage into a
 *     per-window dirty rect instead of the global screen dirty list.
 *   - Window Manager mutations publish a seqlock'd snapshot table into
 *     plain wasm memory; the embedding page polls it (rAF) to mount/move/
 *     retitle/restack one host window per Mac window and blit dirty
 *     regions. An SPSC input ring carries mouse/key events back in, drained
 *     on the SDL thread into the normal IEventListener callbacks.
 */

#include <WindowMgr.h>
#include <cstdint>

namespace Executor
{
class IEventListener;

/* Env-gated master switch (cached after first call). */
bool yoreRootlessEnabled();

/* Window lifecycle (all called on the emulator thread). */
void yoreRootlessWindowCreated(WindowPeek w);   /* after OpenPort/OpenCPort + bounds/rect setup */
void yoreRootlessWindowDisposed(WindowPeek w);  /* from CloseWindow, before ClosePort */
void yoreRootlessWindowMoved(WindowPeek w);     /* after PORT_BOUNDS updated by MoveWindow */
void yoreRootlessWindowResized(WindowPeek w);   /* after PORT_RECT updated by SizeWindow */

/* After wCalcRgns: grow the backing buffer to cover the STRUCT region
 * (frame + content) so WDEF frame drawing lands in the buffer too. */
void yoreRootlessSyncFrame(WindowPeek w);

/* Before a window's first draw (ShowHide / visible NewWindow, after the
 * first wCalcRgns): if the WDEF's structure hangs off the screen's left or
 * top edge — apps place content assuming System 7's 1px frame, Platinum
 * needs 6/22px — shift the window on-screen so the chrome is visible.
 * Uses MoveWindow's rootless fast path (offset regions + re-bias, no
 * repaint), so it is safe mid-ShowHide. Only chrome-scale offsets are
 * nudged; windows parked far off-screen on purpose are left alone. */
void yoreRootlessNudgeOnscreen(WindowPeek w);

/* Rewrite the export table from LM(WindowList) (seqlock'd; cheap no-op when
 * disabled). Called from ROMlib_rootless_update plus title/hilite changes. */
void yoreRootlessPublish();

/* RAII: while alive, WMgrCPort's pixmap is REBOUND to `w`'s backing buffer:
 * baseAddr = the real buffer, bounds = the struct rect (so QuickDraw's
 * bounds/visRgn clipping covers the whole buffer even where the struct
 * hangs off the screen), visRgn extended over the struct, clip intersected
 * with the struct region. Frame drawing (WDEF wDraw/wDrawGIcon, content
 * erases) lands in the window's buffer instead of the invisible screen.
 * Applies to private-buffer AND screen-backed windows. Inert when disabled,
 * w is null/unregistered, or thePort isn't the window-manager port. */
class YoreFrameRedirect
{
public:
    explicit YoreFrameRedirect(WindowPeek w);
    ~YoreFrameRedirect();
    YoreFrameRedirect(const YoreFrameRedirect &) = delete;
    YoreFrameRedirect &operator=(const YoreFrameRedirect &) = delete;

private:
    bool active_ = false;
    void *savedBase_ = nullptr;
    int16_t savedRowBytes_ = 0;
    int16_t savedPixelSize_ = 0;
    Rect savedBounds_ = { 0, 0, 0, 0 };
    RgnHandle savedClip_ = nullptr;
    RgnHandle savedVis_ = nullptr;
};

/* True if this baseAddr (as stored in a BitMap/PixMap) is a registered
 * window buffer — the "screen-like bits" extension used by
 * active_screen_addr_p so depth inference and blit specialization treat
 * window buffers exactly like the screen. Screen-backed B&W window *ports*
 * stay on screenBits; their display buffers ARE registered here so
 * YoreFrameRedirect frame draws hit the 32bpp winbuf path. */
bool yoreRootlessIsWinBuf(uint32_t baseAddr);

/* Dirty-note hook for the QuickDraw accrue sites. Coordinates are
 * buffer-relative (same values the sites feed dirty_rect_accrue for the
 * screen). Returns true when the bits belong to a window buffer (caller
 * skips the global screen accrue). */
bool yoreRootlessNoteDirty(uint32_t baseAddr, int top, int left, int bottom, int right);

/* Screen-damage hook (from VideoDriver::updateScreen — covers QuickDraw's
 * dirty_rect_accrue AND refresh mode's flush_shadow_screen, which catches
 * apps writing screen memory directly): guest draws to the shared screen
 * land in screen-backed windows' content, so refresh the damaged sub-rect
 * of every intersecting screen-backed display buffer. Coordinates are
 * screen-relative (== guest-global for the screen). */
void yoreRootlessScreenDamaged(int top, int left, int bottom, int right);

/* Drain the input ring into the vdriver event callbacks. Called on the SDL
 * (front-end) thread each event-loop iteration. */
void yoreRootlessDrainInput(IEventListener *listener);

/* Menu tracking (from ROMlib_rootless_openmenu/closemenu): the open
 * dropdowns' screen rects, published in the table so the host can overlay
 * crops of the screen framebuffer while a menu is down. */
void yoreRootlessMenuOpen(Rect r);
void yoreRootlessMenuClose();

/* Cursor bridge: classic 16×16 data/mask cursor → ARGB in the table (the
 * SDL cursor lands on a detached canvas nobody sees). Called from the
 * vdriver's setCursor/setCursorVisible (any thread; atomics only). */
void yoreRootlessSetCursor(const char *data, const uint16_t mask[16],
                         int hotX, int hotY);
void yoreRootlessSetCursorVisible(bool visible);

/* Screen-resize request (from the JS input ring, drained on the SDL
 * thread). The SDL2 vdriver's updateMode() consumes it on the emulator
 * thread via doevent → gd_vdriver_mode_changed. */
void yoreRootlessRequestScreenSize(int w, int h);
bool yoreRootlessTakeScreenSizeRequest(int *w, int *h);

/* Host-activation → guest activation: the page requests a raise (ring
 * type 6) when the user mouses down on a window that isn't the guest's
 * front window; doevent consumes it BEFORE dequeuing the click, so
 * FindWindow hit-tests against the same stacking the user sees. */
void yoreRootlessHandleRaise();
}
