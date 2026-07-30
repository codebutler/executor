/* Platinum stock controls — direct port of
 * appearence/appearance/cdefs/cdef23_buttons.py and cdef24_scrollbar.py.
 */
#include <base/common.h>
#include <QuickDraw.h>
#include <CQuickDraw.h>
#include <ControlMgr.h>
#include <FontMgr.h>
#include <MenuMgr.h>

#include <rsys/appearance_mgr.h>
#include <quickdraw/cquick.h>
#include <ctl/ctl.h>

#include <algorithm>

#include "theme_data.h"
#include "theme_glyphs_data.inc"

using namespace Executor;

namespace
{
/* Save/restore the port's fore+back color and pen across a CDEF draw.
 * The Platinum draw routines set RGBBackColor (EraseRoundRect fills) and
 * RGBForeColor freely; the Dialog Manager draws statText items with
 * TETextBox AFTER DrawControls, and TETextBox erases each item rect with
 * the port's CURRENT back color. Leaking a control fill (e.g. the #DD
 * button/edit grey) left a grey box behind every dialog label. Classic
 * CDEFs are entered with the port colors the Dialog Manager expects; keep
 * that contract by restoring on exit. */
struct PortColorGuard
{
    RGBColor fg_, bk_;
    PenState pen_;
    PortColorGuard()
    {
        GetForeColor(&fg_);
        GetBackColor(&bk_);
        GetPenState(&pen_);
    }
    ~PortColorGuard()
    {
        RGBForeColor(&fg_);
        RGBBackColor(&bk_);
        SetPenState(&pen_);
    }
};

void clut_from(const uint8_t src[][3], RGBColor out[16])
{
    for(int i = 0; i < 16; ++i)
        platinum_rgb(src[i][0], src[i][1], src[i][2], &out[i]);
}

void pixel(INTEGER x, INTEGER y, uint8_t r, uint8_t g, uint8_t b)
{
    Rect px;
    SetRect(&px, x, y, x + 1, y + 1);
    platinum_paint_rect(&px, r, g, b);
}

/* Pen-tracking helper matching QuickDraw Move/Line used by CDEF 23 bevels. */
struct Pen
{
    INTEGER x, y;
    void moveTo(INTEGER h, INTEGER v) { x = h; y = v; }
    void move(INTEGER dh, INTEGER dv) { x += dh; y += dv; }
    void line(INTEGER dh, INTEGER dv, uint8_t r, uint8_t g, uint8_t b)
    {
        PenNormal();
        platinum_fore(r, g, b);
        MoveTo(x, y);
        LineTo(x + dh, y + dv);
        x += dh;
        y += dv;
    }
};

/* _draw_normal_bevel — cdef23_buttons.py / sub_1A30 */
void draw_normal_bevel(const Rect *rect)
{
    const INTEGER left = rect->left, top = rect->top;
    const INTEGER right = rect->right, bottom = rect->bottom;
    const INTEGER height = bottom - top;
    const INTEGER width = right - left;
    const uint8_t(*B)[3] = platinum::kBevelNormal;
    Pen p;

    p.moveTo(left + 2, bottom - 4);
    p.line(0, 6 - height, B[0][0], B[0][1], B[0][2]);
    p.move(1, 1);
    p.line(0, -1, B[0][0], B[0][1], B[0][2]);
    p.line(width - 7, 0, B[0][0], B[0][1], B[0][2]);

    p.move(2, 0);
    p.line(-1, -1, B[1][0], B[1][1], B[1][2]);
    p.move(5 - width, 0);
    p.line(-1, 1, B[1][0], B[1][1], B[1][2]);
    p.move(0, height - 5);
    p.line(1, 1, B[1][0], B[1][1], B[1][2]);

    p.move(1, 0);
    p.line(width - 6, 0, B[2][0], B[2][1], B[2][2]);
    p.move(0, -1);
    p.line(1, 0, B[2][0], B[2][1], B[2][2]);
    p.line(0, 6 - height, B[2][0], B[2][1], B[2][2]);

    p.move(-1, 0);
    p.line(0, height - 7, B[3][0], B[3][1], B[3][2]);
    p.move(-1, 0);
    p.line(0, 1, B[3][0], B[3][1], B[3][2]);
    p.line(7 - width, 0, B[3][0], B[3][1], B[3][2]);
}

/* _draw_pressed_bevel — cdef23_buttons.py / sub_1B58 */
void draw_pressed_bevel(const Rect *rect)
{
    const INTEGER left = rect->left, top = rect->top;
    const INTEGER right = rect->right, bottom = rect->bottom;
    const INTEGER height = bottom - top;
    const INTEGER width = right - left;
    const uint8_t(*B)[3] = platinum::kBevelPressed;
    Pen p;

    p.moveTo(left + 3, bottom - 2);
    p.line(width - 6, 0, B[0][0], B[0][1], B[0][2]);
    p.move(0, -1);
    p.line(1, 0, B[0][0], B[0][1], B[0][2]);
    p.line(0, 6 - height, B[0][0], B[0][1], B[0][2]);

    p.move(0, -1);
    p.line(-1, 1, B[1][0], B[1][1], B[1][2]);
    p.line(0, height - 7, B[1][0], B[1][1], B[1][2]);
    p.move(-1, 0);
    p.line(0, 1, B[1][0], B[1][1], B[1][2]);
    p.line(7 - width, 0, B[1][0], B[1][1], B[1][2]);
    p.line(-1, 1, B[1][0], B[1][1], B[1][2]);

    p.move(-1, -1);
    p.line(0, 5 - height, B[2][0], B[2][1], B[2][2]);
    p.move(1, 0);
    p.line(0, -1, B[2][0], B[2][1], B[2][2]);
    p.line(width - 5, 0, B[2][0], B[2][1], B[2][2]);

    p.move(-1, 1);
    p.line(7 - width, 0, B[3][0], B[3][1], B[3][2]);
    p.move(0, 1);
    p.line(-1, 0, B[3][0], B[3][1], B[3][2]);
    p.line(0, height - 7, B[3][0], B[3][1], B[3][2]);
}

/* _draw_frame_bevel — FrameRoundRect + #22 corner dots */
void draw_frame_bevel(const Rect *rect, bool active)
{
    PenNormal();
    if(active)
        platinum_fore(0, 0, 0);
    else
        platinum_fore(platinum::kInactiveFrame[0], platinum::kInactiveFrame[1],
                      platinum::kInactiveFrame[2]);
    FrameRoundRect(rect, platinum::kPushOval, platinum::kPushOval);

    if(!active)
        return;

    const INTEGER L = rect->left, T = rect->top;
    const INTEGER R = rect->right, B = rect->bottom;
    const uint8_t *c = platinum::kCornerDot;
    pixel(L, T + 2, c[0], c[1], c[2]);
    pixel(L + 2, T, c[0], c[1], c[2]);
    pixel(R - 3, T, c[0], c[1], c[2]);
    pixel(R - 1, T + 2, c[0], c[1], c[2]);
    pixel(R - 1, B - 3, c[0], c[1], c[2]);
    pixel(R - 3, B - 1, c[0], c[1], c[2]);
    pixel(L + 2, B - 1, c[0], c[1], c[2]);
    pixel(L, B - 3, c[0], c[1], c[2]);
}

void draw_control_title(ControlHandle c, const Rect *r, bool disabled,
                        bool pressed)
{
    StringPtr title = CTL_TITLE(c);
    if(!title || !title[0])
        return;
    TextFont(0);
    TextSize(12);
    TextFace(0);
    TextMode(srcOr);
    if(disabled)
        platinum_fore(0x88, 0x88, 0x88);
    else if(pressed)
        platinum_fore(0xFF, 0xFF, 0xFF);
    else
        platinum_fore(0, 0, 0);

    INTEGER tw = StringWidth(title);
    INTEGER tx = (r->left + r->right - tw) / 2;
    INTEGER ty = (r->top + r->bottom) / 2 + 4;
    MoveTo(tx, ty);
    DrawString(title);
}

/* Sys7-compatible thumb geometry so hit-testing stays aligned. */
void sys7_thumb_rect(ControlHandle ctl, Rect *out)
{
    INTEGER maxv = CTL_MAX(ctl);
    INTEGER minv = CTL_MIN(ctl);
    INTEGER diff = maxv - minv;
    if(diff <= 0 || CTL_HILITE(ctl) == 255)
    {
        SetRect(out, 0, 0, 0, 0);
        return;
    }
    Rect r = CTL_RECT(ctl);
    INTEGER height = RECT_HEIGHT(&r);
    INTEGER width = RECT_WIDTH(&r);
    INTEGER val = CTL_VALUE(ctl);
    if(height > width)
    {
        out->top = (short)(r.top + width
                           + ((val - minv) * ((LONGINT)height - 3 * width) / diff));
        out->bottom = out->top + width;
        out->left = r.left + 1;
        out->right = r.right - 1;
    }
    else
    {
        out->left = (short)(r.left + height
                            + ((val - minv) * ((LONGINT)width - 3 * height) / diff));
        out->right = out->left + height;
        out->top = r.top + 1;
        out->bottom = r.bottom - 1;
    }
}

/* Track segment sunken bevel — cdef24 _draw_track_segment_bevel */
void draw_track_segment(const Rect *seg, bool vertical)
{
    INTEGER left = seg->left, top = seg->top;
    INTEGER right = seg->right, bottom = seg->bottom;
    INTEGER w = right - left, h = bottom - top;
    if(w <= 0 || h <= 0)
        return;

    Rect fill;
    if(vertical)
        SetRect(&fill, left + 2, top + 2, right - 2, bottom);
    else
        SetRect(&fill, left + 2, top + 2, right, bottom - 2);
    if(fill.right > fill.left && fill.bottom > fill.top)
        platinum_paint_rect(&fill, platinum::kTrackSegFill[0],
                            platinum::kTrackSegFill[1], platinum::kTrackSegFill[2]);

    platinum_vline(left, top, bottom - 1, 0x77, 0x77, 0x77);
    platinum_hline(top, left, right - 1, 0x77, 0x77, 0x77);

    INTEGER sVar1 = vertical ? h : w;
    if(sVar1 > 1)
    {
        platinum_vline(left + 1, top + 1, bottom - 1, 0x88, 0x88, 0x88);
        platinum_hline(top + 1, left + 1, right - 1, 0x88, 0x88, 0x88);
    }
    if(vertical)
    {
        platinum_vline(right - 1, top, bottom - 1, 0xCC, 0xCC, 0xCC);
        if(sVar1 > 1)
            platinum_vline(right - 2, top + 1, bottom - 1, 0xBB, 0xBB, 0xBB);
    }
    else
    {
        platinum_hline(bottom - 1, left, right - 1, 0xCC, 0xCC, 0xCC);
        if(sVar1 > 1)
            platinum_hline(bottom - 2, left + 1, right - 1, 0xBB, 0xBB, 0xBB);
    }
}

void draw_thumb(const Rect *th, bool vertical)
{
    INTEGER left = th->left, top = th->top;
    INTEGER right = th->right, bottom = th->bottom;
    INTEGER w = right - left, h = bottom - top;
    if(w <= 0 || h <= 0)
        return;

    Rect face;
    SetRect(&face, left + 1, top + 1, right - 1, bottom - 1);
    platinum_paint_rect(&face, platinum::kThumbFill[0], platinum::kThumbFill[1],
                        platinum::kThumbFill[2]);

    platinum_vline(left + 1, top + 1, bottom - 2, 0xDD, 0xDD, 0xDD);
    platinum_hline(top + 1, left + 2, right - 2, 0xDD, 0xDD, 0xDD);
    platinum_vline(right - 2, top + 2, bottom - 2, 0x99, 0x99, 0x99);
    platinum_hline(bottom - 2, left + 2, right - 3, 0x99, 0x99, 0x99);

    if(vertical)
    {
        const int grip_h = 8;
        if(h >= grip_h + 6)
        {
            INTEGER gy = top + (h - grip_h) / 2;
            INTEGER gx1 = left + 4, gx2 = right - 5;
            if(gx2 > gx1)
            {
                for(int i = 0; i < 4; ++i)
                {
                    platinum_hline(gy + i * 2, gx1, gx2, 0xDD, 0xDD, 0xDD);
                    platinum_hline(gy + i * 2 + 1, gx1, gx2, 0x99, 0x99, 0x99);
                }
            }
        }
    }
    else
    {
        const int grip_w = 8;
        if(w >= grip_w + 6)
        {
            INTEGER gx = left + (w - grip_w) / 2;
            INTEGER gy1 = top + 4, gy2 = bottom - 5;
            if(gy2 > gy1)
            {
                for(int i = 0; i < 4; ++i)
                {
                    platinum_vline(gx + i * 2, gy1, gy2, 0xDD, 0xDD, 0xDD);
                    platinum_vline(gx + i * 2 + 1, gy1, gy2, 0x99, 0x99, 0x99);
                }
            }
        }
    }

    platinum_frame_rect(th, 0, 0, 0);
}

void draw_arrow(INTEGER x, INTEGER y, const uint8_t *bitmap, bool active,
                bool pressed)
{
    RGBColor clut[16];
    if(!active)
        clut_from(platinum::kArrowClutInactive, clut);
    else if(pressed)
        clut_from(platinum::kArrowClutPressed, clut);
    else
        clut_from(platinum::kArrowClutNormal, clut);
    platinum_blit_4bpp(x, y, bitmap, 16, 16, 8, clut, 4);
}
} /* namespace */

