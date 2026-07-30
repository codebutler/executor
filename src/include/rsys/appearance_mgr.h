/* Platinum Appearance Manager — native ROMlib port of appearence RE. */
#pragma once

#include <base/common.h>
#include <QuickDraw.h>
#include <CQuickDraw.h>
#include <WindowMgr.h>
#include <ControlMgr.h>
#include <MenuMgr.h>

namespace Executor
{
/* Boot: Gestalt 'appr'/'apvr'. Called after InitResources(). */
extern "C" void pc_platinum_init(void);

bool platinum_active(void);

/* Native defprocs (wired when --appearance platinum). */
LONGINT C_wdef_platinum(INTEGER varcode, WindowPtr window, INTEGER message,
                        LONGINT parm);
LONGINT C_cdef_platinum(INTEGER var, ControlHandle c, INTEGER mess,
                        LONGINT param);
LONGINT C_cdef_platinum_scroll(INTEGER var, ControlHandle c, INTEGER mess,
                               LONGINT param);
int32_t C_mbdf_platinum(int16_t sel, int16_t mess, int16_t param1,
                        int32_t param2);
void C_mdef_platinum(INTEGER mess, MenuHandle mh, Rect *rp, Point p,
                     GUEST<INTEGER> *item);

/* Theme paint helpers used by DrawTheme* and defprocs. */
void platinum_rgb(uint8_t r, uint8_t g, uint8_t b, RGBColor *out);
void platinum_fore(uint8_t r, uint8_t g, uint8_t b);
void platinum_back(uint8_t r, uint8_t g, uint8_t b);
void platinum_paint_rect(const Rect *r, uint8_t r8, uint8_t g8, uint8_t b8);
void platinum_frame_rect(const Rect *r, uint8_t r8, uint8_t g8, uint8_t b8);
void platinum_hline(INTEGER y, INTEGER x1, INTEGER x2, uint8_t r8, uint8_t g8,
                    uint8_t b8);
void platinum_vline(INTEGER x, INTEGER y1, INTEGER y2, uint8_t r8, uint8_t g8,
                    uint8_t b8);
/* Blit 4bpp nibbles. Pixels with nibble > skip_above are transparent.
 * WDEF widgets: skip_above=0xA. CDEF23 linear glyphs: 0xF. Thumb: 0x8. */
void platinum_blit_4bpp(INTEGER dst_h, INTEGER dst_v, const uint8_t *nibbles,
                        INTEGER width, INTEGER height, INTEGER row_bytes,
                        const RGBColor *clut /* 16 entries */,
                        int skip_above = 0xA);

void platinum_draw_document_window(WindowPeek w, INTEGER varcode, LONGINT parm);
void platinum_draw_push_button(ControlHandle c, INTEGER part);
void platinum_draw_checkbox(ControlHandle c, INTEGER part);
void platinum_draw_radio(ControlHandle c, INTEGER part);
void platinum_draw_scrollbar(ControlHandle c, INTEGER part);
void platinum_draw_popup_button(ControlHandle c, INTEGER part);
LONGINT C_cdef_platinum_popup(INTEGER var, ControlHandle c, INTEGER mess,
                              LONGINT param);

/* Menu bar color when Platinum is active (#CCCCCC). */
void platinum_menu_bar_color(RGBColor *bar_color);
void platinum_menu_hilite_color(RGBColor *c); /* #3366CC */
}
