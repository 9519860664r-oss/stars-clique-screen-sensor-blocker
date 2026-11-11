// Stars Clique Screen Sensor Blocker
// Turns the touch screen off/on with a hotkey (default Num *, which is Fn + * on many laptops).
// Tray icon + small on-screen notice. Plain Win32, no runtime dependencies.
// Build with /DLANG_EN for the English version.
//
// Copyright (c) 2026 Stars Clique. PolyForm Noncommercial License 1.0.0.

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shellapi.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devguid.h>
#include <shellscalingapi.h>
#include <dbt.h>
#include <math.h>
#include <stdint.h>
#include <wchar.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "advapi32.lib")

#ifdef LANG_EN
#define TR(ru, en) en
#else
#define TR(ru, en) ru
#endif

#define APP_NAME      L"Stars Clique Screen Sensor Blocker"
#define TOUCH_HWID    L"HID_DEVICE_UP:000D_U:0004"   // HID digitizer page, usage "touch screen"
#define DEFAULT_VK    VK_MULTIPLY                     // Num * (Fn + * on laptops without a numpad)

#define WM_TRAY       (WM_APP + 1)
#define WM_TOGGLED    (WM_APP + 2)   // wParam: new state, lParam: 1 = error
#define WM_CAPCANCEL  (WM_APP + 3)
#define ID_HOTKEY     1
#define ID_ANIM       2
#define ID_RESYNC     3
#define ID_TRAYRETRY  4
#define CMD_TOGGLE     100
#define CMD_EXIT       101
#define CMD_RESTARTOPT 102
#define CMD_HOTKEY     103

#define GLYPH_LOCK     L"\xE72E"
#define GLYPH_UNLOCK   L"\xE785"
#define GLYPH_KEYBOARD L"\xE765"
#define GLYPH_ERROR    L"\xE711"
#define COL_RED        RGB(244, 63, 94)
#define COL_VIOLET     RGB(139, 92, 246)
#define COL_GRAY       RGB(120, 120, 128)

enum { ST_UNKNOWN = -1, ST_ON = 0, ST_BLOCKED = 1 };

static HINSTANCE g_inst;
static HWND      g_main, g_osd, g_cap;
static UINT      g_taskbarCreated;
static int       g_state = ST_UNKNOWN;
static volatile LONG g_busy;
static HICON     g_iconOn, g_iconOff;
static int       g_trayTries;
static UINT      g_hkVk = DEFAULT_VK, g_hkMods;
static wchar_t   g_hkName[64];

// ---------------------------------------------------------------- device

// Finds the touch screen. Caller destroys *set.
static BOOL find_touch(HDEVINFO *set, SP_DEVINFO_DATA *dev)
{
    HDEVINFO s = SetupDiGetClassDevsW(&GUID_DEVCLASS_HIDCLASS, NULL, NULL, DIGCF_PRESENT);
    if (s == INVALID_HANDLE_VALUE) return FALSE;
    SP_DEVINFO_DATA d = { sizeof d };
    for (DWORD i = 0; SetupDiEnumDeviceInfo(s, i, &d); i++) {
        wchar_t ids[2048] = { 0 };
        if (!SetupDiGetDeviceRegistryPropertyW(s, &d, SPDRP_HARDWAREID, NULL, (BYTE *)ids, sizeof ids - 4, NULL))
            continue;
        for (wchar_t *p = ids; *p; p += wcslen(p) + 1) {
            if (_wcsicmp(p, TOUCH_HWID) == 0) { *set = s; *dev = d; return TRUE; }
        }
    }
    SetupDiDestroyDeviceInfoList(s);
    return FALSE;
}

static int read_state(SP_DEVINFO_DATA *d)
{
    ULONG st = 0, prob = 0;
    if (CM_Get_DevNode_Status(&st, &prob, d->DevInst, 0) != CR_SUCCESS) return ST_UNKNOWN;
    return ((st & DN_HAS_PROBLEM) && prob == CM_PROB_DISABLED) ? ST_BLOCKED : ST_ON;
}

static int query_state(void)
{
    HDEVINFO s; SP_DEVINFO_DATA d;
    if (!find_touch(&s, &d)) return ST_UNKNOWN;
    int r = read_state(&d);
    SetupDiDestroyDeviceInfoList(s);
    return r;
}

static BOOL prop_change(HDEVINFO s, SP_DEVINFO_DATA *d, DWORD change, DWORD scope)
{
    SP_PROPCHANGE_PARAMS p = { 0 };
    p.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
    p.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
    p.StateChange = change;
    p.Scope = scope;
    p.HwProfile = 0;
    if (!SetupDiSetClassInstallParamsW(s, d, &p.ClassInstallHeader, sizeof p)) return FALSE;
    return SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, s, d);
}

