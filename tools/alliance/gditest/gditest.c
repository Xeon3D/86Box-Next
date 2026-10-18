/* gditest -- draws a fixed GDI test picture (filled polygons, ellipses,
   hatched rectangles, a fan of lines, text) for checking a display
   driver's acceleration against a reference. Build:
   i686-w64-mingw32-gcc -O2 -mwindows -nostdlib -fno-builtin -Wl,-e,_entry@0
     -o GDITEST.EXE gditest.c -lgdi32 -luser32 -lkernel32   (in WSL) */
#include <windows.h>
/* No C runtime: Windows 98's msvcrt lacks what mingw-w64's start-up needs. */
void *
memset(void *d, int c, unsigned int n)
{
    unsigned char *p = d;

    while (n--)
        *p++ = (unsigned char) c;
    return d;
}


static void
paint(HDC dc, RECT *rc)
{
    static const COLORREF cols[6] = { RGB(255, 0, 0), RGB(0, 160, 0), RGB(0, 0, 255),
                                      RGB(255, 200, 0), RGB(200, 0, 200), RGB(0, 180, 180) };
    POINT  tri[3]  = { { 20, 150 }, { 120, 20 }, { 200, 170 } };
    static const POINT star[10] = { { 440, 25 }, { 457, 76 }, { 511, 77 }, { 468, 109 }, { 484, 160 }, { 440, 130 }, { 396, 160 }, { 412, 109 }, { 369, 77 }, { 423, 76 } };
    static const POINT fan[48] = { { 560, 290 }, { 559, 301 }, { 556, 313 }, { 553, 324 }, { 547, 334 }, { 541, 344 }, { 533, 353 }, { 524, 361 }, { 515, 367 }, { 504, 373 }, { 493, 376 }, { 481, 379 }, { 470, 380 }, { 459, 379 }, { 447, 376 }, { 436, 373 }, { 426, 367 }, { 416, 361 }, { 407, 353 }, { 399, 344 }, { 393, 334 }, { 387, 324 }, { 384, 313 }, { 381, 301 }, { 380, 290 }, { 381, 279 }, { 384, 267 }, { 387, 256 }, { 393, 246 }, { 399, 236 }, { 407, 227 }, { 416, 219 }, { 425, 213 }, { 436, 207 }, { 447, 204 }, { 459, 201 }, { 470, 200 }, { 481, 201 }, { 493, 204 }, { 504, 207 }, { 515, 213 }, { 524, 219 }, { 533, 227 }, { 541, 236 }, { 547, 245 }, { 553, 256 }, { 556, 267 }, { 559, 279 } };
    POINT  quad[4] = { { 230, 30 }, { 330, 50 }, { 310, 160 }, { 220, 120 } };
    HBRUSH b;
    HPEN   pen;
    int    i;

    FillRect(dc, rc, (HBRUSH) GetStockObject(WHITE_BRUSH));

    /* Convex polygons, solid brushes, no outline. */
    SelectObject(dc, GetStockObject(NULL_PEN));
    b = CreateSolidBrush(cols[0]);
    SelectObject(dc, b);
    Polygon(dc, tri, 3);
    SelectObject(dc, GetStockObject(WHITE_BRUSH));
    DeleteObject(b);
    b = CreateSolidBrush(cols[2]);
    SelectObject(dc, b);
    Polygon(dc, quad, 4);
    SelectObject(dc, GetStockObject(WHITE_BRUSH));
    DeleteObject(b);

    /* A star (not convex), alternate fill. */
    b = CreateSolidBrush(cols[4]);
    SelectObject(dc, b);
    SetPolyFillMode(dc, ALTERNATE);
    Polygon(dc, star, 10);
    SelectObject(dc, GetStockObject(WHITE_BRUSH));
    DeleteObject(b);

    /* Ellipses: solid, and hatched with an outline. */
    b = CreateSolidBrush(cols[3]);
    SelectObject(dc, b);
    Ellipse(dc, 20, 200, 180, 300);
    SelectObject(dc, GetStockObject(WHITE_BRUSH));
    DeleteObject(b);
    pen = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
    SelectObject(dc, pen);
    b = CreateHatchBrush(HS_DIAGCROSS, cols[5]);
    SelectObject(dc, b);
    Ellipse(dc, 200, 200, 340, 300);
    SelectObject(dc, GetStockObject(WHITE_BRUSH));
    DeleteObject(b);

    /* Hatched rectangles, all six styles. */
    for (i = 0; i < 6; i++) {
        b = CreateHatchBrush(i, cols[i]);
        SelectObject(dc, b);
        Rectangle(dc, 20 + i * 55, 320, 70 + i * 55, 370);
        SelectObject(dc, GetStockObject(WHITE_BRUSH));
        DeleteObject(b);
    }

    /* A fan of lines in every direction. */
    for (i = 0; i < 48; i++) {
        MoveToEx(dc, 470, 290, NULL);
        LineTo(dc, fan[i].x, fan[i].y);
    }
    SelectObject(dc, GetStockObject(BLACK_PEN));
    DeleteObject(pen);

    SetBkMode(dc, TRANSPARENT);
    TextOut(dc, 20, 385, "GDITEST: polygons, star, ellipses, hatches, line fan", 52);
}

static LRESULT CALLBACK
wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    PAINTSTRUCT ps;
    RECT        rc;

    switch (m) {
        case WM_PAINT:
            BeginPaint(w, &ps);
            GetClientRect(w, &rc);
            paint(ps.hdc, &rc);
            EndPaint(w, &ps);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProc(w, m, wp, lp);
    }
}

int WINAPI
WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASS wc = { 0 };
    MSG      msg;
    HWND     w;

    (void) prev;
    (void) cmd;
    wc.lpfnWndProc   = wndproc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "gditest";
    RegisterClass(&wc);
    w = CreateWindow("gditest", "GDI test", WS_OVERLAPPEDWINDOW, 0, 0, 600, 450, NULL, NULL, inst, NULL);
    ShowWindow(w, show);
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}

void __stdcall
entry(void)
{
    ExitProcess(WinMain(GetModuleHandle(NULL), NULL, NULL, SW_SHOWNORMAL));
}