void Executor::platinum_draw_push_button(ControlHandle c, INTEGER part)
{
    Rect r = CTL_RECT(c);
    /* State comes from contrlHilite ALONE (cdef23 RE: pressed when
     * 0 < hilite < 0xFE, inactive when hilite >= 0xFE). The drawCntl
     * param is the part to REDRAW, not a state: TrackControl redraws
     * with param = partstart on mouse-up after zeroing contrlHilite —
     * reading param as "pressed" left buttons stuck dark. */
    int hilite = CTL_HILITE(c) & 0xFF;
    bool disabled = hilite >= 0xFE;
    bool pressed = !disabled && hilite != 0;
    bool active = !disabled;
    (void)part;

    PenNormal();

    /* Step 2: FrameRoundRect work rect (no default outset — apps rarely flag it) */
    platinum_fore(0, 0, 0);
    FrameRoundRect(&r, platinum::kPushOval, platinum::kPushOval);

    /* Step 3: EraseRoundRect face fill */
    if(active && !pressed)
    {
        platinum_back(platinum::kBtnFill[0], platinum::kBtnFill[1],
                      platinum::kBtnFill[2]);
        EraseRoundRect(&r, platinum::kPushOval, platinum::kPushOval);
        draw_normal_bevel(&r);
    }
    else if(active && pressed)
    {
        platinum_back(platinum::kBtnFillPressed[0], platinum::kBtnFillPressed[1],
                      platinum::kBtnFillPressed[2]);
        EraseRoundRect(&r, platinum::kPushOval, platinum::kPushOval);
        draw_pressed_bevel(&r);
    }
    else
    {
        platinum_back(platinum::kBtnFillInactive[0], platinum::kBtnFillInactive[1],
                      platinum::kBtnFillInactive[2]);
        EraseRoundRect(&r, platinum::kPushOval, platinum::kPushOval);
    }

    draw_frame_bevel(&r, active);
    draw_control_title(c, &r, disabled, pressed);
}

