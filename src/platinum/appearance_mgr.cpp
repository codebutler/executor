#include <base/common.h>
#include <rsys/appearance.h>
#include <rsys/appearance_mgr.h>
#include <rsys/gestalt.h>

#include <QuickDraw.h>
#include <CQuickDraw.h>
#include <WindowMgr.h>
#include <MemoryMgr.h>

#include <Appearance.h>

#include <quickdraw/cquick.h>
#include <wind/wind.h>

#include <algorithm>
#include <cstring>

#include "theme_data.h"

using namespace Executor;

bool Executor::platinum_active(void)
{
    return ROMlib_get_appearance() == appearance_platinum;
}

extern "C" void pc_platinum_init(void)
{
    /* Gestalt: Appearance Manager present (bit 0). Version 1.0.1. */
    ROMlib_add_to_gestalt_list(gestaltAppearanceAttr, noErr,
                               (1u << gestaltAppearanceExists));
    ROMlib_add_to_gestalt_list(gestaltAppearanceVersion, noErr, 0x0101);
}

static OSErr theme_brush_to_rgb(int16_t brush, RGBColor *out)
{
    /* Platinum brushes are solid greys / whites. */
    uint8_t v = 0xCC;
    switch(brush)
    {
        case 10: /* kThemeBrushListViewBackground */
        case 15: /* kThemeDocumentWindowBackgroundBrush */
        case 16: /* kThemeFinderWindowBackgroundBrush */
            v = 0xFF;
            break;
        case 1: /* dialog / alert / modeless / utility backgrounds — the
                 * inactive variants are the same grey under Platinum */
        case 2:
        case 3:
        case 4:
        case 5:
        case 6:
        case 7:
        case 8:
            v = 0xDD;
            break;
        default:
            if(brush < 1 || brush > 16)
                return appearanceBadBrushIndexErr;
            v = 0xCC;
            break;
    }
    platinum_rgb(v, v, v, out);
    return noErr;
}

OSErr Executor::C_RegisterAppearanceClient(void)
{
    return noErr;
}

OSErr Executor::C_UnregisterAppearanceClient(void)
{
    return noErr;
}

OSErr Executor::C_SetThemePen(int16_t inBrush, int16_t /*inDepth*/,
                                 Boolean /*inIsColorDevice*/)
{
    RGBColor c;
    OSErr err = theme_brush_to_rgb(inBrush, &c);
    if(err == noErr)
        RGBForeColor(&c);
    return err;
}

OSErr Executor::C_SetThemeBackground(int16_t inBrush, int16_t /*inDepth*/,
                                        Boolean /*inIsColorDevice*/)
{
    RGBColor c;
    OSErr err = theme_brush_to_rgb(inBrush, &c);
    if(err == noErr)
        RGBBackColor(&c);
    return err;
}

OSErr Executor::C_SetThemeTextColor(int16_t inColor, int16_t /*inDepth*/,
                                       Boolean /*inIsColorDevice*/)
{
    if(inColor < 1 || inColor > 38)
        return appearanceBadTextColorIndexErr;
    platinum_fore(0, 0, 0);
    return noErr;
}

OSErr Executor::C_SetThemeWindowBackground(WindowPtr inWindow,
                                              int16_t inBrush, Boolean inUpdate)
{
    RGBColor c;
    OSErr err = theme_brush_to_rgb(inBrush, &c);
    if(err != noErr)
        return err;
    if(!inWindow)
        return paramErr;

    /* AM 1.0 semantics: the brush becomes the window's CONTENT color, so
     * every erase — Window Manager (ShowHide/PaintOne), Dialog Manager,
     * the app's own EraseRect via the port bkColor — paints the theme
     * background. SetWinColor installs the ctab, updates the port's
     * background color, erases visible content, and redraws the frame.
     *
     * IMPORTANT: Executor's SetWinColor LEAVES thePort switched to the
     * window-manager port when the window is visible (upstream FIXME in
     * windColor.cpp). Callers of SetThemeWindowBackground keep drawing
     * into their own port afterwards — without restoring, everything they
     * draw lands on the invisible screen framebuffer (seen only as a
     * strip of garbage through the rootless menubar band). Save and
     * restore thePort across the whole call. */
    GrafPtr save_port = qdGlobals().thePort;

    CTabHandle ctab = (CTabHandle)NewHandle(CTAB_STORAGE_FOR_SIZE(12));
    if(!ctab)
        return memFullErr;
    CTAB_SEED(ctab) = 0;
    CTAB_FLAGS(ctab) = 0;
    CTAB_SIZE(ctab) = 12;
    memcpy(CTAB_TABLE(ctab), default_color_win_ctab,
           13 * sizeof(ColorSpec));
    ColorSpec *table = CTAB_TABLE(ctab);
    for(int i = 0; i <= 12; ++i)
        if(table[i].value == wContentColor)
            table[i].rgb = c;
    SetWinColor(inWindow, ctab);

    if(inUpdate)
    {
        /* Queue an update so the app repaints its content over the new
         * background (SetWinColor already erased it). */
        SetPort(inWindow);
        Rect r = PORT_RECT(inWindow);
        InvalRect(&r);
    }

    if(save_port)
        SetPort(save_port);
    return noErr;
}