// Returns the new state, or ST_UNKNOWN on failure.
static int set_blocked(BOOL block)
{
    HDEVINFO s; SP_DEVINFO_DATA d;
    if (!find_touch(&s, &d)) return ST_UNKNOWN;
    if (block) {
        prop_change(s, &d, DICS_DISABLE, DICS_FLAG_GLOBAL);
    } else {
        // Same order devcon uses: clear the global flag, then the profile flag.
        prop_change(s, &d, DICS_ENABLE, DICS_FLAG_GLOBAL);
        prop_change(s, &d, DICS_ENABLE, DICS_FLAG_CONFIGSPECIFIC);
    }
    int r = read_state(&d);
    SetupDiDestroyDeviceInfoList(s);
    return r;
}

static DWORD WINAPI toggle_thread(LPVOID arg)
{
    (void)arg;
    // Read the real state first: it may have been changed in Device Manager.
    int cur = query_state();
    BOOL block = (cur != ST_BLOCKED);
    int r = cur == ST_UNKNOWN ? ST_UNKNOWN : set_blocked(block);
    BOOL ok = (r == (block ? ST_BLOCKED : ST_ON));
    // Clear the flag here, not in the UI thread, so WM_DESTROY can wait for it.
    InterlockedExchange(&g_busy, 0);
    PostMessageW(g_main, WM_TOGGLED, (WPARAM)(ok ? r : query_state()), ok ? 0 : 1);
    return 0;
}

static void osd_show(int state, BOOL error);
static int g_shown = ST_UNKNOWN;   // what the notice currently says

// Device work runs off the UI thread. The notice is shown right away
// with the expected result and corrected if it fails.
static void request_toggle(void)
{
    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0) return;
    g_shown = ST_UNKNOWN;
    if (g_state != ST_UNKNOWN) {
        g_shown = g_state == ST_BLOCKED ? ST_ON : ST_BLOCKED;
        osd_show(g_shown, FALSE);
    }
    HANDLE t = CreateThread(NULL, 0, toggle_thread, NULL, 0, NULL);
    if (t) CloseHandle(t); else InterlockedExchange(&g_busy, 0);
}

// ---------------------------------------------------------------- settings (HKCU)

#define REG_KEY      L"Software\\Stars Clique\\Screen Sensor Blocker"
#define REG_REBLOCK  L"BlockAfterLogon"     // set at shutdown: block again after logon
#define REG_OPT      L"UnblockForRestart"   // setting: turn touch on for restart (default on)
#define REG_HK_KEY   L"HotkeyKey"
#define REG_HK_MODS  L"HotkeyMods"

static DWORD reg_get(const wchar_t *name, DWORD def)
{
    DWORD v = def, sz = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, REG_KEY, name, RRF_RT_REG_DWORD, NULL, &v, &sz) != ERROR_SUCCESS) v = def;
    return v;
}

static void reg_set(const wchar_t *name, DWORD v)
{
    RegSetKeyValueW(HKEY_CURRENT_USER, REG_KEY, name, REG_DWORD, &v, sizeof v);
}

static void reg_del(const wchar_t *name)
{
    RegDeleteKeyValueW(HKEY_CURRENT_USER, REG_KEY, name);
}

static BOOL get_unblock_option(void) { return reg_get(REG_OPT, 1) != 0; }

// ---------------------------------------------------------------- hotkey

static BOOL is_extended_key(UINT vk)
{
    switch (vk) {
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_DIVIDE: case VK_NUMLOCK:
    case VK_RCONTROL: case VK_RMENU: case VK_APPS:
        return TRUE;
    }
    return FALSE;
}

static void hotkey_format(UINT vk, UINT mods, wchar_t *out, int n)
{
    out[0] = 0;
    if (mods & MOD_CONTROL) wcscat_s(out, n, L"Ctrl + ");
    if (mods & MOD_ALT)     wcscat_s(out, n, L"Alt + ");
    if (mods & MOD_SHIFT)   wcscat_s(out, n, L"Shift + ");
    if (mods & MOD_WIN)     wcscat_s(out, n, L"Win + ");
    wchar_t key[32] = { 0 };
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) {
        key[0] = (wchar_t)vk;   // layout-independent (no Cyrillic letters for Latin keys)
    } else {
        UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        LONG lp = (LONG)((sc << 16) | (is_extended_key(vk) ? (1u << 24) : 0));
        if (!sc || !GetKeyNameTextW(lp, key, 32)) swprintf(key, 32, L"0x%02X", vk);
    }
    wcscat_s(out, n, key);
}

