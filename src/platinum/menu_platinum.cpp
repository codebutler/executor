/* Platinum menu bar / menu item paint hooks.
 * Most MBDF/MDEF messages still go through Sys7; we override bar color and
 * provide DrawTheme*-compatible paint used from the patched mbdf path.
 */
#include <base/common.h>
#include <rsys/appearance_mgr.h>

using namespace Executor;

int32_t Executor::C_mbdf_platinum(int16_t /*sel*/, int16_t /*mess*/,
                                  int16_t /*param1*/, int32_t /*param2*/)
{
    /* Sentinel: Sys7 MBDF handles structure; color comes from menu_bar_color. */
    return -1;
}

void Executor::C_mdef_platinum(INTEGER /*mess*/, MenuHandle /*mh*/, Rect * /*rp*/,
                              Point /*p*/, GUEST<INTEGER> * /*item*/)
{
    /* unused — Sys7 MDEF retained; hilite color patched via menu colors */
}
