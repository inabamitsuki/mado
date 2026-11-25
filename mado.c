#include <windows.h>
#include <dwmapi.h>
#include <commdlg.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

#define PAD 40
#define WM_SYNCSIZE (WM_APP + 1)

enum { ID_TITLE = 101, ID_FILE, ID_RATIO, ID_W, ID_H, ID_APPLY, ID_CAP, ID_STATUS, ID_ICON, ID_ICONBROWSE, ID_ICONRESET, ID_NOICON };

typedef struct { const char *name; int rw, rh; } Ratio;
static const Ratio g_ratios[] = {
    {"Free", 0, 0}, {"16:9", 16, 9}, {"16:10", 16, 10}, {"4:3", 4, 3},
    {"3:2", 3, 2}, {"1:1", 1, 1}, {"21:9", 21, 9}, {"9:16", 9, 16}, {"3:4", 3, 4}
};
#define NRATIOS ((int)(sizeof(g_ratios) / sizeof(g_ratios[0])))

static HINSTANCE g_hInst;
static HWND g_glass = NULL, g_panel = NULL;
static COLORREF g_backColor = RGB(0, 0, 0);
static int g_ratioIdx = 0;
static HICON g_iconBig = NULL, g_iconSmall = NULL;

static LRESULT CALLBACK BackProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_ERASEBKGND)
    {
        RECT rc;
        GetClientRect(h, &rc);
        HBRUSH b = CreateSolidBrush(g_backColor);
        FillRect((HDC)w, &rc, b);
        DeleteObject(b);
        return 1;
    }
    return DefWindowProc(h, m, w, l);
}

static BYTE *GrabScreen(RECT rc)
{
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    HDC hs = GetDC(NULL);
    HDC hm = CreateCompatibleDC(hs);
    HBITMAP hb = CreateCompatibleBitmap(hs, w, h);
    HGDIOBJ old = SelectObject(hm, hb);
    BitBlt(hm, 0, 0, w, h, hs, rc.left, rc.top, SRCCOPY | CAPTUREBLT);
    SelectObject(hm, old);

    BITMAPINFOHEADER bi = {0};
    bi.biSize = sizeof(bi);
    bi.biWidth = w;
    bi.biHeight = -h;
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;

    BYTE *px = (BYTE *)malloc((size_t)w * h * 4);
    GetDIBits(hm, hb, 0, h, px, (BITMAPINFO *)&bi, DIB_RGB_COLORS);
    DeleteObject(hb);
    DeleteDC(hm);
    ReleaseDC(NULL, hs);
    return px;
}

static unsigned long crcTable[256];
static int crcReady = 0;

static void InitCrc(void)
{
    unsigned long c; int n, k;
    for (n = 0; n < 256; n++)
    {
        c = (unsigned long)n;
        for (k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1);
        crcTable[n] = c;
    }
    crcReady = 1;
}