void Executor::platinum_draw_checkbox(ControlHandle c, INTEGER /*part*/)
{
    Rect r = CTL_RECT(c);
    int hilite = CTL_HILITE(c) & 0xFF;
    bool disabled = hilite >= 0xFE;
    bool pressed = !disabled && hilite != 0;
    INTEGER val = CTL_VALUE(c); /* 0 off, 1 on, 2 mixed */

    /* cdef23: mark 0=empty, 2=✓, 3=mixed (NOT 1=X) */
    int mark = (val <= 0) ? 0 : (val == 1) ? 2 : 3;
    int state_row = disabled ? 8 : (pressed ? 4 : 0);
    int idx = state_row + mark;

    RGBColor clut[16];
    platinum::cdef23_linear_clut(clut);
    const uint8_t *g = platinum::kCdef23Glyphs + idx * platinum::kCdef23GlyphBytes;
    INTEGER gx = r.left;
    INTEGER gy = r.top + (RECT_HEIGHT(&r) - platinum::kCdef23GlyphH) / 2;
    platinum_blit_4bpp(gx, gy, g, platinum::kCdef23GlyphW, platinum::kCdef23GlyphH,
                       platinum::kCdef23GlyphRowBytes, clut, 0xF);

    StringPtr title = CTL_TITLE(c);
    if(title && title[0])
    {
        TextFont(0);
        TextSize(12);
        TextFace(0);
        platinum_fore(disabled ? 0x88 : 0x00, disabled ? 0x88 : 0x00,
                      disabled ? 0x88 : 0x00);
        /* glyph + 5px gap — cdef23_buttons.py */
        MoveTo(r.left + platinum::kCdef23GlyphW + 5,
               r.top + (RECT_HEIGHT(&r) + 8) / 2);
        DrawString(title);
    }
}