static void hotkey_load(void)
{
    g_hkVk = reg_get(REG_HK_KEY, DEFAULT_VK);
    g_hkMods = reg_get(REG_HK_MODS, 0) & (MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN);
    if (!g_hkVk || g_hkVk > 0xFE) { g_hkVk = DEFAULT_VK; g_hkMods = 0; }
    hotkey_format(g_hkVk, g_hkMods, g_hkName, ARRAYSIZE(g_hkName));
}

static void hotkey_save(void)
{
    if (g_hkVk == DEFAULT_VK && g_hkMods == 0) { reg_del(REG_HK_KEY); reg_del(REG_HK_MODS); }
    else { reg_set(REG_HK_KEY, g_hkVk); reg_set(REG_HK_MODS, g_hkMods); }
    hotkey_format(g_hkVk, g_hkMods, g_hkName, ARRAYSIZE(g_hkName));
}

static BOOL hotkey_register(void)
{
    UnregisterHotKey(g_main, ID_HOTKEY);
    return RegisterHotKey(g_main, ID_HOTKEY, g_hkMods | MOD_NOREPEAT, g_hkVk);
}

// Keys that are fine on their own; anything that types text needs Ctrl, Alt or Win.
static BOOL standalone_ok(UINT vk)
{
    return (vk >= VK_F1 && vk <= VK_F24) || (vk >= VK_NUMPAD0 && vk <= VK_DIVIDE) ||
           vk == VK_PAUSE || vk == VK_SCROLL || (vk >= VK_BROWSER_BACK && vk <= VK_LAUNCH_APP2);
}

static BOOL is_modifier(UINT vk)
{
    return vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN ||
           (vk >= VK_LSHIFT && vk <= VK_RMENU) || vk == VK_CAPITAL || vk == VK_NUMLOCK;
}

// ---------------------------------------------------------------- drawing

typedef struct { uint32_t *px; int w, h; } Canvas;

static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

static float sd_rrect(float px, float py, float cx, float cy, float hw, float hh, float r)
{
    float qx = fabsf(px - cx) - hw + r, qy = fabsf(py - cy) - hh + r;
    float ox = fmaxf(qx, 0), oy = fmaxf(qy, 0);
    return sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0) - r;
}

static float sd_circle(float px, float py, float cx, float cy, float r)
{
    return hypotf(px - cx, py - cy) - r;
}

static float sd_segment(float px, float py, float ax, float ay, float bx, float by)
{
    float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    float t = clampf((wx * vx + wy * vy) / (vx * vx + vy * vy), 0, 1);
    return hypotf(wx - vx * t, wy - vy * t);
}

// Premultiplied "source over".
static void blend(Canvas *c, int x, int y, COLORREF col, float a)
{
    if (a <= 0) return;
    uint32_t *p = &c->px[y * c->w + x];
    float inv = 1 - a;
    float db = (float)(*p & 255), dg = (float)((*p >> 8) & 255), dr = (float)((*p >> 16) & 255), da = (float)(*p >> 24);
    float r = GetRValue(col) * a + dr * inv, g = GetGValue(col) * a + dg * inv, b = GetBValue(col) * a + db * inv;
    float al = 255 * a + da * inv;
    *p = ((uint32_t)(al + .5f) << 24) | ((uint32_t)(r + .5f) << 16) | ((uint32_t)(g + .5f) << 8) | (uint32_t)(b + .5f);
}

// Removes coverage (used to cut a gap around the slash in the tray icon).
static void erase(Canvas *c, int x, int y, float a)
{
    if (a <= 0) return;
    uint32_t *p = &c->px[y * c->w + x];
    float k = 1 - a;
    uint32_t b = (uint32_t)((*p & 255) * k), g = (uint32_t)(((*p >> 8) & 255) * k);
    uint32_t r = (uint32_t)(((*p >> 16) & 255) * k), al = (uint32_t)((*p >> 24) * k);
    *p = (al << 24) | (r << 16) | (g << 8) | b;
}

static BOOL light_taskbar(void)
{
    DWORD v = 0, sz = sizeof v;
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"SystemUsesLightTheme", RRF_RT_REG_DWORD, NULL, &v, &sz);
    return v != 0;
}

static HBITMAP make_dib(HDC dc, int w, int h, uint32_t **bits)
{
    BITMAPINFO bi = { 0 };
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    return CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)bits, NULL, 0);
}