/* DrawTheme* below are direct ports of appearence/appearance/__init__.py. */

OSErr Executor::C_DrawThemeWindowHeader(const Rect *inRect, uint32_t inState)
{
    if(inState == kThemeStatePressed)
        return paramErr;
    const INTEGER L = inRect->left, T = inRect->top;
    const INTEGER R = inRect->right, B = inRect->bottom;
    platinum_paint_rect(inRect, 0xDD, 0xDD, 0xDD);
    if(inState == kThemeStateActive)
    {
        platinum_frame_rect(inRect, 0, 0, 0);
        platinum_vline(L + 1, T + 1, B - 3, 0xFF, 0xFF, 0xFF);
        platinum_hline(T + 1, L + 1, R - 2, 0xFF, 0xFF, 0xFF);
        platinum_hline(B - 2, L + 2, R - 2, 0xAA, 0xAA, 0xAA);
        platinum_vline(R - 2, T + 2, B - 2, 0xAA, 0xAA, 0xAA);
    }
    else
        platinum_frame_rect(inRect, 0x55, 0x55, 0x55);
    return noErr;
}

OSErr Executor::C_DrawThemeWindowListViewHeader(const Rect *inRect, uint32_t inState)
{
    if(inState == kThemeStatePressed)
        return paramErr;
    const INTEGER L = inRect->left, T = inRect->top;
    const INTEGER R = inRect->right, B = inRect->bottom;
    platinum_paint_rect(inRect, 0xDD, 0xDD, 0xDD);
    if(inState == kThemeStateActive)
    {
        /* 3-sided black frame (no bottom) */
        platinum_vline(L, T, B - 1, 0, 0, 0);
        platinum_hline(T, L, R - 1, 0, 0, 0);
        platinum_vline(R - 1, T, B - 1, 0, 0, 0);
        platinum_vline(L + 1, T + 1, B - 2, 0xFF, 0xFF, 0xFF);
        platinum_hline(T + 1, L + 1, R - 2, 0xFF, 0xFF, 0xFF);
        platinum_hline(B - 1, L + 2, R - 2, 0xAA, 0xAA, 0xAA);
        platinum_vline(R - 2, T + 2, B - 1, 0xAA, 0xAA, 0xAA);
    }
    else
    {
        platinum_vline(L, T, B - 1, 0x55, 0x55, 0x55);
        platinum_hline(T, L, R - 1, 0x55, 0x55, 0x55);
        platinum_vline(R - 1, T, B - 1, 0x55, 0x55, 0x55);
    }
    return noErr;
}

OSErr Executor::C_DrawThemePlacard(const Rect *inRect, uint32_t inState)
{
    if(inState == kThemeStatePressed)
        return paramErr;
    const INTEGER L = inRect->left, T = inRect->top;
    const INTEGER R = inRect->right, B = inRect->bottom;
    if(inState == kThemeStateActive)
    {
        platinum_paint_rect(inRect, 0xDD, 0xDD, 0xDD);
        platinum_frame_rect(inRect, 0, 0, 0);
        /* Left + top white highlight (CDEF 14 / DrawThemePlacard) */
        platinum_vline(L + 1, T + 1, B - 3, 0xFF, 0xFF, 0xFF);
        platinum_hline(T + 1, L + 1, R - 2, 0xFF, 0xFF, 0xFF);
        platinum_hline(B - 2, L + 2, R - 2, 0xAA, 0xAA, 0xAA);
        platinum_vline(R - 2, T + 2, B - 2, 0xAA, 0xAA, 0xAA);
    }
    else
    {
        platinum_paint_rect(inRect, 0xEE, 0xEE, 0xEE);
        platinum_frame_rect(inRect, 0x55, 0x55, 0x55);
    }
    return noErr;
}

