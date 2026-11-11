// Dev-only: renders every on-screen card and the tray icons to BMP files
// (no device access, no admin). Used for visual checks and README screenshots.
// Build: tools\build_preview.bat
#define wWinMain real_wWinMain
#include "../src/main.c"
#undef wWinMain

static void save_bmp(const wchar_t *path, uint32_t *px, int w, int h)
{
    BITMAPFILEHEADER fh = { 0x4D42 };
    BITMAPINFOHEADER ih = { sizeof ih, w, -h, 1, 32 };
    DWORD n = (DWORD)w * h * 4, wr;
    fh.bfOffBits = sizeof fh + sizeof ih;
    fh.bfSize = fh.bfOffBits + n;
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    WriteFile(f, &fh, sizeof fh, &wr, NULL);
    WriteFile(f, &ih, sizeof ih, &wr, NULL);
    WriteFile(f, px, n, &wr, NULL);
    CloseHandle(f);
}

static void dump(const wchar_t *dir, const wchar_t *name, const wchar_t *t, const wchar_t *s, const wchar_t *g, COLORREF c)
{
    Card k = { 0 };
    card_render(&k, t, s, g, c);
    BITMAP b; GetObject(k.bmp, sizeof b, &b);
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%s/%s.bmp", dir, name);
    save_bmp(path, (uint32_t *)b.bmBits, b.bmWidth, b.bmHeight);
    card_free(&k);
}

static void dump_icon(const wchar_t *dir, const wchar_t *name, BOOL blocked, int s)
{
    HICON ic = make_tray_icon(blocked, s);
    ICONINFO ii; GetIconInfo(ic, &ii);
    uint32_t *buf = malloc((size_t)s * s * 4);
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), s, -s, 1, 32 } };
    HDC dc = GetDC(NULL);
    GetDIBits(dc, ii.hbmColor, 0, s, buf, &bi, DIB_RGB_COLORS);
    ReleaseDC(NULL, dc);
    for (int i = 0; i < s * s; i++) {   // to premultiplied, like the cards
        uint32_t p = buf[i], a = p >> 24;
        buf[i] = (a << 24) | ((((p >> 16) & 255) * a / 255) << 16) | ((((p >> 8) & 255) * a / 255) << 8) | ((p & 255) * a / 255);
    }
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%s/%s.bmp", dir, name);
    save_bmp(path, buf, s, s);
    free(buf);
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2) return 2;
    const wchar_t *d = argv[1];
    hotkey_format(DEFAULT_VK, 0, g_hkName, ARRAYSIZE(g_hkName));
    wchar_t sub[160], def[64];
    swprintf(sub, 160, TR(L"%s — включить", L"%s to unlock"), g_hkName);
    dump(d, L"locked", TR(L"Сенсор заблокирован", L"Touch screen locked"), sub, GLYPH_LOCK, COL_RED);
    swprintf(sub, 160, TR(L"%s — выключить", L"%s to lock"), g_hkName);
    dump(d, L"unlocked", TR(L"Сенсор разблокирован", L"Touch screen unlocked"), sub, GLYPH_UNLOCK, COL_VIOLET);
    dump(d, L"error", TR(L"Не получилось", L"Something went wrong"),
         TR(L"Сенсорный экран не найден", L"Touch screen not found"), GLYPH_ERROR, COL_GRAY);
    hotkey_format(DEFAULT_VK, 0, def, ARRAYSIZE(def));
    swprintf(sub, 160, TR(L"Esc — отмена   Backspace — вернуть %s", L"Esc — cancel   Backspace — reset to %s"), def);
    dump(d, L"capture", TR(L"Нажми новые клавиши", L"Press the new hotkey"), sub, GLYPH_KEYBOARD, COL_VIOLET);
    dump(d, L"capture_hint", TR(L"Нажми новые клавиши", L"Press the new hotkey"),
         TR(L"Добавь Ctrl или Alt, чтобы не мешать набору текста", L"Add Ctrl or Alt so it doesn't get in the way of typing"),
         GLYPH_KEYBOARD, COL_VIOLET);
    dump(d, L"saved", TR(L"Клавиши сохранены", L"Hotkey saved"), L"Ctrl + Alt + T", GLYPH_KEYBOARD, COL_VIOLET);
    dump_icon(d, L"tray_on", FALSE, 64);
    dump_icon(d, L"tray_off", TRUE, 64);
    return 0;
}