void Executor::platinum_draw_radio(ControlHandle c, INTEGER /*part*/)
{
    Rect r = CTL_RECT(c);
    int hilite = CTL_HILITE(c) & 0xFF;
    bool disabled = hilite >= 0xFE;
    bool pressed = !disabled && hilite != 0;
    INTEGER val = CTL_VALUE(c);
    int mark = (val <= 0) ? 0 : (val == 1) ? 1 : 2;
    int state_row = disabled ? 6 : (pressed ? 3 : 0);
    int idx = 12 + state_row + mark;

    RGBColor clut[16];
    platinum::cdef23_linear_clut(clut);
    const uint8_t *g = platinum::kCdef23Glyphs + idx * platinum::kCdef23GlyphBytes;
    INTEGER gx = r.left;
    INTEGER gy = r.top + (RECT_HEIGHT(&r) - platinum::kCdef23GlyphH) / 2;
    platinum_blit_4bpp(gx, gy, g, platinum::kCdef23GlyphW, platinum::kCdef23GlyphH,
                       platinum::kCdef23GlyphRowBytes, clut, 0xF);

    StringPtr title = CTL_TITLE(c);
    if(title && title[0])
    {
        TextFont(0);
        TextSize(12);
        TextFace(0);
        platinum_fore(disabled ? 0x88 : 0x00, disabled ? 0x88 : 0x00,
                      disabled ? 0x88 : 0x00);
        MoveTo(r.left + platinum::kCdef23GlyphW + 5,
               r.top + (RECT_HEIGHT(&r) + 8) / 2);
        DrawString(title);
    }
}