OSErr Executor::C_DrawThemeModelessDialogFrame(const Rect *inRect, uint32_t inState)
{
    if(inState == kThemeStatePressed)
        return paramErr;
    const INTEGER L = inRect->left, T = inRect->top;
    const INTEGER R = inRect->right, B = inRect->bottom;
    platinum_frame_rect(inRect, 0, 0, 0);
    if(inState == kThemeStateActive)
    {
        platinum_vline(R - 2, T + 2, B - 2, 0, 0, 0);
        platinum_hline(B - 2, L + 2, R - 2, 0, 0, 0);
        platinum_vline(R - 3, T + 2, B - 2, 0x99, 0x99, 0x99);
        platinum_hline(B - 3, L + 2, R - 3, 0x99, 0x99, 0x99);
    }
    else
    {
        Rect inner = *inRect;
        InsetRect(&inner, 1, 1);
        platinum_frame_rect(&inner, 0xDD, 0xDD, 0xDD);
    }
    return noErr;
}

OSErr Executor::C_DrawThemeEditTextFrame(const Rect *inRect, uint32_t inState)
{
    if(inState == kThemeStatePressed)
        return paramErr;
    const INTEGER L = inRect->left, T = inRect->top;
    const INTEGER R = inRect->right, B = inRect->bottom;
    if(inState == kThemeStateActive)
    {
        /* Outer sunken layer outside rect */
        platinum_hline(T - 1, L - 1, R, 0x88, 0x88, 0x88);
        platinum_vline(L - 1, T - 1, B, 0x88, 0x88, 0x88);
        platinum_vline(R, T - 1, B, 0xFF, 0xFF, 0xFF);
        platinum_hline(B, L - 1, R, 0xFF, 0xFF, 0xFF);
        /* Inner layer */
        platinum_hline(T - 2, L - 2, R + 1, 0xCC, 0xCC, 0xCC);
        platinum_vline(L - 2, T - 2, B + 1, 0xCC, 0xCC, 0xCC);
        platinum_vline(R + 1, T - 2, B + 1, 0xFF, 0xFF, 0xFF);
        platinum_hline(B + 1, L - 2, R + 1, 0xFF, 0xFF, 0xFF);
    }
    else
    {
        Rect r;
        SetRect(&r, L - 1, T - 1, R + 1, B + 1);
        platinum_frame_rect(&r, 0x88, 0x88, 0x88);
    }
    return noErr;
}

OSErr Executor::C_DrawThemeListBoxFrame(const Rect *inRect, uint32_t inState)
{
    return C_DrawThemeEditTextFrame(inRect, inState);
}

OSErr Executor::C_DrawThemeFocusRect(const Rect *inRect, Boolean inHasFocus)
{
    const INTEGER L = inRect->left, T = inRect->top;
    const INTEGER R = inRect->right, B = inRect->bottom;
    Rect outer, mid, inner;
    SetRect(&outer, L - 3, T - 3, R + 3, B + 3);
    SetRect(&mid, L - 2, T - 2, R + 2, B + 2);
    SetRect(&inner, L - 1, T - 1, R + 1, B + 1);
    if(inHasFocus)
    {
        const uint8_t *ring = platinum::kFocusRing;
        uint8_t ro = (uint8_t)std::min(255, (int)ring[0] + 0x33);
        uint8_t go = (uint8_t)std::min(255, (int)ring[1] + 0x33);
        uint8_t bo = (uint8_t)std::min(255, (int)ring[2] + 0x33);
        platinum_frame_rect(&outer, ro, go, bo);
        platinum_frame_rect(&mid, ring[0], ring[1], ring[2]);
        platinum_frame_rect(&inner, ring[0], ring[1], ring[2]);
    }
    else
    {
        platinum_frame_rect(&outer, 0xFF, 0xFF, 0xFF);
        platinum_frame_rect(&mid, 0xFF, 0xFF, 0xFF);
        platinum_frame_rect(&inner, 0xFF, 0xFF, 0xFF);
    }
    return noErr;
}

