/*
 * icons.c -- a Windows 9x test of icon drawing for the NV4 emulation:
 * the shell's icons and the shortcut overlay, drawn as Explorer draws
 * them (image lists with masks and overlays) and with DrawIconEx.
 *
 * Rows: DrawIconEx 32x32, DrawIconEx 16x16, then image lists of 32x32
 * icons made with ILC_COLORDDB, ILC_COLOR4, ILC_COLOR8, ILC_COLOR24 and
 * ILC_COLOR32 (all with ILC_MASK), each icon drawn normally, with the
 * shortcut overlay, and with ILD_SELECTED. The icons are shell32.dll's
 * 0 (document), 3 (folder), 15 (computer) and 29 (the shortcut arrow).
 *
 *   icons        draw on the screen (the display driver does it)
 *   icons ref    draw into a 32 bpp DIB section first (GDI's own
 *                engine), then copy that 1:1 to the screen
 *
 * Any key or click ends it. Built without the C runtime (build.sh).
 */
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>

#define NICONS 4
static const int icon_idx[NICONS] = { 0, 3, 15, 29 };
static HICON     icon_big[NICONS];
static HICON     icon_small[NICONS];
static int       ref_mode;

static const UINT list_flags[5] = { ILC_COLORDDB, ILC_COLOR4, ILC_COLOR8, ILC_COLOR24, ILC_COLOR32 };
static const char *list_name[5] = { "ILC_COLORDDB", "ILC_COLOR4", "ILC_COLOR8", "ILC_COLOR24", "ILC_COLOR32" };

void *
memset(void *d, int c, size_t n)
{
    BYTE *p = d;

    while (n--)
        *p++ = (BYTE) c;
    return d;
}

static void
label(HDC dc, int x, int y, const char *s)
{
    TextOut(dc, x, y, s, lstrlen(s));
}

static void
draw_all(HDC dc)
{
    int y = 8;

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0xff, 0xff, 0xff));

    label(dc, 8, y, "DrawIconEx 32x32 / 16x16");
    for (int i = 0; i < NICONS; i++) {
        DrawIconEx(dc, 220 + i * 48, y, icon_big[i], 32, 32, 0, NULL, DI_NORMAL);
        DrawIconEx(dc, 220 + NICONS * 48 + 16 + i * 24, y, icon_small[i], 16, 16, 0, NULL, DI_NORMAL);
    }
    y += 48;

    for (int l = 0; l < 5; l++) {
        HIMAGELIST il = ImageList_Create(32, 32, list_flags[l] | ILC_MASK, NICONS, 0);

        label(dc, 8, y + 8, list_name[l]);
        for (int i = 0; i < NICONS; i++)
            ImageList_AddIcon(il, icon_big[i]);
        ImageList_SetOverlayImage(il, 3, 1);
        for (int i = 0; i < 3; i++) {
            ImageList_Draw(il, i, dc, 220 + i * 48, y, ILD_NORMAL);
            ImageList_Draw(il, i, dc, 380 + i * 48, y, ILD_TRANSPARENT | INDEXTOOVERLAYMASK(1));
            ImageList_Draw(il, i, dc, 540 + i * 48, y, ILD_SELECTED | INDEXTOOVERLAYMASK(1));
        }
        ImageList_Draw(il, 3, dc, 700, y, ILD_TRANSPARENT);
        ImageList_Destroy(il);
        y += 48;
    }
    label(dc, 220, y, "normal");
    label(dc, 380, y, "with overlay");
    label(dc, 540, y, "selected + overlay");
    label(dc, 700, y, "arrow alone");
}

static LRESULT CALLBACK
wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC         dc = BeginPaint(w, &ps);
            RECT        r;

            GetClientRect(w, &r);
            if (ref_mode) {
                BITMAPINFOHEADER h;
                void   *b;
                HDC     mdc = CreateCompatibleDC(dc);
                HBITMAP dib;

                memset(&h, 0, sizeof(h));
                h.biSize     = sizeof(h);
                h.biWidth    = r.right;
                h.biHeight   = -r.bottom;
                h.biPlanes   = 1;
                h.biBitCount = 32;
                dib = CreateDIBSection(dc, (BITMAPINFO *) &h, DIB_RGB_COLORS, &b, NULL, 0);
                SelectObject(mdc, dib);
                FillRect(mdc, &r, CreateSolidBrush(RGB(0, 0x80, 0x80)));
                draw_all(mdc);
                BitBlt(dc, 0, 0, r.right, r.bottom, mdc, 0, 0, SRCCOPY);
                DeleteDC(mdc);
                DeleteObject(dib);
            } else {
                FillRect(dc, &r, CreateSolidBrush(RGB(0, 0x80, 0x80)));
                draw_all(dc);
            }
            EndPaint(w, &ps);
            return 0;
        }
        case WM_KEYDOWN:
        case WM_LBUTTONDOWN:
            DestroyWindow(w);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(w, m, wp, lp);
}

void
entry(void)
{
    const char *cmd = GetCommandLine();
    WNDCLASS    wc;
    MSG         msg;
    HINSTANCE   inst = GetModuleHandle(NULL);

    if (*cmd == '"') {
        for (cmd++; *cmd && (*cmd != '"'); cmd++)
            ;
    }
    while (*cmd && (*cmd != ' '))
        cmd++;
    for (; *cmd; cmd++)
        if (((cmd[0] | 0x20) == 'r') && ((cmd[1] | 0x20) == 'e') && ((cmd[2] | 0x20) == 'f'))
            ref_mode = 1;

    InitCommonControls();
    for (int i = 0; i < NICONS; i++)
        ExtractIconEx("shell32.dll", icon_idx[i], &icon_big[i], &icon_small[i], 1);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = wndproc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "nv4icons";
    RegisterClass(&wc);
    CreateWindow("nv4icons", "icons", WS_POPUP | WS_VISIBLE, 0, 0, GetSystemMetrics(SM_CXSCREEN),
                 GetSystemMetrics(SM_CYSCREEN), NULL, NULL, inst, NULL);
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    ExitProcess(0);
}
