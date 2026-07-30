/* Platinum document window chrome — direct port of
 * appearence/appearance/wdefs/wdef64_document.py (WDEF 64 Silver).
 *
 * Paint order and geometry match render_document_window / draw_frame /
 * draw_titlebar_stripes / draw_widget_glyph / draw_grow_box. Structure
 * region expands so the ~6px chrome lives OUTSIDE the content port
 * (otherwise apps erase it immediately — the bug behind "looks like Sys7").
 */
#include <base/common.h>
#include <QuickDraw.h>
#include <CQuickDraw.h>
#include <WindowMgr.h>
#include <FontMgr.h>
#include <MemoryMgr.h>
#include <rsys/appearance_mgr.h>
#include <quickdraw/cquick.h>
#include <wind/wind.h>

#include "theme_data.h"
#include "theme_glyphs_data.inc"

using namespace Executor;

namespace
{
/* Insets: structure ↔ content. From appearence cross-section:
 *   x=0 black, 1 white, 2-3 #CC, 4 #99, 5 black, 6 content…
 *   top: 19px titlebar + 3px top chrome before content. */
constexpr int kInsetLeft = 6;
constexpr int kInsetTop = 22; /* TITLEBAR_HEIGHT + 3 */
constexpr int kInsetRight = 7;
constexpr int kInsetBottom = 7;

void window_content(WindowPeek w, int *left, int *top, int *right, int *bottom)
{
    *left = PORT_RECT(w).left - PORT_BOUNDS(w).left;
    *top = PORT_RECT(w).top - PORT_BOUNDS(w).top;
    *right = PORT_RECT(w).right - PORT_BOUNDS(w).left;
    *bottom = PORT_RECT(w).bottom - PORT_BOUNDS(w).top;
}

void structure_from_content(int left, int top, int right, int bottom,
                            int *sl, int *st, int *sr, int *sb)
{
    *sl = left - kInsetLeft;
    *st = top - kInsetTop;
    *sr = right + kInsetRight;
    *sb = bottom + kInsetBottom;
}

/* Appearence GLYPH_CLUT_INACTIVE — used for active/unpressed widgets. */
void make_inactive_glyph_clut(RGBColor out[16])
{
    static const uint8_t kInactive[9][3] = {
        { 0xFF, 0xFF, 0xFF }, { 0xCC, 0xCC, 0xCC }, { 0x99, 0x99, 0x99 },
        { 0xEE, 0xEE, 0xEE }, { 0xDD, 0xDD, 0xDD }, { 0xBB, 0xBB, 0xBB },
        { 0xAA, 0xAA, 0xAA }, { 0x88, 0x88, 0x88 }, { 0x22, 0x22, 0x22 },
    };
    for(int i = 0; i < 16; ++i)
        platinum_rgb(0, 0, 0, &out[i]);
    for(int i = 0; i < 9; ++i)
        platinum_rgb(kInactive[i][0], kInactive[i][1], kInactive[i][2], &out[i]);
}

void blit_glyph_4bpp(INTEGER x, INTEGER y, const uint8_t *glyph,
                     const RGBColor *clut, int max_idx)
{
    for(INTEGER row = 0; row < platinum::kWdefGlyphH; ++row)
    {
        const uint8_t *rowp = glyph + row * platinum::kWdefGlyphRowBytes;
        for(INTEGER col = 0; col < platinum::kWdefGlyphW; ++col)
        {
            uint8_t byte = rowp[col >> 1];
            uint8_t nibble = (col & 1) ? (byte & 0x0F) : ((byte >> 4) & 0x0F);
            if(nibble > max_idx)
                continue;
            RGBColor c = clut[nibble];
            RGBForeColor(&c);
            Rect px;
            SetRect(&px, x + col, y + row, x + col + 1, y + row + 1);
            PaintRect(&px);
        }
    }
}

/* draw_frame — appearence wdef64_document.draw_frame (active path).
 * ox/oy = structure origin; sw/sh = structure size.
 * Does NOT paint the content interior (Executor apps own that). */
void draw_frame(int ox, int oy, int sw, int sh, bool active)
{
    const int ct = platinum::kTitlebarHeight; /* 19 */

    if(!active)
    {
        /* Inactive — appearence draw_frame(active=False): #DD chrome ring
         * + black FrameRect. Content hole left to the app (Python fills
         * white at (2,ct+1); we skip so we don't erase guest pixels).
         * Structure still uses active insets, so the ring covers the full
         * chrome bands (left 1..6, etc.). */
        Rect band;
        SetRect(&band, ox + 1, oy + 1, ox + sw - 1, oy + ct + 1); /* titlebar */
        platinum_paint_rect(&band, 0xDD, 0xDD, 0xDD);
        SetRect(&band, ox + 1, oy + ct, ox + 6, oy + sh - 1); /* left */
        platinum_paint_rect(&band, 0xDD, 0xDD, 0xDD);
        SetRect(&band, ox + sw - 7, oy + ct, ox + sw - 1, oy + sh - 1); /* right */
        platinum_paint_rect(&band, 0xDD, 0xDD, 0xDD);
        SetRect(&band, ox + 1, oy + sh - 7, ox + sw - 1, oy + sh - 1); /* bottom */
        platinum_paint_rect(&band, 0xDD, 0xDD, 0xDD);

        Rect outer;
        SetRect(&outer, ox, oy, ox + sw, oy + sh);
        platinum_frame_rect(&outer, 0, 0, 0);
        return;
    }

    uint8_t fr, hl, shc, g, b;
    platinum::role_rgb(0, &fr, &g, &b); /* #CC */
    platinum::role_rgb(3, &hl, &g, &b); /* white */
    platinum::role_rgb(4, &shc, &g, &b); /* #99 */

    /* Chrome ring only — hole is content (6,22)..(sw-7,sh-7) in struct coords. */
    Rect band;
    /* under titlebar / top chrome */
    SetRect(&band, ox + 1, oy + ct, ox + sw - 1, oy + 22);
    platinum_paint_rect(&band, fr, fr, fr);
    /* left */
    SetRect(&band, ox + 1, oy + ct, ox + 6, oy + sh - 1);
    platinum_paint_rect(&band, fr, fr, fr);
    /* right */
    SetRect(&band, ox + sw - 7, oy + ct, ox + sw - 1, oy + sh - 1);
    platinum_paint_rect(&band, fr, fr, fr);
    /* bottom */
    SetRect(&band, ox + 1, oy + sh - 7, ox + sw - 1, oy + sh - 1);
    platinum_paint_rect(&band, fr, fr, fr);

    /* Outer bevels — exact Python LineTo endpoints */
    platinum_vline(ox + 1, oy + ct, oy + sh - 4, hl, hl, hl);
    platinum_vline(ox + sw - 2, oy + ct, oy + sh - 2, 0, 0, 0);
    platinum_vline(ox + sw - 3, oy + ct, oy + sh - 2, shc, shc, shc);
    platinum_hline(oy + sh - 2, ox + 1, ox + sw - 2, 0, 0, 0);
    platinum_hline(oy + sh - 3, ox + 2, ox + sw - 3, shc, shc, shc);

    /* Inner groove */
    platinum_vline(ox + 4, oy + ct + 1, oy + sh - 7, shc, shc, shc);
    platinum_hline(oy + ct + 1, ox + 4, ox + sw - 7, shc, shc, shc);
    platinum_vline(ox + sw - 6, oy + ct + 2, oy + sh - 6, hl, hl, hl);
    platinum_hline(oy + sh - 6, ox + 5, ox + sw - 6, hl, hl, hl);

    /* Black content border */
    Rect content;
    SetRect(&content, ox + 5, oy + ct + 2, ox + sw - 6, oy + sh - 6);
    platinum_frame_rect(&content, 0, 0, 0);
}

/* draw_titlebar_stripes — appearence draw_titlebar_stripes */
void draw_titlebar_stripes(int x1, int y1, int x2, int y2,
                           int stripe_x1, int stripe_x2)
{
    uint8_t hl, hs, sl, sd, g, b;
    platinum::role_rgb(3, &hl, &g, &b);
    platinum::role_rgb(4, &hs, &g, &b);
    platinum::role_rgb(7, &sl, &g, &b);
    platinum::role_rgb(8, &sd, &g, &b);

    platinum_hline(y1 + 1, x1, x2 - 2, hl, hl, hl);
    platinum_vline(x1, y1 + 1, y2, hl, hl, hl);
    platinum_vline(x2, y1 + 1, y2, 0, 0, 0);
    platinum_vline(x2 - 1, y1 + 2, y2, hs, hs, hs);

    if(stripe_x2 - stripe_x1 < 8)
        return;

    int stripe_y = y1 + 4;
    for(int i = 0; i < 6; ++i)
    {
        int yl = stripe_y + i * 2;
        int yd = yl + 1;
        if(yd > y2 - 3)
            break;
        platinum_hline(yl, stripe_x1, stripe_x2, sl, sl, sl);
        platinum_hline(yd, stripe_x1 + 1, stripe_x2 + 1, sd, sd, sd);
    }
}

/* calc_widget_rects — appearence; returns global coords given structure origin. */
void widget_rects(int ox, int oy, int sw,
                  Rect *close_r, Rect *zoom_r, Rect *collapse_r)
{
    const int v_top = 4;
    SetRect(close_r, ox + 4, oy + v_top, ox + 17, oy + v_top + 13);
    int collapse_left = ox + sw - 18;
    SetRect(collapse_r, collapse_left, oy + v_top, collapse_left + 13,
            oy + v_top + 13);
    int zoom_left = collapse_left - 16;
    SetRect(zoom_r, zoom_left, oy + v_top, zoom_left + 13, oy + v_top + 13);
}

void draw_widgets(int ox, int oy, int sw, bool active)
{
    Rect close_r, zoom_r, collapse_r;
    widget_rects(ox, oy, sw, &close_r, &zoom_r, &collapse_r);

    if(active)
    {
        /* Python quirk: active/unpressed → inactive 4bpp glyphs + INACTIVE CLUT */
        RGBColor clut[16];
        make_inactive_glyph_clut(clut);
        const uint8_t *base = platinum::kWdef64GlyphInactive;
        auto blit = [&](int idx, const Rect &r) {
            blit_glyph_4bpp(r.left, r.top, base + idx * platinum::kWdefGlyphBytes,
                            clut, 0x8);
        };
        blit(0, close_r);
        blit(1, zoom_r);
        blit(2, collapse_r);
    }
    else
    {
        /* Inactive window widgets — 1bpp dim glyphs (WDEF64 sub_2312 / Python).
         * Ink #99 on white face. */
        auto blit1 = [](int idx, const Rect &r) {
            platinum_paint_rect(&r, 0xFF, 0xFF, 0xFF);
            const uint8_t *g = platinum::kWdef64GlyphInactive1bppDim
                + idx * platinum::kWdef1bppDimBytes;
            for(INTEGER row = 0; row < platinum::kWdefGlyphH; ++row)
            {
                uint16_t word = (uint16_t)((g[row * 2] << 8) | g[row * 2 + 1]);
                for(INTEGER col = 0; col < platinum::kWdefGlyphW; ++col)
                {
                    if((word >> (15 - col)) & 1)
                    {
                        Rect px;
                        SetRect(&px, r.left + col, r.top + row,
                                r.left + col + 1, r.top + row + 1);
                        platinum_paint_rect(&px, 0x99, 0x99, 0x99);
                    }
                }
            }
        };
        blit1(0, close_r);
        blit1(1, zoom_r);
        blit1(2, collapse_r);
    }
}

void draw_title(WindowPeek w, int ox, int oy, int sw, bool active, uint8_t fill)
{
    StringHandle th = WINDOW_TITLE(w);
    if(!th || !WINDOW_TITLE_WIDTH(w))
        return;

    INTEGER title_width = WINDOW_TITLE_WIDTH(w);
    /* Python centers in full window width; Executor title_width is precomputed. */
    INTEGER tx = ox + (sw - title_width) / 2;
    if(tx < ox + 28)
        tx = ox + 28;

    const int y1 = oy;
    const int y2 = oy + platinum::kTitlebarHeight - 1;

    Rect erase;
    SetRect(&erase, tx - 4, y1 + 3, tx + title_width + 4, y2 - 1);
    platinum_paint_rect(&erase, fill, fill, fill);

    TextFont(0);
    TextSize(12);
    TextFace(0);
    TextMode(srcOr);

    /* Baseline roughly matching Python ty for 12pt in 19px bar. */
    INTEGER baseline = oy + 14;

    HLockGuard guard(th);
    if(active)
    {
        /* TEXT_SHADOW = #EEEEEE then black (role 2) — appearence draw_title_text */
        platinum_fore(0xEE, 0xEE, 0xEE);
        MoveTo(tx + 1, baseline + 1);
        DrawString(*th);
        platinum_fore(0, 0, 0);
        MoveTo(tx, baseline);
        DrawString(*th);
    }
    else
    {
        platinum_fore(0xEE, 0xEE, 0xEE);
        MoveTo(tx + 1, baseline + 1);
        DrawString(*th);
        platinum_fore(0x99, 0x99, 0x99); /* INACTIVE_DARK */
        MoveTo(tx, baseline);
        DrawString(*th);
    }
}

/* draw_grow_box — appearence draw_grow_box, structure-relative. */
void draw_grow_box(int ox, int oy, int sw, int sh)
{
    uint8_t fr, sd, g, b;
    platinum::role_rgb(0, &fr, &g, &b);
    platinum::role_rgb(8, &sd, &g, &b); /* #77 */

    int sep_x = ox + sw - 22;
    int sep_y = oy + sh - 22;
    int cb_left = ox + 5;
    int cb_right = ox + sw - 7;
    int cb_bottom = oy + sh - 7;
    int fill_right = ox + sw - 4;
    int fill_bottom = oy + sh - 4;

    Rect gb;
    SetRect(&gb, sep_x, sep_y, fill_right + 1, fill_bottom + 1);
    platinum_paint_rect(&gb, fr, fr, fr);

    platinum_hline(sep_y, cb_left, cb_right, 0, 0, 0);
    platinum_vline(sep_x, sep_y, cb_bottom, 0, 0, 0);

    int ix1 = sep_x + 1;
    int iy1 = sep_y + 1;
    platinum_vline(ix1, iy1, cb_bottom, 0xFF, 0xFF, 0xFF);
    platinum_hline(iy1, ix1, cb_right, 0xFF, 0xFF, 0xFF);

    platinum_vline(ox + sw - 6, sep_y, iy1, 0xFF, 0xFF, 0xFF);
    platinum_hline(oy + sh - 6, sep_x, ix1, 0xFF, 0xFF, 0xFF);

    int oxp = ix1 + 1;
    int oyp = iy1 + 1;
    for(int i = 0; i < 3; ++i)
    {
        int hx = oxp + 7 + i * 2;
        int hy = oyp + 2 + i * 2;
        for(int step = 0; step < 6; ++step)
        {
            platinum_fore(0xFF, 0xFF, 0xFF);
            MoveTo(hx - step, hy + step);
            LineTo(hx - step, hy + step);
        }
        platinum_fore(0xFF, 0xFF, 0xFF);
        MoveTo(hx + 1, hy);
        LineTo(hx + 1, hy);

        int sx = oxp + 8 + i * 2;
        int sy = oyp + 3 + i * 2;
        for(int step = 0; step < 6; ++step)
        {
            platinum_fore(sd, sd, sd);
            MoveTo(sx - step, sy + step);
            LineTo(sx - step, sy + step);
        }
        /* TERMINATOR_GRAY #AA */
        platinum_fore(0xAA, 0xAA, 0xAA);
        MoveTo(oxp + 2 + i * 2, oyp + 8 + i * 2);
        LineTo(oxp + 2 + i * 2, oyp + 8 + i * 2);
    }
}

void calc_rgns_platinum(WindowPeek w)
{
    int left, top, right, bottom, sl, st, sr, sb;
    window_content(w, &left, &top, &right, &bottom);
    structure_from_content(left, top, right, bottom, &sl, &st, &sr, &sb);

    SetRectRgn(WINDOW_CONT_REGION(w), left, top, right, bottom);
    SetRectRgn(WINDOW_STRUCT_REGION(w), sl, st, sr, sb);
}

LONGINT hit_platinum(WindowPeek w, LONGINT parm, bool growable)
{
    Point p;
    p.v = (INTEGER)((uint32_t)parm >> 16);
    p.h = (INTEGER)((uint32_t)parm & 0xFFFF);

    int left, top, right, bottom, sl, st, sr, sb;
    window_content(w, &left, &top, &right, &bottom);
    structure_from_content(left, top, right, bottom, &sl, &st, &sr, &sb);
    int sw = sr - sl;

    if(PtInRgn(p, WINDOW_CONT_REGION(w)))
    {
        if(growable && p.h >= right - 16 && p.v >= bottom - 16)
            return wInGrow;
        return wInContent;
    }

    /* Titlebar: structure top .. content top (includes 3px top chrome). */
    if(p.h >= sl && p.h < sr && p.v >= st && p.v < top)
    {
        if(!WINDOW_HILITED(w))
            return wInDrag;

        Rect close_r, zoom_r, collapse_r;
        widget_rects(sl, st, sw, &close_r, &zoom_r, &collapse_r);

        if(WINDOW_GO_AWAY_FLAG(w) && PtInRect(p, &close_r))
            return wInGoAway;
        if(WINDOW_SPARE_FLAG(w) && PtInRect(p, &zoom_r))
            return ROMlib_window_zoomed(w) ? wInZoomIn : wInZoomOut;
        /* Collapse: draw + hit, click may no-op (Executor has no shade). */
        if(PtInRect(p, &collapse_r))
            return wInDrag;
        return wInDrag;
    }

    /* Grow chrome outside content (bottom-right structure corner). */
    if(growable && WINDOW_HILITED(w)
       && p.h >= sr - 22 && p.h < sr && p.v >= sb - 22 && p.v < sb)
        return wInGrow;

    return wNoHit;
}

void draw_document(WindowPeek w, INTEGER varcode, LONGINT parm)
{
    if(!WINDOW_VISIBLE(w))
        return;

    /* Part toggles fall through to Sys7 for now. */
    if(parm != 0)
        return;

    /* Ensure structure/content match Platinum insets before painting.
     * Rootless clips to strucRgn — paint outside it shows desktop. */
    calc_rgns_platinum(w);

    int left, top, right, bottom, sl, st, sr, sb;
    window_content(w, &left, &top, &right, &bottom);
    structure_from_content(left, top, right, bottom, &sl, &st, &sr, &sb);

    /* Prefer the live structure bbox so paint origin matches the clip. */
    {
        RgnHandle srgn = WINDOW_STRUCT_REGION(w);
        if(srgn && *srgn)
        {
            const Rect &bb = (*srgn)->rgnBBox;
            sl = bb.left;
            st = bb.top;
            sr = bb.right;
            sb = bb.bottom;
        }
    }
    int sw = sr - sl;
    int sh = sb - st;
    if(sw < 40 || sh < 30)
        return;

    bool active = WINDOW_HILITED(w);
    bool growable = (varcode == documentProc);
    uint8_t fr, g, b;
    platinum::role_rgb(0, &fr, &g, &b);

    PenNormal();

    /* Phase 1: frame chrome (Python draw_frame) — not content fill */
    draw_frame(sl, st, sw, sh, active);

    /* Phase 2: titlebar background — interior only (1 .. sw-2), keep outer black */
    Rect tbr;
    SetRect(&tbr, sl + 1, st + 1, sr - 1, st + platinum::kTitlebarHeight - 1);
    if(active)
        platinum_paint_rect(&tbr, fr, fr, fr);
    else
        platinum_paint_rect(&tbr, 0xDD, 0xDD, 0xDD);

    /* Phase 3: stripes between close.right+4 and zoom.left-6 */
    Rect close_r, zoom_r, collapse_r;
    widget_rects(sl, st, sw, &close_r, &zoom_r, &collapse_r);
    if(active)
    {
        int stripe_left = close_r.right + 4;
        int stripe_right = zoom_r.left - 6;
        draw_titlebar_stripes(sl + 1, st, sr - 2,
                              st + platinum::kTitlebarHeight - 1,
                              stripe_left, stripe_right);
    }

    /* Phase 4: title */
    draw_title(w, sl, st, sw, active, active ? fr : 0xDD);

    /* Phase 5: widgets */
    draw_widgets(sl, st, sw, active);

    /* Phase 6: grow box */
    if(active && growable)
        draw_grow_box(sl, st, sw, sh);

    /* Phase 7: final outer edges via PaintRect (more reliable than FrameRect/LineTo) */
    Rect edge;
    SetRect(&edge, sl, st, sr, st + 1); /* top black */
    platinum_paint_rect(&edge, 0, 0, 0);
    SetRect(&edge, sl, sb - 1, sr, sb); /* bottom black */
    platinum_paint_rect(&edge, 0, 0, 0);
    SetRect(&edge, sl, st, sl + 1, sb); /* left black */
    platinum_paint_rect(&edge, 0, 0, 0);
    SetRect(&edge, sr - 1, st, sr, sb); /* right black */
    platinum_paint_rect(&edge, 0, 0, 0);
    if(active)
    {
        /* Top / left white highlights (appearence stripe bevels) */
        SetRect(&edge, sl + 1, st + 1, sr - 2, st + 2);
        platinum_paint_rect(&edge, 0xFF, 0xFF, 0xFF);
        SetRect(&edge, sl + 1, st + 1, sl + 2, st + platinum::kTitlebarHeight - 1);
        platinum_paint_rect(&edge, 0xFF, 0xFF, 0xFF);
    }

    (void)collapse_r;
}
} /* namespace */