// Tray icon: a small screen with a touch dot; crossed out when blocked.
static HICON make_tray_icon(BOOL blocked, int s)
{
    BOOL light = light_taskbar();
    COLORREF base   = light ? RGB(32, 32, 36)   : RGB(255, 255, 255);
    COLORREF accent = light ? RGB(124, 58, 237) : RGB(167, 139, 250);
    COLORREF red    = light ? RGB(220, 38, 38)  : RGB(248, 113, 113);

    uint32_t *bits;
    HDC sdc = GetDC(NULL);
    HBITMAP color = make_dib(sdc, s, s, &bits);
    ReleaseDC(NULL, sdc);
    if (!color) return NULL;
    Canvas c = { bits, s, s };
    ZeroMemory(bits, (size_t)s * s * 4);

    float k = (float)s, stroke = fmaxf(1.5f, k * 0.1f);
    for (int y = 0; y < s; y++)
        for (int x = 0; x < s; x++) {
            float px = x + .5f, py = y + .5f;
            float d = sd_rrect(px, py, .5f * k, .42f * k, .44f * k, .32f * k, .1f * k);
            blend(&c, x, y, base, clampf(stroke / 2 - fabsf(d) + .5f, 0, 1));
            d = sd_rrect(px, py, .5f * k, .88f * k, .2f * k, .05f * k, .05f * k);
            blend(&c, x, y, base, clampf(.5f - d, 0, 1));
            d = sd_circle(px, py, .5f * k, .42f * k, .12f * k);
            blend(&c, x, y, blocked ? base : accent, clampf(.5f - d, 0, 1));
            if (blocked) {
                d = sd_segment(px, py, .12f * k, .9f * k, .88f * k, .1f * k);
                erase(&c, x, y, clampf(stroke * 1.5f - d + .5f, 0, 1));
                blend(&c, x, y, red, clampf(stroke * .65f - d + .5f, 0, 1));
            }
        }

    // Icons want straight (not premultiplied) alpha.
    for (int i = 0; i < s * s; i++) {
        uint32_t p = bits[i], a = p >> 24;
        if (a && a < 255) {
            uint32_t r = ((p >> 16) & 255) * 255 / a, g = ((p >> 8) & 255) * 255 / a, b = (p & 255) * 255 / a;
            bits[i] = (a << 24) | (min(r, 255) << 16) | (min(g, 255) << 8) | min(b, 255);
        }
    }

    HBITMAP mask = CreateBitmap(s, s, 1, 1, NULL);
    ICONINFO ii = { TRUE, 0, 0, mask, color };
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return ic;
}

// Draws text with grayscale antialiasing and composites it with real alpha.
static void draw_text(Canvas *c, HDC dc, HFONT font, const wchar_t *s, RECT r, UINT fmt, COLORREF col)
{
    uint32_t *mask;
    HBITMAP bmp = make_dib(dc, c->w, c->h, &mask);
    if (!bmp) return;
    ZeroMemory(mask, (size_t)c->w * c->h * 4);
    HGDIOBJ ob = SelectObject(dc, bmp), of = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    DrawTextW(dc, s, -1, &r, fmt | DT_NOPREFIX);
    GdiFlush();
    for (int y = max(r.top, 0); y < min(r.bottom, c->h); y++)
        for (int x = max(r.left, 0); x < min(r.right, c->w); x++) {
            uint32_t m = mask[y * c->w + x];
            uint32_t v = max(max(m & 255, (m >> 8) & 255), (m >> 16) & 255);
            if (v) blend(c, x, y, col, v / 255.f);
        }
    SelectObject(dc, of);
    SelectObject(dc, ob);
    DeleteObject(bmp);
}

static HFONT make_font(const wchar_t *face, int px, int weight)
{
    return CreateFontW(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       ANTIALIASED_QUALITY, DEFAULT_PITCH, face);
}

static BOOL font_exists(const wchar_t *face)
{
    HDC dc = GetDC(NULL);
    HFONT f = make_font(face, 12, FW_NORMAL);
    HGDIOBJ o = SelectObject(dc, f);
    wchar_t got[LF_FACESIZE] = { 0 };
    GetTextFaceW(dc, LF_FACESIZE, got);
    SelectObject(dc, o);
    DeleteObject(f);
    ReleaseDC(NULL, dc);
    return _wcsicmp(got, face) == 0;
}

// A rendered card (pill with a round badge, title and subtitle), ready for UpdateLayeredWindow.
typedef struct { HDC dc; HBITMAP bmp; SIZE size; POINT pos; } Card;

static void card_free(Card *k)
{
    if (k->dc) { DeleteDC(k->dc); DeleteObject(k->bmp); k->dc = NULL; k->bmp = NULL; }
}

static void card_render(Card *k, const wchar_t *title, const wchar_t *sub, const wchar_t *glyph, COLORREF badge)
{
    POINT cur; GetCursorPos(&cur);
    HMONITOR mon = MonitorFromPoint(cur, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof mi }; GetMonitorInfoW(mon, &mi);
    UINT dpx = 96, dpy = 96; GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpx, &dpy);
    float sc = dpx / 96.f;