static unsigned long Crc(unsigned long crc, const BYTE *buf, size_t len)
{
    size_t i;
    if (!crcReady) InitCrc();
    crc ^= 0xFFFFFFFFUL;
    for (i = 0; i < len; i++)
        crc = crcTable[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFUL;
}

static void PutBE32(BYTE *p, unsigned long v)
{
    p[0] = (BYTE)(v >> 24); p[1] = (BYTE)(v >> 16);
    p[2] = (BYTE)(v >> 8);  p[3] = (BYTE)v;
}

static void WriteChunk(FILE *f, const char *type, const BYTE *data, unsigned long len)
{
    BYTE b[4];
    unsigned long crc, c, i;
    PutBE32(b, len);
    fwrite(b, 1, 4, f);
    fwrite(type, 1, 4, f);
    if (len) fwrite(data, 1, len, f);
    crc = Crc(0, (const BYTE *)type, 4);
    c = crc ^ 0xFFFFFFFFUL;
    for (i = 0; i < len; i++)
        c = crcTable[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    crc = c ^ 0xFFFFFFFFUL;
    PutBE32(b, crc);
    fwrite(b, 1, 4, f);
}

static BOOL SavePng(const char *name, const BYTE *rgba, int w, int h)
{
    size_t rowBytes = (size_t)w * 4;
    size_t rawSize = (size_t)h * (rowBytes + 1);
    BYTE *raw = (BYTE *)malloc(rawSize);
    int y;
    for (y = 0; y < h; y++)
    {
        raw[(size_t)y * (rowBytes + 1)] = 0;
        memcpy(raw + (size_t)y * (rowBytes + 1) + 1, rgba + (size_t)y * rowBytes, rowBytes);
    }

    size_t blocks = (rawSize + 65534) / 65535;
    size_t zSize = 2 + rawSize + blocks * 5 + 4;
    BYTE *z = (BYTE *)malloc(zSize);
    size_t zp = 0, pos = 0;
    z[zp++] = 0x78; z[zp++] = 0x01;
    while (pos < rawSize)
    {
        size_t n = rawSize - pos;
        if (n > 65535) n = 65535;
        z[zp++] = (pos + n == rawSize) ? 1 : 0;
        z[zp++] = (BYTE)(n & 0xFF);
        z[zp++] = (BYTE)(n >> 8);
        z[zp++] = (BYTE)(~n & 0xFF);
        z[zp++] = (BYTE)((~n >> 8) & 0xFF);
        memcpy(z + zp, raw + pos, n);
        zp += n; pos += n;
    }
    {
        unsigned long a = 1, b = 0; size_t i;
        for (i = 0; i < rawSize; i++)
        {
            a = (a + raw[i]) % 65521;
            b = (b + a) % 65521;
        }
        PutBE32(z + zp, (b << 16) | a);
        zp += 4;
    }

    FILE *f = fopen(name, "wb");
    if (!f) { free(raw); free(z); return FALSE; }

    static const BYTE sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    fwrite(sig, 1, 8, f);
    BYTE ihdr[13];
    PutBE32(ihdr, (unsigned long)w);
    PutBE32(ihdr + 4, (unsigned long)h);
    ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    WriteChunk(f, "IHDR", ihdr, 13);
    WriteChunk(f, "IDAT", z, (unsigned long)zp);
    WriteChunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw); free(z);
    return TRUE;
}

static BOOL CaptureSelf(HWND hwnd, const char *fileName)
{
    RECT wr, scr, rc;
    GetWindowRect(hwnd, &wr);
    scr.left = 0; scr.top = 0;
    scr.right = GetSystemMetrics(SM_CXSCREEN);
    scr.bottom = GetSystemMetrics(SM_CYSCREEN);

    rc = wr;
    InflateRect(&rc, PAD, PAD);
    IntersectRect(&rc, &rc, &scr);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return FALSE;

    HWND back = CreateWindowEx(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        TEXT("AeroBackdrop"), TEXT(""), WS_POPUP,
                               0, 0, scr.right, scr.bottom, NULL, NULL, g_hInst, NULL);

    SetCursorPos(scr.right - 1, scr.bottom - 1);

    g_backColor = RGB(0, 0, 0);
    ShowWindow(back, SW_SHOWNOACTIVATE);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetForegroundWindow(hwnd);
    UpdateWindow(back);
    DwmFlush();
    Sleep(400);
    BYTE *onBlack = GrabScreen(rc);

    g_backColor = RGB(255, 255, 255);
    InvalidateRect(back, NULL, TRUE);
    UpdateWindow(back);
    DwmFlush();
    Sleep(400);
    BYTE *onWhite = GrabScreen(rc);

    DestroyWindow(back);
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);

    BYTE *rgba = (BYTE *)malloc((size_t)w * h * 4);
    size_t i, count = (size_t)w * h;
    for (i = 0; i < count; i++)
    {
        const BYTE *pb = onBlack + i * 4;
        const BYTE *pw = onWhite + i * 4;
        int d = ((pw[2] - pb[2]) + (pw[1] - pb[1]) + (pw[0] - pb[0])) / 3;
        if (d < 0) d = 0;
        if (d > 255) d = 255;
        int a = 255 - d;
        BYTE *o = rgba + i * 4;
        if (a == 0) { o[0] = o[1] = o[2] = 0; o[3] = 0; }
        else
        {
            int r = pb[2] * 255 / a, g = pb[1] * 255 / a, b = pb[0] * 255 / a;
            o[0] = (BYTE)(r > 255 ? 255 : r);
            o[1] = (BYTE)(g > 255 ? 255 : g);
            o[2] = (BYTE)(b > 255 ? 255 : b);
            o[3] = (BYTE)a;
        }
    }

    BOOL ok = SavePng(fileName, rgba, w, h);
    free(onBlack); free(onWhite); free(rgba);
    return ok;
}

static void SetStatus(const char *s) { SetDlgItemText(g_panel, ID_STATUS, s); }

static HICON MakeBlankIcon(int size)
{
    static BYTE andMask[1024], xorMask[1024];
    if (size > 64) size = 64;
    memset(andMask, 0xFF, sizeof(andMask));
    memset(xorMask, 0x00, sizeof(xorMask));
    return CreateIcon(g_hInst, size, size, 1, 1, andMask, xorMask);
}