OSErr Executor::C_DrawThemePrimaryGroup(const Rect *inRect, uint32_t inState)
{
    if(inState == kThemeStatePressed)
        return paramErr;
    uint8_t outer = (inState == kThemeStateActive) ? 0x99 : 0xBB;
    platinum_frame_rect(inRect, outer, outer, outer);
    Rect r = *inRect;
    InsetRect(&r, 1, 1);
    platinum_frame_rect(&r, 0xEE, 0xEE, 0xEE);
    return noErr;
}

OSErr Executor::C_DrawThemeSecondaryGroup(const Rect *inRect, uint32_t inState)
{
    if(inState == kThemeStatePressed)
        return paramErr;
    uint8_t outer = (inState == kThemeStateActive) ? 0xAA : 0xCC;
    platinum_frame_rect(inRect, outer, outer, outer);
    Rect r = *inRect;
    InsetRect(&r, 1, 1);
    platinum_frame_rect(&r, 0xEE, 0xEE, 0xEE);
    return noErr;
}

OSErr Executor::C_DrawThemeSeparator(const Rect *inRect, uint32_t inState)
{
    if(inState == kThemeStatePressed)
        return paramErr;
    const INTEGER L = inRect->left, T = inRect->top;
    const INTEGER R = inRect->right, B = inRect->bottom;
    const INTEGER w = R - L, h = B - T;
    uint8_t dark = (inState == kThemeStateActive) ? 0xBB : 0x88;
    uint8_t light = (inState == kThemeStateActive) ? 0xDD : 0xFF;
    if(w >= h)
    {
        INTEGER mid = T + h / 2;
        platinum_hline(mid, L, R - 1, dark, dark, dark);
        platinum_hline(mid + 1, L, R - 1, light, light, light);
    }
    else
    {
        INTEGER mid = L + w / 2;
        platinum_vline(mid, T, B - 1, dark, dark, dark);
        platinum_vline(mid + 1, T, B - 1, light, light, light);
    }
    return noErr;
}

OSErr Executor::C_DrawThemeMenuBarBackground(const Rect *inBounds,
                                                int16_t /*inState*/,
                                                uint32_t /*inAttributes*/)
{
    platinum_paint_rect(inBounds, platinum::kMenuBar[0], platinum::kMenuBar[1],
                        platinum::kMenuBar[2]);
    platinum_hline(inBounds->top, inBounds->left, inBounds->right - 1, 0xEE, 0xEE, 0xEE);
    platinum_hline(inBounds->bottom - 2, inBounds->left, inBounds->right - 1, 0xAA, 0xAA,
                   0xAA);
    platinum_hline(inBounds->bottom - 1, inBounds->left, inBounds->right - 1, 0, 0, 0);
    return noErr;
}

OSErr Executor::C_DrawThemeMenuTitle(const Rect * /*inMenuBarRect*/,
                                        const Rect *inTitleRect, int16_t inState,
                                        uint32_t /*inAttributes*/, ProcPtr /*inTitleProc*/,
                                        uint32_t /*inTitleData*/)
{
    if(inState == kThemeMenuSelected)
        platinum_paint_rect(inTitleRect, 0, 0, 0);
    return noErr;
}

OSErr Executor::C_GetThemeMenuBarHeight(GUEST<int16_t> *outHeight)
{
    if(outHeight)
        *outHeight = platinum::kMenuBarHeight;
    return noErr;
}

OSErr Executor::C_DrawThemeMenuBackground(const Rect *inMenuRect,
                                             int16_t /*inMenuType*/)
{
    platinum_paint_rect(inMenuRect, 0xFF, 0xFF, 0xFF);
    platinum_frame_rect(inMenuRect, 0, 0, 0);
    /* 2px drop shadow (bottom-right) — MDEF 63 / DrawThemeMenuBackground */
    for(int i = 0; i < 2; ++i)
    {
        platinum_vline(inMenuRect->right + i, inMenuRect->top + 2,
                       inMenuRect->bottom + i, 0x88, 0x88, 0x88);
        platinum_hline(inMenuRect->bottom + i, inMenuRect->left + 2,
                       inMenuRect->right + i, 0x88, 0x88, 0x88);
    }
    return noErr;
}