#ifdef PREVIEW_SCALE
    sc = PREVIEW_SCALE;   // dev screenshots only (tools/preview.c)
#endif

    static const wchar_t *uiFace, *icFace;
    if (!uiFace) {
        uiFace = font_exists(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
        icFace = font_exists(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    }
    HFONT fTitle = make_font(uiFace, (int)(15 * sc + .5f), FW_SEMIBOLD);
    HFONT fSub   = make_font(uiFace, (int)(12 * sc + .5f), FW_NORMAL);
    HFONT fIcon  = make_font(icFace, (int)(17 * sc + .5f), FW_NORMAL);

    HDC sdc = GetDC(NULL);
    HDC dc = CreateCompatibleDC(sdc);
    ReleaseDC(NULL, sdc);

    SIZE st, ss;
    HGDIOBJ oldFont = SelectObject(dc, fTitle);
    GetTextExtentPoint32W(dc, title, (int)wcslen(title), &st);
    SelectObject(dc, fSub);
    GetTextExtentPoint32W(dc, sub, (int)wcslen(sub), &ss);
    SelectObject(dc, oldFont);   // a font still selected can't be deleted (GDI leak)

    int m   = (int)(16 * sc);            // room for the shadow
    int ph  = (int)(60 * sc);            // pill height
    int bd  = (int)(36 * sc);            // badge diameter
    int pad = (int)(12 * sc);
    int tx  = pad + bd + (int)(12 * sc);
    int pw  = tx + max(st.cx, ss.cx) + (int)(22 * sc);
    int W = pw + 2 * m, H = ph + 2 * m;

    uint32_t *bits;
    HBITMAP bmp = make_dib(dc, W, H, &bits);
    ZeroMemory(bits, (size_t)W * H * 4);
    Canvas c = { bits, W, H };

    float cx = m + pw / 2.f, cy = m + ph / 2.f, r = ph / 2.f;
    float bcx = m + pad + bd / 2.f;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            float px = x + .5f, py = y + .5f;
            // soft shadow
            float ds = sd_rrect(px, py - 3 * sc, cx, cy, pw / 2.f, ph / 2.f, r);
            float sh = 1 - clampf(ds / (m - 2.f), 0, 1);
            blend(&c, x, y, RGB(0, 0, 0), .32f * sh * sh);
            // body + hairline border
            float d = sd_rrect(px, py, cx, cy, pw / 2.f, ph / 2.f, r);
            blend(&c, x, y, RGB(30, 30, 34), .95f * clampf(.5f - d, 0, 1));
            blend(&c, x, y, RGB(255, 255, 255), .09f * clampf(1 - fabsf(d + sc * .75f) / sc, 0, 1));
            // badge
            d = sd_circle(px, py, bcx, cy, bd / 2.f);
            blend(&c, x, y, badge, clampf(.5f - d, 0, 1));
        }

    RECT rb = { (LONG)(bcx - bd / 2.f), (LONG)(cy - bd / 2.f), (LONG)(bcx + bd / 2.f), (LONG)(cy + bd / 2.f) };
    draw_text(&c, dc, fIcon, glyph, rb, DT_CENTER | DT_VCENTER | DT_SINGLELINE, RGB(255, 255, 255));
    int gap = (int)(1 * sc), top = (int)(cy - (st.cy + ss.cy + gap) / 2.f);
    RECT rt = { m + tx, top, W, top + st.cy };
    draw_text(&c, dc, fTitle, title, rt, DT_LEFT | DT_SINGLELINE, RGB(255, 255, 255));
    RECT rs = { m + tx, top + st.cy + gap, W, top + st.cy + gap + ss.cy };
    draw_text(&c, dc, fSub, sub, rs, DT_LEFT | DT_SINGLELINE, RGB(168, 168, 178));

    DeleteObject(fTitle); DeleteObject(fSub); DeleteObject(fIcon);

    card_free(k);
    SelectObject(dc, bmp);
    k->dc = dc;
    k->bmp = bmp;
    k->size.cx = W; k->size.cy = H;
    k->pos.x = (mi.rcWork.left + mi.rcWork.right - W) / 2;
    k->pos.y = mi.rcWork.bottom - H - (int)(40 * sc) + m;
}

static void card_apply(HWND w, Card *k, BYTE alpha, int lift)
{
    POINT src = { 0, 0 }, pos = { k->pos.x, k->pos.y + lift };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA };
    UpdateLayeredWindow(w, NULL, &pos, &k->size, k->dc, &src, 0, &bf, ULW_ALPHA);
}

// ---------------------------------------------------------------- on-screen notice

static Card      g_osdCard;
static ULONGLONG g_osdStart;

