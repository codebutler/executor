/* pc rootless-windows bridge — see pcbridge.h for the model.
 *
 * Everything here runs on the emulator (Toolbox) thread except
 * pcRootlessDrainInput (SDL front-end thread) and the JS reader, which
 * polls the seqlock'd Table / writes dirtyAck + the ring head from the
 * browser main thread. Shared fields are touched through __atomic
 * builtins; wasm shared memory makes them interoperable with JS Atomics.
 *
 * Geometry model: the backing buffer covers the window's STRUCT rect
 * (frame + content; buffer origin = struct top-left in guest-global
 * coords). Until the first wCalcRgns the struct rect is assumed equal to
 * the content rect; pcRootlessSyncFrame grows the buffer once the WDEF
 * has computed the real regions. The baseAddr stored in the window port
 * (and temporarily in WMgrCPort during PcFrameRedirect) is BIASED so that
 * classic bounds-based addressing hits the buffer: real − (sy·rowBytes +
 * sx·4). The dirty-note sites hand us GLOBAL coordinates (rect minus the
 * bitmap's bounds top-left is global for every biased bitmap by
 * construction); we translate by (sx,sy) into buffer coords.
 */

#include <base/common.h>
#include <QuickDraw.h>
#include <CQuickDraw.h>
#include <WindowMgr.h>
#include <MenuMgr.h>
#include <SegmentLdr.h>

#include <quickdraw/cquick.h>
#include <wind/wind.h>
#include <wind/pcbridge.h>
#include <vdriver/vdriver.h>

#include <algorithm>
#include <atomic>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#ifdef EMSCRIPTEN
#include <emscripten.h>
#define PC_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define PC_EXPORT
#endif

using namespace Executor;