static BOOL LoadIconsFromFile(const char *path, HICON *big, HICON *small)
{
    int cb = GetSystemMetrics(SM_CXICON), cs = GetSystemMetrics(SM_CXSMICON);
    const char *dot = strrchr(path, '.');
    *big = *small = NULL;

    if (dot && _stricmp(dot, ".ico") == 0)
    {
        *big   = (HICON)LoadImage(NULL, path, IMAGE_ICON, cb, cb, LR_LOADFROMFILE);
        *small = (HICON)LoadImage(NULL, path, IMAGE_ICON, cs, cs, LR_LOADFROMFILE);
    }
    else
    {
        ExtractIconEx(path, 0, big, small, 1);
    }
    if (!*big && !*small) return FALSE;
    if (!*big)   *big   = CopyIcon(*small);
    if (!*small) *small = CopyIcon(*big);
    return TRUE;
}

static void SetStatus(const char *s);

static void ApplyIcon(void)
{
    char path[MAX_PATH];
    HICON big = NULL, small = NULL, ob, os;
    BOOL none = (IsDlgButtonChecked(g_panel, ID_NOICON) == BST_CHECKED);

    GetDlgItemText(g_panel, ID_ICON, path, sizeof(path));

    if (none)
    {
        big   = MakeBlankIcon(GetSystemMetrics(SM_CXICON));
        small = MakeBlankIcon(GetSystemMetrics(SM_CXSMICON));
    }
    else if (path[0])
    {
        if (!LoadIconsFromFile(path, &big, &small))
        {
            SetStatus("Could not load icon from that file.");
            return;
        }
    }

    ob = g_iconBig; os = g_iconSmall;
    SendMessage(g_glass, WM_SETICON, ICON_BIG,   (LPARAM)big);
    SendMessage(g_glass, WM_SETICON, ICON_SMALL, (LPARAM)small);
    g_iconBig = big; g_iconSmall = small;
    if (ob) DestroyIcon(ob);
    if (os) DestroyIcon(os);
}

static void BrowseIcon(HWND owner)
{
    char file[MAX_PATH] = "";
    OPENFILENAME ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = "Icons (*.ico;*.exe;*.dll)\0*.ico;*.exe;*.dll\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = "Choose title icon";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (GetOpenFileName(&ofn))
    {
        SetDlgItemText(owner, ID_ICON, file);
        CheckDlgButton(owner, ID_NOICON, BST_UNCHECKED);
        ApplyIcon();
    }
}

static void ApplyTitle(void)
{
    char buf[256];
    GetDlgItemText(g_panel, ID_TITLE, buf, sizeof(buf));
    SetWindowText(g_glass, buf);
    ApplyIcon();
}

static void ApplySize(void)
{
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    int maxW = sw - 2 * PAD, maxH = sh - 2 * PAD;
    int W = (int)GetDlgItemInt(g_panel, ID_W, NULL, FALSE);
    int H = (int)GetDlgItemInt(g_panel, ID_H, NULL, FALSE);
    const Ratio *r = &g_ratios[g_ratioIdx];

    if (W < 200) W = 200;
    if (r->rw > 0)
    {
        H = W * r->rh / r->rw;
        if (W > maxW) { W = maxW; H = W * r->rh / r->rw; }
        if (H > maxH) { H = maxH; W = H * r->rw / r->rh; }
        if (H < 100) { H = 100; W = H * r->rw / r->rh; }
    }
    else
    {
        if (H < 100) H = 100;
        if (W > maxW) W = maxW;
        if (H > maxH) H = maxH;
    }

    SetWindowPos(g_glass, NULL, 0, 0, W, H, SWP_NOMOVE | SWP_NOZORDER);
    ApplyTitle();
    SendMessage(g_panel, WM_SYNCSIZE, 0, 0);
}