#define OSD_IN    140
#define OSD_HOLD  1500
#define OSD_OUT   280

static void osd_card(const wchar_t *title, const wchar_t *sub, const wchar_t *glyph, COLORREF badge)
{
    card_render(&g_osdCard, title, sub, glyph, badge);
    g_osdStart = GetTickCount64();
    card_apply(g_osd, &g_osdCard, 0, 0);
    ShowWindow(g_osd, SW_SHOWNOACTIVATE);
    SetWindowPos(g_osd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetTimer(g_osd, ID_ANIM, 15, NULL);
}

static void osd_show(int state, BOOL error)
{
    wchar_t sub[128];
    if (error) {
        osd_card(TR(L"Не получилось", L"Something went wrong"),
                 TR(L"Сенсорный экран не найден", L"Touch screen not found"), GLYPH_ERROR, COL_GRAY);
    } else if (state == ST_BLOCKED) {
        swprintf(sub, 128, TR(L"%s — включить", L"%s to unlock"), g_hkName);
        osd_card(TR(L"Сенсор заблокирован", L"Touch screen locked"), sub, GLYPH_LOCK, COL_RED);
    } else {
        swprintf(sub, 128, TR(L"%s — выключить", L"%s to lock"), g_hkName);
        osd_card(TR(L"Сенсор разблокирован", L"Touch screen unlocked"), sub, GLYPH_UNLOCK, COL_VIOLET);
    }
}

// Drops the notice bitmap and hands unused pages back to Windows while idle.
static void osd_free(void)
{
    card_free(&g_osdCard);
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

static LRESULT CALLBACK osd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_TIMER && wp == ID_ANIM) {
        float t = (float)(GetTickCount64() - g_osdStart), a;
        if (t < OSD_IN) a = t / OSD_IN;
        else if (t < OSD_IN + OSD_HOLD) a = 1;
        else if (t < OSD_IN + OSD_HOLD + OSD_OUT) a = 1 - (t - OSD_IN - OSD_HOLD) / OSD_OUT;
        else {
            KillTimer(h, ID_ANIM);
            ShowWindow(h, SW_HIDE);
            osd_free();
            return 0;
        }
        float e = 1 - (1 - a) * (1 - a) * (1 - a);   // ease-out
        card_apply(h, &g_osdCard, (BYTE)(255 * e + .5f), (int)((1 - e) * 8));
        return 0;
    }
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, msg, wp, lp);
}

// ---------------------------------------------------------------- hotkey capture window
// A card like the notice that waits for the new key combination.

static Card g_capCard;

static void capture_draw(const wchar_t *sub)
{
    card_render(&g_capCard, TR(L"Нажми новые клавиши", L"Press the new hotkey"), sub, GLYPH_KEYBOARD, COL_VIOLET);
    card_apply(g_cap, &g_capCard, 255, 0);
}

static void capture_hint(void)
{
    wchar_t def[64], sub[160];
    hotkey_format(DEFAULT_VK, 0, def, ARRAYSIZE(def));
    swprintf(sub, 160, TR(L"Esc — отмена   Backspace — вернуть %s", L"Esc — cancel   Backspace — reset to %s"), def);
    capture_draw(sub);
}

static void capture_start(void)
{
    if (g_cap) return;
    UnregisterHotKey(g_main, ID_HOTKEY);   // so the current combo can be picked again
    g_cap = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"SCSSB.Cap", APP_NAME, WS_POPUP,
                            0, 0, 0, 0, NULL, NULL, g_inst, NULL);
    if (!g_cap) { hotkey_register(); return; }
    capture_hint();
    ShowWindow(g_cap, SW_SHOW);
    SetForegroundWindow(g_cap);
    SetFocus(g_cap);
}

static void capture_finish(BOOL apply, UINT vk, UINT mods)
{
    if (!g_cap) return;
    HWND w = g_cap;
    g_cap = NULL;
    DestroyWindow(w);
    card_free(&g_capCard);

    if (!apply) { hotkey_register(); return; }
    UINT oldVk = g_hkVk, oldMods = g_hkMods;
    g_hkVk = vk; g_hkMods = mods;
    if (hotkey_register()) {
        hotkey_save();
        osd_card(TR(L"Клавиши сохранены", L"Hotkey saved"), g_hkName, GLYPH_KEYBOARD, COL_VIOLET);
    } else {
        wchar_t name[64], title[128];
        hotkey_format(vk, mods, name, ARRAYSIZE(name));
        swprintf(title, 128, TR(L"%s уже заняты", L"%s is already taken"), name);
        g_hkVk = oldVk; g_hkMods = oldMods;
        hotkey_register();
        osd_card(title, TR(L"Выбери другие клавиши", L"Pick another hotkey"), GLYPH_ERROR, COL_GRAY);
    }
}