void Executor::platinum_draw_document_window(WindowPeek w, INTEGER varcode,
                                             LONGINT parm)
{
    draw_document(w, varcode, parm);
}

LONGINT Executor::C_wdef_platinum(INTEGER varcode, WindowPtr window,
                                  INTEGER message, LONGINT parm)
{
    WindowPeek w = (WindowPeek)window;

    int zoom_bit = varcode & ZOOMBIT;
    varcode &= ~ZOOMBIT;
    if(varcode > 4 && varcode != 5)
        varcode &= 3;

    bool doc_family = (varcode == documentProc || varcode == noGrowDocProc
                       || varcode == movableDBoxProc);

    switch(message)
    {
        case wCalcRgns:
            if(!doc_family)
                return -1;
            calc_rgns_platinum(w);
            return 0;

        case wDraw:
            if(!doc_family)
                return -1;
            if(parm != 0)
                return -1; /* toggles → Sys7 for now */
            draw_document(w, varcode, parm);
            return 0;

        case wHit:
            if(!doc_family)
                return -1;
            return hit_platinum(w, parm, varcode == documentProc);

        case wDrawGIcon:
            if(varcode == documentProc && WINDOW_VISIBLE(w) && WINDOW_HILITED(w))
            {
                int left, top, right, bottom, sl, st, sr, sb;
                window_content(w, &left, &top, &right, &bottom);
                structure_from_content(left, top, right, bottom, &sl, &st, &sr,
                                       &sb);
                PenNormal();
                draw_grow_box(sl, st, sr - sl, sb - st);
                return 0;
            }
            return -1;

        case wNew:
        case wDispose:
        case wGrow:
            /* Keep Sys7 zoom-state / grow-outline behavior. */
            return -1;

        default:
            return -1;
    }
    (void)zoom_bit;
}