void Executor::platinum_draw_scrollbar(ControlHandle c, INTEGER part)
{
    Rect r = CTL_RECT(c);
    bool vert = RECT_HEIGHT(&r) > RECT_WIDTH(&r);
    bool disabled = CTL_HILITE(c) == 255 || CTL_MIN(c) >= CTL_MAX(c);
    const int arrow = platinum::kArrowSize;

    PenNormal();

    /* Inactive: flat #EE + black frame only (FUN_000010ea) */
    if(disabled)
    {
        platinum_paint_rect(&r, platinum::kTrackFill[0], platinum::kTrackFill[1],
                            platinum::kTrackFill[2]);
        platinum_frame_rect(&r, 0, 0, 0);
        return;
    }

    bool press_up = (part == inUpButton);
    bool press_down = (part == inDownButton);
    bool press_thumb = (part == inThumb);

    if(vert)
    {
        INTEGER track_top = r.top + arrow;
        INTEGER track_bottom = r.bottom - arrow;
        Rect track;
        SetRect(&track, r.left, track_top, r.right, track_bottom);
        platinum_paint_rect(&track, platinum::kTrackFill[0], platinum::kTrackFill[1],
                            platinum::kTrackFill[2]);

        Rect th;
        sys7_thumb_rect(c, &th);
        bool have_thumb = (th.bottom > th.top);

        if(have_thumb)
        {
            Rect seg;
            if(track_top < th.top)
            {
                SetRect(&seg, r.left + 1, track_top, r.right - 1, th.top);
                draw_track_segment(&seg, true);
            }
            if(th.bottom < track_bottom)
            {
                SetRect(&seg, r.left + 1, th.bottom, r.right - 1, track_bottom);
                draw_track_segment(&seg, true);
            }
            /* Pressed thumb keeps same fill (THUMB_PRESSED == THUMB_FILL). */
            (void)press_thumb;
            draw_thumb(&th, true);
        }

        draw_arrow(r.left, r.top, platinum::kArrowBitmapUp, true, press_up);
        draw_arrow(r.left, r.bottom - arrow, platinum::kArrowBitmapDown, true,
                   press_down);
    }
    else
    {
        INTEGER track_left = r.left + arrow;
        INTEGER track_right = r.right - arrow;
        Rect track;
        SetRect(&track, track_left, r.top, track_right, r.bottom);
        platinum_paint_rect(&track, platinum::kTrackFill[0], platinum::kTrackFill[1],
                            platinum::kTrackFill[2]);

        Rect th;
        sys7_thumb_rect(c, &th);
        bool have_thumb = (th.right > th.left);

        if(have_thumb)
        {
            Rect seg;
            if(track_left < th.left)
            {
                SetRect(&seg, track_left, r.top + 1, th.left, r.bottom - 1);
                draw_track_segment(&seg, false);
            }
            if(th.right < track_right)
            {
                SetRect(&seg, th.right, r.top + 1, track_right, r.bottom - 1);
                draw_track_segment(&seg, false);
            }
            (void)press_thumb;
            draw_thumb(&th, false);
        }

        draw_arrow(r.left, r.top, platinum::kArrowBitmapLeft, true, press_up);
        draw_arrow(r.right - arrow, r.top, platinum::kArrowBitmapRight, true,
                   press_down);
    }

    platinum_frame_rect(&r, 0, 0, 0);
}