namespace
{
constexpr uint32_t PC_MAGIC = 0x70435257; /* 'pCRW' */
constexpr uint32_t PC_VERSION = 4;
constexpr int MAX_WINS = 64;
constexpr int TITLE_BYTES = 48;
constexpr int RING_CAP = 256;

/* Kept in byte-for-byte sync with the TS reader (js/apps/executor). */
struct WinSlot
{
    uint32_t hwnd;
    uint32_t flags; /* bit0 visible, bit1 hilited, bit2 shell strip
                       (the Browser launcher band — see shellStripP) */
    int32_t zorder; /* 0 = frontmost */
    int32_t gx, gy; /* content origin, guest-global coords */
    int32_t w, h; /* content size */
    uint32_t buf; /* REAL buffer base (unbiased) */
    uint32_t rowBytes;
    uint32_t dirtySeq; /* engine bumps after updating the dirty rect */
    uint32_t dirtyAck; /* JS stores the last dirtySeq it blitted */
    int32_t dl, dt, dr, db; /* dirty rect, buffer-local */
    uint32_t titleSeq;
    int32_t sx, sy; /* struct (buffer) origin, guest-global */
    int32_t sw, sh; /* struct (buffer) size */
    uint8_t title[TITLE_BYTES]; /* pascal string, truncated */
};
static_assert(sizeof(WinSlot) == 128, "WinSlot layout is part of the JS ABI");

struct MenuRect
{
    int32_t l, t, r, b;
};

struct Table
{
    uint32_t magic, version, seq, count;
    uint32_t screenW, screenH, bpp, mbarHeight;
    uint32_t frontHwnd;
    uint32_t screenBuf; /* the vdriver framebuffer (menubar + open menus) */
    uint32_t screenRowBytes;
    uint32_t menuCount;
    uint32_t reserved[4];
    WinSlot wins[MAX_WINS];
    MenuRect menus[8];
    /* cursor bridge: 16×16 ARGB (host byte order: [B,G,R,A] little-endian
     * words written as uint32), hotspot, visibility; seq bumps per change */
    uint32_t cursorSeq;
    uint32_t cursorVisible;
    int32_t cursorHotX, cursorHotY;
    uint32_t cursorPixels[16 * 16];
};

struct InputRec
{
    uint32_t type; /* 0 move, 1 down, 2 up, 3 keydown, 4 keyup,
                    * 5 resize-screen, 6 raise-window (aux = hwnd) */
    int32_t x, y, aux; /* aux: Mac virtual keycode for key events */
};

struct InputRing
{
    uint32_t head; /* writer: JS */
    uint32_t tail; /* reader: SDL thread */
    uint32_t capacity;
    uint32_t reserved;
    InputRec recs[RING_CAP];
};

Table table = { PC_MAGIC, PC_VERSION, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, {}, {}, {}, 0, 1, 0, 0, {} };
InputRing ring = { 0, 0, RING_CAP, 0, {} };

struct BufRec
{
    WindowPeek w;
    uint8_t *bytes; /* real (unbiased) buffer */
    int sw, sh, rowBytes; /* buffer (struct) dims */
    int sx, sy; /* buffer origin, guest-global */
    int gx, gy; /* content origin, guest-global */
    int cw, ch; /* content size */
    int fl, ft; /* frame insets: content − struct origin */
    uint32_t biased; /* the baseAddr value stored in the port's bits */
    int slot;
    bool shellApp; /* created while the shell (Browser) was CurApName */
};

std::unordered_map<uint32_t, BufRec> byWin; /* WindowPeek → rec */
std::unordered_map<uint32_t, BufRec *> byBiased; /* biased baseAddr → rec */
std::vector<Rect> openMenus; /* dropdown screen rects, innermost last */

inline uint32_t winKey(WindowPeek w) { return (uint32_t)(uintptr_t)w; }

/* Is the currently running application the shell (the Browser)? Executor
 * installs its bundled Browser as the Finder (LM(FinderName), executor.cpp)
 * and launch.cpp stamps LM(CurApName) per launch — a Pascal-string compare
 * of the two identifies shell-owned windows with no geometry heuristics. */
inline bool shellIsCurApp()
{
    const uint8_t *cur = (const uint8_t *)LM(CurApName);
    const uint8_t *fin = (const uint8_t *)LM(FinderName);
    return cur[0] && cur[0] == fin[0] && memcmp(cur + 1, fin + 1, cur[0]) == 0;
}

/* The Browser's launcher band: a shell-owned, untitled window spanning the
 * full screen width flush under the menu bar. The Browser reserves that
 * band (its DragWindow limit rect pins other windows below it), so the
 * host may treat it as system chrome rather than a document window. */
inline bool shellStripP(const BufRec &r, WindowPeek wp)
{
    if(!r.shellApp)
        return false;
    StringHandle th = WINDOW_TITLE(wp);
    if(th && *th && (*th)[0])
        return false;
    return r.sw >= (int)table.screenW && r.sy <= (int)table.mbarHeight;
}

inline void atomicStore(uint32_t *p, uint32_t v)
{
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}
inline uint32_t atomicLoad(const uint32_t *p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

void contentGeometry(WindowPeek w, int *gx, int *gy, int *cw, int *ch)
{
    /* Global content origin: portRect.topLeft - portBits.bounds.topLeft
     * (invariant under SetOrigin). */
    *gx = PORT_RECT(w).left - PORT_BOUNDS(w).left;
    *gy = PORT_RECT(w).top - PORT_BOUNDS(w).top;
    *cw = std::max(1, PORT_RECT(w).right - PORT_RECT(w).left);
    *ch = std::max(1, PORT_RECT(w).bottom - PORT_RECT(w).top);
}

/* baseAddr bias: buffer[0] is the pixel at global (sx,sy); the blitters
 * address via bounds, so baseAddr = real - (sy*rowBytes + sx*4). uint32
 * wraparound in the intermediate is fine — offsets re-wrap exactly
 * (wasm32 pointer math is modular). */
uint32_t biasedAddr(const BufRec &r)
{
    return (uint32_t)(uintptr_t)r.bytes
        - (uint32_t)(r.sy * r.rowBytes + r.sx * 4);
}

void setPortBaseAddr(WindowPeek w, uint32_t biased, int rowBytes)
{
    if(CGrafPort_p(w))
    {
        PixMapHandle pm = CPORT_PIXMAP_X_NO_ASSERT((CGrafPtr)w);
        PIXMAP_BASEADDR(pm) = (Ptr)(uintptr_t)biased;
        (*pm)->rowBytes = (int16_t)(rowBytes | ((*pm)->rowBytes & 0xC000));
    }
    else
    {
        GrafPtr gp = (GrafPtr)w;
        gp->portBits.baseAddr = (Ptr)(uintptr_t)biased;
        gp->portBits.rowBytes = (int16_t)rowBytes;
    }
}

bool screen32()
{
    return PIXMAP_PIXEL_SIZE(GD_PMAP(LM(MainDevice))) == 32;
}

int freeSlot()
{
    for(int i = 0; i < MAX_WINS; i++)
        if(table.wins[i].hwnd == 0)
            return i;
    return -1;
}

void publishGeometry(const BufRec &r)
{
    WinSlot &s = table.wins[r.slot];
    s.gx = r.gx;
    s.gy = r.gy;
    s.w = r.cw;
    s.h = r.ch;
    s.sx = r.sx;
    s.sy = r.sy;
    s.sw = r.sw;
    s.sh = r.sh;
    s.buf = (uint32_t)(uintptr_t)r.bytes;
    s.rowBytes = (uint32_t)r.rowBytes;
}

void markFullDirty(BufRec &r)
{
    WinSlot &s = table.wins[r.slot];
    uint32_t seq = atomicLoad(&s.dirtySeq);
    s.dl = 0;
    s.dt = 0;
    s.dr = r.sw;
    s.db = r.sh;
    atomicStore(&s.dirtySeq, seq + 1);
}

/* Drop a registry record without touching the WindowRecord: used when the
 * guest ABANDONED the window (InitWindows at app launch resets
 * LM(WindowList) without CloseWindow) — the record's memory may already be
 * freed or reused, so only our own state is safe to touch. */
void dropRecordNoPort(uint32_t key)
{
    auto it = byWin.find(key);
    if(it == byWin.end())
        return;
    BufRec &r = it->second;
    table.wins[r.slot].hwnd = 0;
    byBiased.erase(r.biased);
    free(r.bytes);
    byWin.erase(it);
}

void rebias(BufRec &r, WindowPeek w)
{
    byBiased.erase(r.biased);
    r.biased = biasedAddr(r);
    byBiased[r.biased] = &r;
    setPortBaseAddr(w, r.biased, r.rowBytes);
}
} /* namespace */

/* Pending cross-thread requests: written on the SDL thread (ring drain),
 * consumed on the emulator thread (updateMode / doevent). */
static uint32_t pendingScreenSize = 0; /* (w<<16)|h, 0 = none */
static uint32_t pendingRaise = 0; /* hwnd, 0 = none */

bool Executor::pcRootlessEnabled()
{
    static int cached = -1;
    if(cached < 0)
    {
        const char *v = getenv("PC_ROOTLESS_WINDOWS");
        cached = (v && v[0] == '1') ? 1 : 0;
    }
    return cached == 1;
}

void Executor::pcRootlessWindowCreated(WindowPeek w)
{
    if(!pcRootlessEnabled() || !screen32())
        return;
    /* A live window can't share an address with another live window: an
     * existing entry here means the old WindowRecord was abandoned (app
     * launch) and the heap recycled its address. Drop the stale record —
     * otherwise this window would get NO buffer and draw to the invisible
     * screen ("windows stop drawing" after launching an app). */
    if(byWin.count(winKey(w)))
        dropRecordNoPort(winKey(w));

    int slot = freeSlot();
    if(slot < 0)
    {
        warning_unexpected("pc rootless: out of window slots");
        return;
    }

    BufRec r;
    r.w = w;
    contentGeometry(w, &r.gx, &r.gy, &r.cw, &r.ch);
    r.sx = r.gx;
    r.sy = r.gy;
    r.sw = r.cw;
    r.sh = r.ch;
    r.fl = 0;
    r.ft = 0;
    r.rowBytes = r.sw * 4;
    r.slot = slot;
    r.shellApp = shellIsCurApp();
    r.bytes = (uint8_t *)malloc((size_t)r.rowBytes * r.sh);
    if(!r.bytes)
        return;
    memset(r.bytes, 0xFF, (size_t)r.rowBytes * r.sh); /* white */

    auto [it, ok] = byWin.emplace(winKey(w), r);
    BufRec &rec = it->second;
    rec.biased = biasedAddr(rec);
    byBiased[rec.biased] = &rec;
    setPortBaseAddr(w, rec.biased, rec.rowBytes);

    WinSlot &s = table.wins[slot];
    memset(&s, 0, sizeof(s));
    s.hwnd = winKey(w);
    publishGeometry(rec);
    markFullDirty(rec);
    pcRootlessPublish();
}

void Executor::pcRootlessWindowDisposed(WindowPeek w)
{
    auto it = byWin.find(winKey(w));
    if(it == byWin.end())
        return;
    BufRec &r = it->second;

    /* Point the dying port back at the screen so any late draws scribble
     * the (invisible) screen instead of freed memory. */
    PixMapHandle screenPM = GD_PMAP(LM(MainDevice));
    setPortBaseAddr(w, (uint32_t)(uintptr_t)(char *)PIXMAP_BASEADDR(screenPM),
                    PIXMAP_ROWBYTES(screenPM) & 0x3FFF);

    table.wins[r.slot].hwnd = 0;
    byBiased.erase(r.biased);
    free(r.bytes);
    byWin.erase(it);
    pcRootlessPublish();
}

void Executor::pcRootlessWindowMoved(WindowPeek w)
{
    auto it = byWin.find(winKey(w));
    if(it == byWin.end())
        return;
    BufRec &r = it->second;

    int gx, gy, cw, ch;
    contentGeometry(w, &gx, &gy, &cw, &ch);
    if(gx == r.gx && gy == r.gy)
        return;

    r.gx = gx;
    r.gy = gy;
    r.sx = gx - r.fl;
    r.sy = gy - r.ft;
    rebias(r, w);
    publishGeometry(r);
    pcRootlessPublish();
}

void Executor::pcRootlessWindowResized(WindowPeek w)
{
    auto it = byWin.find(winKey(w));
    if(it == byWin.end())
        return;
    BufRec &r = it->second;
    /* Only track the new content metrics; the buffer itself grows at the
     * wCalcRgns that inevitably follows (pcRootlessSyncFrame), once the
     * WDEF has computed the new struct region. Until then drawing is
     * clipped to the stale (smaller) visRgn, so the old buffer is safe. */
    contentGeometry(w, &r.gx, &r.gy, &r.cw, &r.ch);
    publishGeometry(r);
    pcRootlessPublish();
}

void Executor::pcRootlessSyncFrame(WindowPeek w)
{
    auto it = byWin.find(winKey(w));
    if(it == byWin.end())
        return;
    BufRec &r = it->second;

    RgnHandle strucRgn = WINDOW_STRUCT_REGION(w);
    if(!strucRgn || !*strucRgn)
        return;
    int nsx = (*strucRgn)->rgnBBox.left.get();
    int nsy = (*strucRgn)->rgnBBox.top.get();
    int nsw = (*strucRgn)->rgnBBox.right.get() - nsx;
    int nsh = (*strucRgn)->rgnBBox.bottom.get() - nsy;
    if(nsw <= 0 || nsh <= 0)
        return;

    contentGeometry(w, &r.gx, &r.gy, &r.cw, &r.ch);

    /* The struct region must contain the content; guard against WDEFs
     * that briefly report otherwise. */
    nsx = std::min(nsx, r.gx);
    nsy = std::min(nsy, r.gy);
    nsw = std::max(nsw, r.gx + r.cw - nsx);
    nsh = std::max(nsh, r.gy + r.ch - nsy);

    if(nsx == r.sx && nsy == r.sy && nsw == r.sw && nsh == r.sh)
    {
        r.fl = r.gx - r.sx;
        r.ft = r.gy - r.sy;
        return;
    }

    int newRowBytes = nsw * 4;
    uint8_t *nb = (uint8_t *)malloc((size_t)newRowBytes * nsh);
    if(!nb)
        return;
    memset(nb, 0xFF, (size_t)newRowBytes * nsh);

    /* Preserve the overlapping CONTENT pixels across the realloc. */
    {
        int copyW = std::min(r.cw, nsw - (r.gx - nsx));
        int copyH = std::min(r.ch, nsh - (r.gy - nsy));
        int oldX = r.gx - r.sx, oldY = r.gy - r.sy;
        copyW = std::min(copyW, r.sw - oldX);
        copyH = std::min(copyH, r.sh - oldY);
        if(copyW > 0 && copyH > 0 && oldX >= 0 && oldY >= 0)
        {
            for(int y = 0; y < copyH; y++)
                memcpy(nb + (size_t)(r.gy - nsy + y) * newRowBytes
                           + (size_t)(r.gx - nsx) * 4,
                       r.bytes + (size_t)(oldY + y) * r.rowBytes
                           + (size_t)oldX * 4,
                       (size_t)copyW * 4);
        }
    }

    byBiased.erase(r.biased);
    free(r.bytes);
    r.bytes = nb;
    r.sx = nsx;
    r.sy = nsy;
    r.sw = nsw;
    r.sh = nsh;
    r.rowBytes = newRowBytes;
    r.fl = r.gx - r.sx;
    r.ft = r.gy - r.sy;
    r.biased = biasedAddr(r);
    byBiased[r.biased] = &r;
    setPortBaseAddr(w, r.biased, r.rowBytes);

    publishGeometry(r);
    markFullDirty(r);
    pcRootlessPublish();
}

void Executor::pcRootlessPublish()
{
    if(!pcRootlessEnabled())
        return;

    uint32_t seq = table.seq;
    atomicStore(&table.seq, seq + 1); /* odd: writing */

    table.screenW = qdGlobals().screenBits.bounds.right
        - qdGlobals().screenBits.bounds.left;
    table.screenH = qdGlobals().screenBits.bounds.bottom
        - qdGlobals().screenBits.bounds.top;
    table.bpp = PIXMAP_PIXEL_SIZE(GD_PMAP(LM(MainDevice)));
    table.mbarHeight = LM(MBarHeight);
    if(vdriver)
    {
        table.screenBuf = (uint32_t)(uintptr_t)vdriver->framebuffer();
        table.screenRowBytes = (uint32_t)vdriver->rowBytes();
    }
    uint32_t mcount = (uint32_t)std::min<size_t>(openMenus.size(), 8);
    for(uint32_t i = 0; i < mcount; i++)
    {
        table.menus[i].l = openMenus[i].left;
        table.menus[i].t = openMenus[i].top;
        table.menus[i].r = openMenus[i].right;
        table.menus[i].b = openMenus[i].bottom;
    }
    table.menuCount = mcount;

    WindowPtr front = FrontWindow();
    table.frontHwnd = front ? winKey((WindowPeek)front) : 0;

    int z = 0;
    uint32_t count = 0;
    std::vector<uint32_t> seen;
    seen.reserve(byWin.size());
    for(WindowPeek wp = LM(WindowList); wp; wp = WINDOW_NEXT_WINDOW(wp))
    {
        auto it = byWin.find(winKey(wp));
        if(it == byWin.end())
            continue;
        seen.push_back(winKey(wp));
        WinSlot &s = table.wins[it->second.slot];
        s.zorder = z++;
        s.flags = (WINDOW_VISIBLE(wp) ? 1 : 0) | (WINDOW_HILITED(wp) ? 2 : 0)
            | (shellStripP(it->second, wp) ? 4 : 0);
        publishGeometry(it->second);
        count++;

        StringHandle th = WINDOW_TITLE(wp);
        if(th && *th)
        {
            uint8_t len = (uint8_t)std::min<int>((*th)[0], TITLE_BYTES - 1);
            if(len != s.title[0] || memcmp(s.title + 1, (char *)*th + 1, len) != 0)
            {
                s.title[0] = len;
                memcpy(s.title + 1, (char *)*th + 1, len);
                s.titleSeq++;
            }
        }
    }
    table.count = count;

    /* Reconcile: registry entries whose window is no longer in the list
     * were ABANDONED (InitWindows at app launch), not closed — free their
     * buffers and clear their slots so the host drops the ghost windows. */
    if(seen.size() != byWin.size())
    {
        std::vector<uint32_t> dead;
        for(const auto &kv : byWin)
            if(std::find(seen.begin(), seen.end(), kv.first) == seen.end())
                dead.push_back(kv.first);
        for(uint32_t k : dead)
            dropRecordNoPort(k);
    }

    atomicStore(&table.seq, seq + 2); /* even: stable */
}

bool Executor::pcRootlessIsWinBuf(uint32_t baseAddr)
{
    if(byBiased.empty())
        return false;
    return byBiased.find(baseAddr) != byBiased.end();
}

bool Executor::pcRootlessNoteDirty(uint32_t baseAddr, int top, int left,
                                   int bottom, int right)
{
    if(byBiased.empty())
        return false;
    auto it = byBiased.find(baseAddr);
    if(it == byBiased.end())
        return false;
    BufRec &r = *it->second;
    WinSlot &s = table.wins[r.slot];

    /* The accrue sites compute rect − bounds.topLeft, which for every
     * biased bitmap is GLOBAL coords; translate to buffer coords. */
    top -= r.sy;
    bottom -= r.sy;
    left -= r.sx;
    right -= r.sx;

    top = std::max(top, 0);
    left = std::max(left, 0);
    bottom = std::min(bottom, r.sh);
    right = std::min(right, r.sw);
    if(top >= bottom || left >= right)
        return true;

    uint32_t seq = atomicLoad(&s.dirtySeq);
    uint32_t ack = atomicLoad(&s.dirtyAck);
    if(ack == seq)
    {
        s.dl = left;
        s.dt = top;
        s.dr = right;
        s.db = bottom;
    }
    else
    {
        s.dl = std::min(s.dl, left);
        s.dt = std::min(s.dt, top);
        s.dr = std::max(s.dr, right);
        s.db = std::max(s.db, bottom);
    }
    atomicStore(&s.dirtySeq, seq + 1);
    return true;
}

/* ── PcFrameRedirect ─────────────────────────────────────────────────── */

PcFrameRedirect::PcFrameRedirect(WindowPeek w)
{
    if(!pcRootlessEnabled() || !w)
        return;
    if(qdGlobals().thePort != wmgr_port)
        return;
    auto it = byWin.find(winKey(w));
    if(it == byWin.end())
        return;
    BufRec &r = it->second;

    PixMapHandle pm = CPORT_PIXMAP_X_NO_ASSERT((CGrafPtr)LM(WMgrCPort));
    savedBase_ = (void *)(uintptr_t)(uint32_t)(uintptr_t)(char *)PIXMAP_BASEADDR(pm);
    savedRowBytes_ = (*pm)->rowBytes.get();
    PIXMAP_BASEADDR(pm) = (Ptr)(uintptr_t)r.biased;
    (*pm)->rowBytes = (int16_t)(r.rowBytes | (savedRowBytes_ & 0xC000));

    /* Safety: never let a frame draw escape the buffer — intersect the
     * wmgr clip with the struct region (both in global coords). */
    savedClip_ = NewRgn();
    CopyRgn(PORT_CLIP_REGION(wmgr_port), savedClip_);
    RgnHandle strucRgn = WINDOW_STRUCT_REGION(w);
    if(strucRgn && *strucRgn)
        SectRgn(PORT_CLIP_REGION(wmgr_port), strucRgn,
                PORT_CLIP_REGION(wmgr_port));

    active_ = true;
}

PcFrameRedirect::~PcFrameRedirect()
{
    if(!active_)
        return;
    PixMapHandle pm = CPORT_PIXMAP_X_NO_ASSERT((CGrafPtr)LM(WMgrCPort));
    PIXMAP_BASEADDR(pm) = (Ptr)(uintptr_t)savedBase_;
    (*pm)->rowBytes = savedRowBytes_;
    if(savedClip_)
    {
        CopyRgn(savedClip_, PORT_CLIP_REGION(wmgr_port));
        DisposeRgn(savedClip_);
    }
}

void Executor::pcRootlessMenuOpen(Rect r)
{
    if(!pcRootlessEnabled())
        return;
    /* The Menu Manager reports the INTERIOR rect; the frame and drop
     * shadow draw just outside it (same −1/+2 inflation upstream's
     * wayland rootless applies). Clamp to the screen so the host blit
     * never reads outside the framebuffer. */
    r.left = std::max(0, r.left - 1);
    r.top = std::max(0, r.top - 1);
    r.right = std::min((int)table.screenW, r.right + 2);
    r.bottom = std::min((int)table.screenH, r.bottom + 2);
    openMenus.push_back(r);
    pcRootlessPublish();
}

void Executor::pcRootlessMenuClose()
{
    if(!pcRootlessEnabled())
        return;
    if(!openMenus.empty())
        openMenus.pop_back();
    pcRootlessPublish();
}

void Executor::pcRootlessDrainInput(IEventListener *listener)
{
    if(!pcRootlessEnabled() || !listener)
        return;
    uint32_t head = atomicLoad(&ring.head);
    uint32_t tail = ring.tail;
    while(tail != head)
    {
        const InputRec &rec = ring.recs[tail % RING_CAP];
        switch(rec.type)
        {
            case 0:
                listener->mouseMoved(rec.x, rec.y);
                break;
            case 5:
                pcRootlessRequestScreenSize(rec.x, rec.y);
                break;
            case 6:
                __atomic_store_n(&pendingRaise, (uint32_t)rec.aux,
                                 __ATOMIC_RELEASE);
                break;
            /* Host app focus ↔ MultiFinder suspend/resume. Rootless has
             * no SDL window that receives FOCUS_LOST/GAINED (input rides
             * the ring; the SDL canvas is a worker stub), so the page
             * pushes these when the Executor group loses/gains .active —
             * same callbacks the stock SDL/Wayland front-ends use. */
            case 7:
                listener->suspendEvent();
                break;
            case 8:
                listener->resumeEvent(/*updateClipboard=*/true);
                break;
            case 1:
                listener->mouseButtonEvent(true, rec.x, rec.y);
                break;
            case 2:
                listener->mouseButtonEvent(false, rec.x, rec.y);
                break;
            case 3:
                listener->keyboardEvent(true, (unsigned char)rec.aux);
                break;
            case 4:
                listener->keyboardEvent(false, (unsigned char)rec.aux);
                break;
        }
        tail++;
    }
    atomicStore(&ring.tail, tail);
}

void Executor::pcRootlessSetCursor(const char *data, const uint16_t mask[16],
                                   int hotX, int hotY)
{
    if(!pcRootlessEnabled())
        return;
    const uint8_t *d = (const uint8_t *)data;
    const uint8_t *m = (const uint8_t *)mask;
    uint32_t *dst = table.cursorPixels;
    for(int y = 0; y < 16; y++)
        for(int x = 0; x < 16; x++)
        {
            bool dbit = d[2 * y + x / 8] & (0x80 >> (x % 8));
            bool mbit = m[2 * y + x / 8] & (0x80 >> (x % 8));
            /* same mapping as the wayland front end: masked → black/white,
             * unmasked+data → black (XOR approximated), else transparent */
            if(mbit)
                *dst++ = dbit ? 0xFF000000u : 0xFFFFFFFFu;
            else
                *dst++ = dbit ? 0xFF000000u : 0x00000000u;
        }
    table.cursorHotX = hotX;
    table.cursorHotY = hotY;
    uint32_t seq = atomicLoad(&table.cursorSeq);
    atomicStore(&table.cursorSeq, seq + 1);
}

void Executor::pcRootlessSetCursorVisible(bool visible)
{
    if(!pcRootlessEnabled())
        return;
    atomicStore(&table.cursorVisible, visible ? 1 : 0);
    uint32_t seq = atomicLoad(&table.cursorSeq);
    atomicStore(&table.cursorSeq, seq + 1);
}

void Executor::pcRootlessHandleRaise()
{
    if(!pcRootlessEnabled())
        return;
    uint32_t h = __atomic_exchange_n(&pendingRaise, 0, __ATOMIC_ACQ_REL);
    if(!h || !byWin.count(h))
        return;
    WindowPtr w = (WindowPtr)(uintptr_t)h;
    if(w == FrontWindow())
        return;
    /* Don't yank a (likely modal) dialog out from under ModalDialog's
     * loop: classic modality assumes the dialog stays frontmost. The
     * click still flows; the guest's own hit-testing applies. */
    WindowPeek front = (WindowPeek)FrontWindow();
    if(front && WINDOW_KIND(front) == dialogKind && !WINDOW_GO_AWAY_FLAG(front))
        return;
    SelectWindow(w);
}

void Executor::pcRootlessRequestScreenSize(int w, int h)
{
    if(w < 512 || h < 342 || w > 8191 || h > 8191)
        return;
    __atomic_store_n(&pendingScreenSize, ((uint32_t)w << 16) | (uint32_t)h,
                     __ATOMIC_RELEASE);
}

bool Executor::pcRootlessTakeScreenSizeRequest(int *w, int *h)
{
    uint32_t v = __atomic_exchange_n(&pendingScreenSize, 0, __ATOMIC_ACQ_REL);
    if(!v)
        return false;
    *w = (int)(v >> 16);
    *h = (int)(v & 0xFFFF);
    return true;
}

extern "C" {
PC_EXPORT uint32_t pc_rootless_table(void)
{
    return (uint32_t)(uintptr_t)&table;
}
PC_EXPORT uint32_t pc_rootless_ring(void)
{
    return (uint32_t)(uintptr_t)&ring;
}
}
