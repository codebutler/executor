# Platinum Appearance Manager

Native ROMlib port of Mac OS 8/9 Platinum (Silver) chrome + Appearance Manager
APIs, driven by the reverse engineering in
[`codebutler/appearence`](https://github.com/codebutler/appearence).

## Activation

```
executor --appearance platinum
```

## Sources

| File | Role |
|---|---|
| `appearance_mgr.cpp` | Gestalt init, `0xAA74` DrawTheme*/SetTheme* |
| `../include/rsys/appearance_mgr.h` | Public header |
| `../../multiversal/defs/Appearance.yaml` | Multiversal trap/API definitions |
| `wdef_platinum.cpp` | Document window paint (WDEF 64 look) |
| `cdef_platinum.cpp` | Push/check/radio + scrollbar + popup paint |
| `menu_platinum.cpp` | Menu bar color hook |
| `theme_*.` | Silver tables, QD helpers, embedded glyphs |

Glyphs extracted from `appearence/original/{wdef_64,cdef_23}.bin` and
`cdef24_scrollbar.py` hex dumps — data only, not runnable Apple code.

Wired into `src/CMakeLists.txt` and called from `main.cpp` via
`pc_platinum_init()` after `InitResources()`.