/* Diamond popup arrow — cdef25_popup_button.py FUN_00001a50 */
static void draw_popup_arrow(INTEGER cx, INTEGER cy, uint8_t r, uint8_t g, uint8_t b)
{
    platinum_fore(r, g, b);
    /* Down triangle: flat top (cx-5..cx+3, cy+1) → tip (cx-1, cy+5) */
    for(INTEGER row = 0; row <= 4; ++row)
    {
        INTEGER half = 4 - row;
        INTEGER x0 = cx - 1 - half;
        INTEGER x1 = cx - 1 + half;
        platinum_hline(cy + 1 + row, x0, x1, r, g, b);
    }
    /* Up triangle: flat bottom (cx-5..cx+3, cy-1) → tip (cx-1, cy-5) */
    for(INTEGER row = 0; row <= 4; ++row)
    {
        INTEGER half = 4 - row;
        INTEGER x0 = cx - 1 - half;
        INTEGER x1 = cx - 1 + half;
        platinum_hline(cy - 1 - row, x0, x1, r, g, b);
    }
}

void Executor::platinum_draw_popup_button(ControlHandle c, INTEGER part)
{
    Rect r = CTL_RECT(c);
    /* Same contrlHilite-only state rule as platinum_draw_push_button. */
    int hilite = CTL_HILITE(c) & 0xFF;
    bool disabled = hilite >= 0xFE;
    bool pressed = !disabled && hilite != 0;
    bool active = !disabled;
    (void)part;
    const INTEGER L = r.left, T = r.top, R = r.right, B = r.bottom;
    const INTEGER h = B - T;

    PenNormal();

    if(!active)
        platinum_back(0xDD, 0xDD, 0xDD);
    else if(pressed)
        platinum_back(platinum::kPopupFillPressed[0], platinum::kPopupFillPressed[1],
                      platinum::kPopupFillPressed[2]);
    else
        platinum_back(platinum::kPopupFill[0], platinum::kPopupFill[1],
                      platinum::kPopupFill[2]);
    EraseRoundRect(&r, platinum::kPushOval, platinum::kPushOval);

    if(active && !pressed)
    {
        platinum_hline(T + 1, L + 1, R - 2, platinum::kPopupBevelLight[0],
                       platinum::kPopupBevelLight[1], platinum::kPopupBevelLight[2]);
        platinum_vline(L + 1, T + 1, B - 2, platinum::kPopupBevelLight[0],
                       platinum::kPopupBevelLight[1], platinum::kPopupBevelLight[2]);
        platinum_vline(R - 2, T + 2, B - 2, platinum::kPopupBevelShadow[0],
                       platinum::kPopupBevelShadow[1], platinum::kPopupBevelShadow[2]);
        platinum_hline(B - 2, L + 2, R - 2, platinum::kPopupBevelShadow[0],
                       platinum::kPopupBevelShadow[1], platinum::kPopupBevelShadow[2]);
    }
    else if(active && pressed)
    {
        platinum_hline(T + 1, L + 1, R - 2, platinum::kPopupPressedTL[0],
                       platinum::kPopupPressedTL[1], platinum::kPopupPressedTL[2]);
        platinum_vline(L + 1, T + 1, B - 2, platinum::kPopupPressedTL[0],
                       platinum::kPopupPressedTL[1], platinum::kPopupPressedTL[2]);
        platinum_vline(R - 2, T + 2, B - 2, platinum::kPopupPressedBR[0],
                       platinum::kPopupPressedBR[1], platinum::kPopupPressedBR[2]);
        platinum_hline(B - 2, L + 2, R - 2, platinum::kPopupPressedBR[0],
                       platinum::kPopupPressedBR[1], platinum::kPopupPressedBR[2]);
    }

    if(active)
        platinum_fore(0, 0, 0);
    else
        platinum_fore(0x88, 0x88, 0x88);
    FrameRoundRect(&r, platinum::kPushOval, platinum::kPushOval);

    if(active)
    {
        const uint8_t *c22 = platinum::kCornerDot;
        pixel(L, T + 2, c22[0], c22[1], c22[2]);
        pixel(L + 2, T, c22[0], c22[1], c22[2]);
        pixel(R - 3, T, c22[0], c22[1], c22[2]);
        pixel(R - 1, T + 2, c22[0], c22[1], c22[2]);
        pixel(R - 1, B - 3, c22[0], c22[1], c22[2]);
        pixel(R - 3, B - 1, c22[0], c22[1], c22[2]);
        pixel(L + 2, B - 1, c22[0], c22[1], c22[2]);
        pixel(L, B - 3, c22[0], c22[1], c22[2]);
    }

    INTEGER arrow_x = R - platinum::kPopupArrowMargin - platinum::kPopupArrowWidth / 2;
    INTEGER arrow_y = T + h / 2;
    if(!active)
        draw_popup_arrow(arrow_x, arrow_y, 0x88, 0x88, 0x88);
    else if(pressed)
        draw_popup_arrow(arrow_x, arrow_y, 0xFF, 0xFF, 0xFF);
    else
        draw_popup_arrow(arrow_x, arrow_y, 0, 0, 0);

    INTEGER sep_x = R - platinum::kPopupTextRight + 2;
    if(active)
    {
        platinum_vline(sep_x, T + 3, B - 4, platinum::kPopupBevelShadow[0],
                       platinum::kPopupBevelShadow[1], platinum::kPopupBevelShadow[2]);
        platinum_vline(sep_x + 1, T + 3, B - 4, platinum::kPopupBevelLight[0],
                       platinum::kPopupBevelLight[1], platinum::kPopupBevelLight[2]);
    }

    Str255 title;
    title[0] = 0;
    popup_data_handle data = (popup_data_handle)CTL_DATA(c);
    if(data && POPUP_MENU(data))
        GetMenuItemText(POPUP_MENU(data), CTL_VALUE(c), title);
    else if(CTL_TITLE(c) && CTL_TITLE(c)[0])
    {
        INTEGER n = CTL_TITLE(c)[0];
        if(n > 255)
            n = 255;
        title[0] = n;
        BlockMove(CTL_TITLE(c) + 1, title + 1, n);
    }

    if(title[0])
    {
        TextFont(0);
        TextSize(12);
        TextFace(0);
        TextMode(srcOr);
        if(!active)
            platinum_fore(0x88, 0x88, 0x88);
        else if(pressed)
            platinum_fore(0xFF, 0xFF, 0xFF);
        else
            platinum_fore(0, 0, 0);
        MoveTo(L + platinum::kPopupTextLeft, T + (h + 8) / 2);
        DrawString(title);
    }
}