OSErr Executor::C_GetThemeMenuBackgroundRegion(const Rect *inMenuRect,
                                                  int16_t /*menuType*/,
                                                  RgnHandle region)
{
    RectRgn(region, inMenuRect);
    return noErr;
}

OSErr Executor::C_DrawThemeMenuItem(const Rect * /*inMenuRect*/,
                                       const Rect *inItemRect,
                                       int16_t /*inVirtualMenuTop*/,
                                       int16_t /*inVirtualMenuBottom*/,
                                       int16_t inState, int16_t /*inItemType*/,
                                       ProcPtr /*inDrawProc*/, uint32_t /*inUserData*/)
{
    if(inState == kThemeMenuSelected)
        platinum_paint_rect(inItemRect, platinum::kMenuHilite[0],
                            platinum::kMenuHilite[1], platinum::kMenuHilite[2]);
    return noErr;
}

OSErr Executor::C_DrawThemeMenuSeparator(const Rect *inItemRect)
{
    INTEGER mid = (inItemRect->top + inItemRect->bottom) / 2;
    platinum_hline(mid, inItemRect->left + 1, inItemRect->right - 2, 0x88, 0x88, 0x88);
    platinum_hline(mid + 1, inItemRect->left + 1, inItemRect->right - 2, 0xFF, 0xFF, 0xFF);
    return noErr;
}

OSErr Executor::C_GetThemeMenuSeparatorHeight(GUEST<int16_t> *outHeight)
{
    if(outHeight)
        *outHeight = platinum::kMenuSeparatorHeight;
    return noErr;
}

OSErr Executor::C_GetThemeMenuItemExtra(int16_t /*inItemType*/,
                                           GUEST<int16_t> *outHeight,
                                           GUEST<int16_t> *outWidth)
{
    if(outHeight)
        *outHeight = 0;
    if(outWidth)
        *outWidth = 0;
    return noErr;
}

OSErr Executor::C_GetThemeMenuTitleExtra(GUEST<int16_t> *outWidth,
                                            Boolean inIsSquished)
{
    if(outWidth)
        *outWidth = inIsSquished ? 8 : 14;
    return noErr;
}

OSErr Executor::C_DrawThemeGenericWell(const Rect *inRect, uint32_t inState,
                                          Boolean inFillCenter)
{
    if(inState == kThemeStatePressed)
        return paramErr;
    const INTEGER L = inRect->left, T = inRect->top;
    const INTEGER R = inRect->right, B = inRect->bottom;
    if(inFillCenter)
    {
        Rect face;
        SetRect(&face, L + 2, T + 2, R - 2, B - 2);
        platinum_paint_rect(&face, 0xFF, 0xFF, 0xFF);
    }
    if(inState == kThemeStateActive)
    {
        platinum_hline(T, L, R - 1, 0x88, 0x88, 0x88);
        platinum_vline(L, T, B - 1, 0x88, 0x88, 0x88);
        platinum_vline(R - 1, T, B - 1, 0xFF, 0xFF, 0xFF);
        platinum_hline(B - 1, L, R - 1, 0xFF, 0xFF, 0xFF);
        platinum_hline(T + 1, L + 1, R - 2, 0x55, 0x55, 0x55);
        platinum_vline(L + 1, T + 1, B - 2, 0x55, 0x55, 0x55);
    }
    else
    {
        platinum_frame_rect(inRect, 0xAA, 0xAA, 0xAA);
        Rect r = *inRect;
        InsetRect(&r, 1, 1);
        platinum_frame_rect(&r, 0xDD, 0xDD, 0xDD);
    }
    return noErr;
}

OSErr Executor::C_DrawThemeFocusRegion(RgnHandle inRegion, Boolean inHasFocus)
{
    if(!inHasFocus || !inRegion)
        return noErr;
    Rect r = (*inRegion)->rgnBBox;
    return C_DrawThemeFocusRect(&r, true);
}

Boolean Executor::C_IsThemeInColor(int16_t inDepth, Boolean inIsColorDevice)
{
    return inIsColorDevice && inDepth >= 4;
}

OSErr Executor::C_GetThemeAccentColors(GUEST<CTabHandle> *outColors)
{
    /* Silver Platinum reports no accents (Appearance.h note). */
    if(outColors)
        *outColors = nullptr;
    return appearanceThemeHasNoAccents;
}