static LRESULT CALLBACK cap_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        UINT vk = (UINT)wp;
        if (is_modifier(vk)) return 0;
        UINT mods = 0;
        if (GetKeyState(VK_CONTROL) < 0) mods |= MOD_CONTROL;
        if (GetKeyState(VK_MENU) < 0)    mods |= MOD_ALT;
        if (GetKeyState(VK_SHIFT) < 0)   mods |= MOD_SHIFT;
        if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) mods |= MOD_WIN;
        if (!mods && vk == VK_ESCAPE) capture_finish(FALSE, 0, 0);
        else if (!mods && vk == VK_BACK) capture_finish(TRUE, DEFAULT_VK, 0);
        else if (!(mods & (MOD_CONTROL | MOD_ALT | MOD_WIN)) && !standalone_ok(vk))
            capture_draw(TR(L"Добавь Ctrl или Alt, чтобы не мешать набору текста",
                            L"Add Ctrl or Alt so it doesn't get in the way of typing"));
        else capture_finish(TRUE, vk, mods);
        return 0;
    }
    case WM_CHAR: case WM_SYSCHAR: case WM_KEYUP: case WM_SYSKEYUP:
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_KEYMENU) return 0;
        break;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) PostMessageW(g_main, WM_CAPCANCEL, 0, 0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---------------------------------------------------------------- tray

static BOOL tray_update(DWORD action)
{
    NOTIFYICONDATAW n = { sizeof n };
    n.hWnd = g_main;
    n.uID = 1;
    n.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    n.uCallbackMessage = WM_TRAY;
    n.hIcon = g_state == ST_BLOCKED ? g_iconOff : g_iconOn;
    swprintf(n.szTip, ARRAYSIZE(n.szTip), L"%s\n%s  (%s)", APP_NAME,
             g_state == ST_BLOCKED ? TR(L"Сенсор заблокирован", L"Touch screen locked") :
             g_state == ST_ON ? TR(L"Сенсор включён", L"Touch screen on") :
                                TR(L"Сенсорный экран не найден", L"Touch screen not found"),
             g_hkName);
    return Shell_NotifyIconW(action, &n);
}

static void load_icons(void)
{
    if (g_iconOn) DestroyIcon(g_iconOn);
    if (g_iconOff) DestroyIcon(g_iconOff);
    UINT dpi = GetDpiForSystem();
    int s = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
    g_iconOn = make_tray_icon(FALSE, s);
    g_iconOff = make_tray_icon(TRUE, s);
}

static void tray_menu(void)
{
    wchar_t toggle[96], keys[96];
    swprintf(toggle, 96, L"%s\t%s", g_state == ST_BLOCKED ? TR(L"Разблокировать сенсор", L"Unlock touch screen")
                                                          : TR(L"Заблокировать сенсор", L"Lock touch screen"), g_hkName);
    swprintf(keys, 96, TR(L"Сменить клавиши…\t%s", L"Change hotkey…\t%s"), g_hkName);

    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_DISABLED, 0, APP_NAME);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, CMD_TOGGLE, toggle);
    AppendMenuW(m, MF_STRING, CMD_HOTKEY, keys);
    AppendMenuW(m, MF_STRING | (get_unblock_option() ? MF_CHECKED : 0), CMD_RESTARTOPT,
                TR(L"Включать сенсор при перезагрузке", L"Turn touch on when restarting"));
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, CMD_EXIT, TR(L"Выход", L"Exit"));
    POINT p; GetCursorPos(&p);
    SetForegroundWindow(g_main);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, p.x, p.y, 0, g_main, NULL);
    PostMessageW(g_main, WM_NULL, 0, 0);
    DestroyMenu(m);
}

// ---------------------------------------------------------------- restart handling

// Shutdown/restart/sign-out: turn the sensor on for the sign-in screen, remember to re-block.
static void prepare_for_shutdown(void)
{
    if (!get_unblock_option()) return;
    while (g_busy) Sleep(20);
    if (query_state() == ST_BLOCKED) {
        reg_set(REG_REBLOCK, 1);
        set_blocked(FALSE);
    }
}

// Startup: if it was blocked before the last shutdown, block it again.
static void restore_after_logon(void)
{
    if (!reg_get(REG_REBLOCK, 0)) return;
    reg_del(REG_REBLOCK);
    if (g_state == ST_ON) {
        int r = set_blocked(TRUE);
        if (r != ST_UNKNOWN) g_state = r;
        if (g_state == ST_BLOCKED && g_osd) osd_show(ST_BLOCKED, FALSE);
    }
}

// ---------------------------------------------------------------- main window