static void KeepOnScreen(void)
{
    RECT wr;
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    int x, y;
    GetWindowRect(g_glass, &wr);
    x = wr.left; y = wr.top;
    if (x < PAD) x = PAD;
    if (y < PAD) y = PAD;
    if (x + (wr.right - wr.left) > sw - PAD) x = sw - PAD - (wr.right - wr.left);
    if (y + (wr.bottom - wr.top) > sh - PAD) y = sh - PAD - (wr.bottom - wr.top);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    SetWindowPos(g_glass, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

static void DoCapture(void)
{
    char name[MAX_PATH], clean[MAX_PATH];
    int i, j = 0, len;

    ApplyTitle();
    GetDlgItemText(g_panel, ID_FILE, name, sizeof(name));

    for (i = 0; name[i] && j < MAX_PATH - 5; i++)
        if (!strchr("\\/:*?\"<>|", name[i])) clean[j++] = name[i];
        clean[j] = 0;
    if (j == 0) strcpy(clean, "aero_capture");
    len = (int)strlen(clean);
    if (len < 4 || _stricmp(clean + len - 4, ".png") != 0) strcat(clean, ".png");

    KeepOnScreen();
    ShowWindow(g_panel, SW_HIDE);
    Sleep(300);

    BOOL ok = CaptureSelf(g_glass, clean);

    ShowWindow(g_panel, SW_SHOW);
    SetStatus(ok ? "Saved. Check the folder of the exe." : "Capture FAILED.");
    if (ok) MessageBeep(MB_OK);
}

static BOOL CALLBACK SetFontProc(HWND h, LPARAM l)
{
    SendMessage(h, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return TRUE;
}

static LRESULT CALLBACK PanelProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
        case WM_CREATE:
        {
            int i;
            HWND combo;
            CreateWindow("STATIC", "Window title:", WS_CHILD | WS_VISIBLE, 10, 10, 280, 16, hwnd, NULL, g_hInst, NULL);
            CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", "Aero Glass", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                           10, 28, 280, 22, hwnd, (HMENU)ID_TITLE, g_hInst, NULL);
            CreateWindow("STATIC", "File name:", WS_CHILD | WS_VISIBLE, 10, 58, 280, 16, hwnd, NULL, g_hInst, NULL);
            CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", "aero_capture.png", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                           10, 76, 280, 22, hwnd, (HMENU)ID_FILE, g_hInst, NULL);

            CreateWindow("STATIC", "Title icon (.ico / .exe):", WS_CHILD | WS_VISIBLE, 10, 106, 160, 16, hwnd, NULL, g_hInst, NULL);
            CreateWindow("BUTTON", "No icon", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                         225, 104, 65, 18, hwnd, (HMENU)ID_NOICON, g_hInst, NULL);
            CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                           10, 124, 170, 22, hwnd, (HMENU)ID_ICON, g_hInst, NULL);
            CreateWindow("BUTTON", "Browse...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                         185, 123, 60, 24, hwnd, (HMENU)ID_ICONBROWSE, g_hInst, NULL);
            CreateWindow("BUTTON", "Default", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                         250, 123, 40, 24, hwnd, (HMENU)ID_ICONRESET, g_hInst, NULL);

            CreateWindow("STATIC", "Aspect ratio:", WS_CHILD | WS_VISIBLE, 10, 154, 130, 16, hwnd, NULL, g_hInst, NULL);
            combo = CreateWindow("COMBOBOX", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                 10, 172, 130, 200, hwnd, (HMENU)ID_RATIO, g_hInst, NULL);
            for (i = 0; i < NRATIOS; i++)
                SendMessage(combo, CB_ADDSTRING, 0, (LPARAM)g_ratios[i].name);
            SendMessage(combo, CB_SETCURSEL, 0, 0);

            CreateWindow("STATIC", "Width:", WS_CHILD | WS_VISIBLE, 155, 154, 60, 16, hwnd, NULL, g_hInst, NULL);
            CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", "800", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
                           155, 172, 60, 22, hwnd, (HMENU)ID_W, g_hInst, NULL);
            CreateWindow("STATIC", "Height:", WS_CHILD | WS_VISIBLE, 225, 154, 60, 16, hwnd, NULL, g_hInst, NULL);
            CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", "600", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
                           225, 172, 65, 22, hwnd, (HMENU)ID_H, g_hInst, NULL);

            CreateWindow("BUTTON", "Apply size, title && icon", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                         10, 208, 130, 28, hwnd, (HMENU)ID_APPLY, g_hInst, NULL);
            CreateWindow("BUTTON", "Capture PNG", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                         155, 208, 135, 28, hwnd, (HMENU)ID_CAP, g_hInst, NULL);
            CreateWindow("STATIC", "Ready.", WS_CHILD | WS_VISIBLE, 10, 248, 280, 16, hwnd, (HMENU)ID_STATUS, g_hInst, NULL);

            EnumChildWindows(hwnd, SetFontProc, 0);
            return 0;
        }
        case WM_SYNCSIZE:
        {
            RECT r;
            if (g_glass)
            {
                GetWindowRect(g_glass, &r);
                SetDlgItemInt(hwnd, ID_W, r.right - r.left, FALSE);
                SetDlgItemInt(hwnd, ID_H, r.bottom - r.top, FALSE);
            }
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
                case ID_RATIO:
                    if (HIWORD(wParam) == CBN_SELCHANGE)
                    {
                        g_ratioIdx = (int)SendDlgItemMessage(hwnd, ID_RATIO, CB_GETCURSEL, 0, 0);
                        EnableWindow(GetDlgItem(hwnd, ID_H), g_ratios[g_ratioIdx].rw == 0);
                        ApplySize();
                    }
                    break;
                case ID_ICONBROWSE:
                    BrowseIcon(hwnd);
                    break;
                case ID_ICONRESET:
                    SetDlgItemText(hwnd, ID_ICON, "");
                    CheckDlgButton(hwnd, ID_NOICON, BST_UNCHECKED);
                    ApplyIcon();
                    SetStatus("Default icon restored.");
                    break;
                case ID_NOICON:
                    ApplyIcon();
                    break;
                case ID_APPLY:
                    ApplySize();
                    SetStatus("Size, title and icon applied.");
                    break;
                case ID_CAP:
                    DoCapture();
                    break;
            }
            return 0;
                case WM_CLOSE:
                    DestroyWindow(g_glass);
                    return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK GlassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
        case WM_CREATE:
        {
            BOOL enabled = FALSE;
            MARGINS m = { -1, -1, -1, -1 };
            DwmIsCompositionEnabled(&enabled);
            if (!enabled)
                MessageBox(hwnd, TEXT("Aero is not enabled. The capture will have no glass."),
                           TEXT("Warning"), MB_OK | MB_ICONWARNING);
                DwmExtendFrameIntoClientArea(hwnd, &m);
            return 0;
        }
        case WM_SIZING:
        {
            const Ratio *r = &g_ratios[g_ratioIdx];
            if (r->rw > 0)
            {
                RECT *rc = (RECT *)lParam;
                int w = rc->right - rc->left, h = rc->bottom - rc->top;
                switch (wParam)
                {
                    case WMSZ_LEFT: case WMSZ_RIGHT:
                        rc->bottom = rc->top + w * r->rh / r->rw; break;
                    case WMSZ_TOP: case WMSZ_BOTTOM:
                        rc->right = rc->left + h * r->rw / r->rh; break;
                    case WMSZ_TOPLEFT: case WMSZ_TOPRIGHT:
                        rc->top = rc->bottom - w * r->rh / r->rw; break;
                    default:
                        rc->bottom = rc->top + w * r->rh / r->rw; break;
                }
                return TRUE;
            }
            break;
        }
                    case WM_SIZE:
                        if (g_panel) SendMessage(g_panel, WM_SYNCSIZE, 0, 0);
                        return 0;
                    case WM_KEYDOWN:
                        if (wParam == VK_F5 || wParam == VK_SPACE) DoCapture();
                        return 0;
                    case WM_ERASEBKGND:
                    {
                        RECT rc;
                        GetClientRect(hwnd, &rc);
                        FillRect((HDC)wParam, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
                        return 1;
                    }
                    case WM_DESTROY:
                        if (g_panel) DestroyWindow(g_panel);
                        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show)
{
    g_hInst = hInst;

    WNDCLASS bc = {0};
    bc.lpfnWndProc = BackProc;
    bc.hInstance = hInst;
    bc.lpszClassName = TEXT("AeroBackdrop");
    RegisterClass(&bc);

    WNDCLASS gc = {0};
    gc.lpfnWndProc = GlassProc;
    gc.hInstance = hInst;
    gc.hCursor = LoadCursor(NULL, IDC_ARROW);
    gc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    gc.lpszClassName = TEXT("AeroGlassWin");
    RegisterClass(&gc);

    WNDCLASS pc = {0};
    pc.lpfnWndProc = PanelProc;
    pc.hInstance = hInst;
    pc.hCursor = LoadCursor(NULL, IDC_ARROW);
    pc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    pc.lpszClassName = TEXT("AeroPanelWin");
    RegisterClass(&pc);

    g_glass = CreateWindowEx(0, "AeroGlassWin", "Aero Glass",
                             WS_OVERLAPPEDWINDOW, 120, 80, 800, 600, NULL, NULL, hInst, NULL);

    g_panel = CreateWindowEx(WS_EX_TOOLWINDOW, "AeroPanelWin", "Config",
                             WS_CAPTION | WS_SYSMENU, 960, 80, 316, 310, NULL, NULL, hInst, NULL);
							 
    ShowWindow(g_glass, SW_SHOW);
    ShowWindow(g_panel, SW_SHOW);
    SendMessage(g_panel, WM_SYNCSIZE, 0, 0);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0))
    {
        if (g_panel && IsDialogMessage(g_panel, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}
