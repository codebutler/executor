#include <base/common.h>
#include <QuickDraw.h>
#include <CQuickDraw.h>
#include <MemoryMgr.h>

#include <rsys/appearance_mgr.h>
#include <quickdraw/cquick.h>

#include "theme_data.h"

using namespace Executor;

void Executor::platinum_rgb(uint8_t r, uint8_t g, uint8_t b, RGBColor *out)
{
    out->red = (unsigned short)((r << 8) | r);
    out->green = (unsigned short)((g << 8) | g);
    out->blue = (unsigned short)((b << 8) | b);
}

void Executor::platinum_fore(uint8_t r, uint8_t g, uint8_t b)
{
    RGBColor c;
    platinum_rgb(r, g, b, &c);
    RGBForeColor(&c);
}

void Executor::platinum_back(uint8_t r, uint8_t g, uint8_t b)
{
    RGBColor c;
    platinum_rgb(r, g, b, &c);
    RGBBackColor(&c);
}

void Executor::platinum_paint_rect(const Rect *r, uint8_t r8, uint8_t g8, uint8_t b8)
{
    platinum_fore(r8, g8, b8);
    PaintRect(r);
}

void Executor::platinum_frame_rect(const Rect *r, uint8_t r8, uint8_t g8, uint8_t b8)
{
    PenNormal();
    platinum_fore(r8, g8, b8);
    FrameRect(r);
}

void Executor::platinum_hline(INTEGER y, INTEGER x1, INTEGER x2, uint8_t r8,
                             uint8_t g8, uint8_t b8)
{
    PenNormal();
    platinum_fore(r8, g8, b8);
    MoveTo(x1, y);
    LineTo(x2, y);
}

void Executor::platinum_vline(INTEGER x, INTEGER y1, INTEGER y2, uint8_t r8,
                             uint8_t g8, uint8_t b8)
{
    PenNormal();
    platinum_fore(r8, g8, b8);
    MoveTo(x, y1);
    LineTo(x, y2);
}

void Executor::platinum_blit_4bpp(INTEGER dst_h, INTEGER dst_v,
                                 const uint8_t *nibbles, INTEGER width,
                                 INTEGER height, INTEGER row_bytes,
                                 const RGBColor *clut, int skip_above)
{
    for(INTEGER row = 0; row < height; ++row)
    {
        const uint8_t *rowp = nibbles + row * row_bytes;
        for(INTEGER col = 0; col < width; ++col)
        {
            uint8_t byte = rowp[col >> 1];
            uint8_t nibble = (col & 1) ? (byte & 0x0F) : ((byte >> 4) & 0x0F);
            if(nibble > skip_above)
                continue;
            RGBColor c = clut[nibble];
            RGBForeColor(&c);
            Rect px;
            SetRect(&px, dst_h + col, dst_v + row, dst_h + col + 1, dst_v + row + 1);
            PaintRect(&px);
        }
    }
}

void Executor::platinum_menu_bar_color(RGBColor *bar_color)
{
    platinum_rgb(platinum::kMenuBar[0], platinum::kMenuBar[1], platinum::kMenuBar[2],
                 bar_color);
}

void Executor::platinum_menu_hilite_color(RGBColor *c)
{
    platinum_rgb(platinum::kMenuHilite[0], platinum::kMenuHilite[1],
                 platinum::kMenuHilite[2], c);
}