static LRESULT CALLBACK main_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_HOTKEY:
        if (wp == ID_HOTKEY) request_toggle();
        return 0;
    case WM_TOGGLED:
        g_state = (int)wp;
        tray_update(NIM_MODIFY);
        if (lp != 0 || g_state != g_shown) osd_show(g_state, lp != 0);
        return 0;
    case WM_CAPCANCEL:
        capture_finish(FALSE, 0, 0);
        return 0;
    case WM_TRAY:
        if (lp == WM_LBUTTONUP) request_toggle();
        else if (lp == WM_RBUTTONUP) tray_menu();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case CMD_TOGGLE:     request_toggle(); break;
        case CMD_HOTKEY:     capture_start(); break;
        case CMD_RESTARTOPT: reg_set(REG_OPT, !get_unblock_option()); break;
        case CMD_EXIT:       DestroyWindow(h); break;
        }
        return 0;
    case WM_DEVICECHANGE:
        // Device list changed (Device Manager, sleep/resume...): resync the tray a bit later.
        if (wp == DBT_DEVNODES_CHANGED) SetTimer(h, ID_RESYNC, 700, NULL);
        return TRUE;
    case WM_TIMER:
        if (wp == ID_RESYNC && !g_busy) {
            KillTimer(h, ID_RESYNC);
            int s = query_state();
            if (s != g_state) { g_state = s; tray_update(NIM_MODIFY); }
        } else if (wp == ID_TRAYRETRY) {
            // Explorer may not be ready yet right after logon.
            if (tray_update(NIM_ADD) || ++g_trayTries > 30) KillTimer(h, ID_TRAYRETRY);
        }
        return 0;
    case WM_SETTINGCHANGE:
        if (lp && wcscmp((const wchar_t *)lp, L"ImmersiveColorSet") == 0) { load_icons(); tray_update(NIM_MODIFY); }
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        // Shutdown, restart or sign-out is really happening (not cancelled).
        if (wp) prepare_for_shutdown();
        return 0;
    case WM_DESTROY:
        // Never leave the user without touch after quitting from the menu.
        while (g_busy) Sleep(50);
        if (query_state() == ST_BLOCKED) set_blocked(FALSE);
        tray_update(NIM_DELETE);
        UnregisterHotKey(h, ID_HOTKEY);
        PostQuitMessage(0);
        return 0;
    }
    if (msg == g_taskbarCreated && msg) { load_icons(); tray_update(NIM_ADD); return 0; }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    (void)prev; (void)cmd; (void)show;
    g_inst = inst;

    HANDLE mutex = CreateMutexW(NULL, TRUE, L"Local\\StarsCliqueScreenSensorBlocker");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW wc = { 0 };
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpfnWndProc = main_proc;
    wc.lpszClassName = L"SCSSB.Main";
    RegisterClassW(&wc);
    wc.lpfnWndProc = osd_proc;
    wc.lpszClassName = L"SCSSB.Osd";
    RegisterClassW(&wc);
    wc.lpfnWndProc = cap_proc;
    wc.lpszClassName = L"SCSSB.Cap";
    RegisterClassW(&wc);

    g_main = CreateWindowExW(WS_EX_TOOLWINDOW, L"SCSSB.Main", APP_NAME, WS_POPUP, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    g_osd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                            L"SCSSB.Osd", APP_NAME, WS_POPUP, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    // We run elevated; let Explorer (normal rights) reach us with tray clicks and restarts.
    ChangeWindowMessageFilterEx(g_main, g_taskbarCreated, MSGFLT_ALLOW, NULL);
    ChangeWindowMessageFilterEx(g_main, WM_TRAY, MSGFLT_ALLOW, NULL);

    hotkey_load();
    g_state = query_state();
    restore_after_logon();
    load_icons();
    if (!tray_update(NIM_ADD)) SetTimer(g_main, ID_TRAYRETRY, 2000, NULL);

    if (!hotkey_register()) {
        wchar_t title[128];
        swprintf(title, 128, TR(L"%s уже заняты", L"%s is already taken"), g_hkName);
        osd_card(title, TR(L"Смени клавиши в меню значка в трее", L"Change the hotkey from the tray icon menu"),
                 GLYPH_ERROR, COL_GRAY);
    }

    // Windows 11 "efficiency mode": the process sleeps all the time anyway.
    PROCESS_POWER_THROTTLING_STATE pt = { PROCESS_POWER_THROTTLING_CURRENT_VERSION,
        PROCESS_POWER_THROTTLING_EXECUTION_SPEED, PROCESS_POWER_THROTTLING_EXECUTION_SPEED };
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof pt);
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (mutex) CloseHandle(mutex);
    return 0;
}