LONGINT Executor::C_cdef_platinum(INTEGER var, ControlHandle c, INTEGER mess,
                                  LONGINT param)
{
    if(mess != drawCntl)
        return -1;
    if(!CTL_VIS(c))
        return 0;

    PortColorGuard color_guard;
    PenNormal();
    switch(var & 0xF)
    {
        case pushButProc & 0xF:
            platinum_draw_push_button(c, (INTEGER)param);
            return 0;
        case checkBoxProc & 0xF:
            platinum_draw_checkbox(c, (INTEGER)param);
            return 0;
        case radioButProc & 0xF:
            platinum_draw_radio(c, (INTEGER)param);
            return 0;
        default:
            return -1;
    }
}

LONGINT Executor::C_cdef_platinum_scroll(INTEGER /*var*/, ControlHandle c,
                                         INTEGER mess, LONGINT param)
{
    if(mess != drawCntl)
        return -1;
    if(!CTL_VIS(c))
        return 0;
    PortColorGuard color_guard;
    PenNormal();
    platinum_draw_scrollbar(c, (INTEGER)param);
    return 0;
}

LONGINT Executor::C_cdef_platinum_popup(INTEGER /*var*/, ControlHandle c,
                                        INTEGER mess, LONGINT param)
{
    if(mess != drawCntl)
        return -1;
    if(!CTL_VIS(c))
        return 0;
    PortColorGuard color_guard;
    PenNormal();
    platinum_draw_popup_button(c, (INTEGER)param);
    return 0;
}
