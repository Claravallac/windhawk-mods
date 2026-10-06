// ==WindhawkMod==
// @id              mic-volume-tray
// @name            Mic Volume Tray
// @description     Ícone na bandeja com o volume do microfone e slider estilo Windows 11 ao clicar
// @version         1.3.0
// @author          you
// @include         explorer.exe
// @compilerOptions -lole32 -lgdi32 -luser32 -lshell32 -ladvapi32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Mic Volume Tray

Adiciona um ícone na bandeja do sistema que mostra o volume do microfone padrão:

- número (0–100) com fundo transparente, na cor do tema (branco no escuro, preto no claro), como os ícones nativos do Windows 11;
- medidor fino na base do ícone com o nível do sinal ao vivo (na cor de destaque do sistema);
- número em **vermelho** quando o microfone está mudo;
- tooltip com o nome do dispositivo, volume e estado.

## Slider (visual Windows 11)
**Clique esquerdo** no ícone abre um flyout no estilo do Windows 11, na mesma posição
dos painéis nativos da bandeja (canto da tela, 12px de margem), com:

- botão de microfone para mutar/reativar;
- slider para ajustar o volume (arraste ou clique na barra);
- número do volume à direita;
- medidor ao vivo do sinal discreto abaixo do slider (opcional).

O visual acompanha o **tema claro/escuro** do Windows e a **cor de destaque** do sistema.

Com o painel aberto: **rodinha do mouse** ou **setas** ajustam o volume (±2%),
**M** muta/reativa, **Esc** ou clicar fora fecha.

**Clique do meio** no ícone muta/reativa (opcional).
**Clique direito** abre um menu com atalho para os dispositivos de gravação.

## Observação (Windows 11)
O ícone novo pode aparecer escondido no menu "^" da bandeja. Arraste-o para fora
(ou ative em Configurações → Personalização → Barra de tarefas → Outros ícones da bandeja).
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- showLiveMeter: true
  $name: Mostrar medidor ao vivo
  $description: Barra com o nível do sinal do microfone (no ícone e no painel do slider)
- updateIntervalMs: 50
  $name: Intervalo de atualização (ms)
  $description: Menor = medidor mais suave, mas usa um pouco mais de CPU (20–500)
- middleClickMute: true
  $name: Clique do meio alterna mudo
  $description: Clicar com o botão do meio no ícone muta/reativa o microfone
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <atomic>
#include <vector>

#define WM_TRAY    (WM_APP + 1)
#define WM_RELOAD  (WM_APP + 2)
#define ID_TIMER   1
#define IDM_MUTE   100
#define IDM_RECDEV 101

// PKEY_Device_FriendlyName {a45c254e-df1c-4efd-8020-67d146a850e0}, 14
static const PROPERTYKEY kFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

// O mingw só declara IAudioMeterInformation (forward). Definimos a interface aqui,
// com o mesmo layout de vtable e IID do Windows SDK.
// IID_IAudioMeterInformation {C02216F6-8C67-4B5B-9D00-D008E73E0064}
static const GUID kIidMeter = {
    0xC02216F6, 0x8C67, 0x4B5B, {0x9D, 0x00, 0xD0, 0x08, 0xE7, 0x3E, 0x00, 0x64}};

struct IMicMeter : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetPeakValue(float* pfPeak) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMeteringChannelCount(UINT* pnChannelCount) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetChannelsPeakValues(UINT32 u32ChannelCount, float* afPeakValues) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryHardwareSupport(DWORD* pdwHardwareSupportMask) = 0;
};

static std::atomic<int> g_showMeter{1};
static std::atomic<int> g_interval{50};
static std::atomic<int> g_middleMute{1};

static HANDLE g_thread = nullptr;
static DWORD g_threadId = 0;
static HANDLE g_mutex = nullptr;
static std::atomic<HWND> g_hwnd{nullptr};
static UINT g_taskbarCreated = 0;

// ---------------------------------------------------------------- Áudio

struct Mic {
    IMMDeviceEnumerator* en = nullptr;
    IAudioEndpointVolume* vol = nullptr;
    IMicMeter* meter = nullptr;
    WCHAR id[256] = {};
    WCHAR name[128] = {};

    void ReleaseDevice() {
        if (vol) { vol->Release(); vol = nullptr; }
        if (meter) { meter->Release(); meter = nullptr; }
        id[0] = 0;
        name[0] = 0;
    }

    void ReleaseAll() {
        ReleaseDevice();
        if (en) { en->Release(); en = nullptr; }
    }

    // Garante que estamos ligados ao microfone padrão atual.
    void Refresh() {
        if (!en) {
            if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                        __uuidof(IMMDeviceEnumerator), (void**)&en))) {
                en = nullptr;
                return;
            }
        }

        IMMDevice* dev = nullptr;
        if (FAILED(en->GetDefaultAudioEndpoint(eCapture, eConsole, &dev)) || !dev) {
            ReleaseDevice();
            return;
        }

        LPWSTR pid = nullptr;
        if (SUCCEEDED(dev->GetId(&pid)) && pid) {
            bool same = vol && wcsncmp(id, pid, ARRAYSIZE(id)) == 0;
            if (!same) {
                ReleaseDevice();
                lstrcpynW(id, pid, ARRAYSIZE(id));

                dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&vol);
                dev->Activate(kIidMeter, CLSCTX_ALL, nullptr, (void**)&meter);

                IPropertyStore* ps = nullptr;
                if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps)) && ps) {
                    PROPVARIANT pv;
                    PropVariantInit(&pv);
                    if (SUCCEEDED(ps->GetValue(kFriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) {
                        lstrcpynW(name, pv.pwszVal, ARRAYSIZE(name));
                    }
                    PropVariantClear(&pv);
                    ps->Release();
                }
            }
            CoTaskMemFree(pid);
        }
        dev->Release();
    }
};

// ---------------------------------------------------------------- Ícone da bandeja

// Ícone no estilo dos ícones de bandeja do Windows 11: fundo transparente,
// número na cor do tema (branco no escuro, preto no claro) e medidor fino embaixo.
// Mudo = número na cor "crítica" do sistema; sem microfone = cinza.
static HICON MakeIcon(int pct, bool muted, bool noDev, int lvl16, bool showMeter,
                      bool dark, COLORREF accent, const WCHAR* face) {
    int s = GetSystemMetrics(SM_CXSMICON);
    if (s < 16) s = 16;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = s;
    bi.bmiHeader.biHeight = -s;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC sdc = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC dc = CreateCompatibleDC(sdc);
    ReleaseDC(nullptr, sdc);
    if (!color || !bits || !dc) {
        if (color) DeleteObject(color);
        if (dc) DeleteDC(dc);
        return nullptr;
    }

    // máscara monocromática zerada (o canal alfa do bitmap de cor é quem manda)
    int stride = ((s + 15) / 16) * 2;
    std::vector<BYTE> zeros((size_t)stride * s, 0);
    HBITMAP mask = CreateBitmap(s, s, 1, 1, zeros.data());

    HGDIOBJ oldBmp = SelectObject(dc, color);
    DWORD* px = (DWORD*)bits;
    memset(bits, 0, (size_t)s * s * 4);

    int barH = showMeter ? (s / 8 > 2 ? s / 8 : 2) : 0;
    int gap = showMeter ? 1 : 0;
    int textH = s - barH - gap;

    // 1) texto branco sobre preto -> a intensidade vira a cobertura (alfa)
    wchar_t txt[8];
    if (noDev) lstrcpyW(txt, L"--");
    else _snwprintf(txt, 7, L"%d", pct);
    txt[7] = 0;

    int digits = (int)wcslen(txt);
    int fh = digits >= 3 ? (s * 10 / 16) : (s * 13 / 16);
    HFONT font = CreateFontW(-fh, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, face);
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT tr = {0, 0, s, textH};
    DrawTextW(dc, txt, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
    SelectObject(dc, oldFont);
    DeleteObject(font);
    GdiFlush();

    // 2) cor do texto conforme o tema / estado
    COLORREF fg;
    if (noDev) fg = dark ? RGB(150, 150, 150) : RGB(110, 110, 110);
    else if (muted) fg = dark ? RGB(255, 153, 164) : RGB(196, 43, 28);
    else fg = dark ? RGB(255, 255, 255) : RGB(26, 26, 26);
    DWORD fgRgb = ((DWORD)GetRValue(fg) << 16) | ((DWORD)GetGValue(fg) << 8) | (DWORD)GetBValue(fg);

    // alfa direto (não pré-multiplicado), como nos ícones comuns
    for (int i = 0; i < s * s; i++) {
        DWORD a = (px[i] >> 8) & 0xFF;
        px[i] = (a << 24) | fgRgb;
    }

    // 3) medidor fino na base: trilho translúcido + barra de nível
    for (int y = textH; y < s; y++) {
        for (int x = 0; x < s; x++) px[y * s + x] = 0;
    }
    if (showMeter && !noDev) {
        for (int y = s - barH; y < s; y++) {
            for (int x = 0; x < s; x++) px[y * s + x] = (70u << 24) | fgRgb;
        }
        if (!muted && lvl16 > 0) {
            int w = lvl16 * s / 16;
            if (w < 1) w = 1;
            float f = lvl16 / 16.0f;
            COLORREF bc = f < 0.9f ? accent : (dark ? RGB(255, 153, 164) : RGB(196, 43, 28));
            DWORD bRgb = ((DWORD)GetRValue(bc) << 16) | ((DWORD)GetGValue(bc) << 8) | (DWORD)GetBValue(bc);
            for (int y = s - barH; y < s; y++) {
                for (int x = 0; x < w; x++) px[y * s + x] = (255u << 24) | bRgb;
            }
        }
    }

    SelectObject(dc, oldBmp);
    DeleteDC(dc);

    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HICON icon = CreateIconIndirect(&ii);

    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

// ---------------------------------------------------------------- GDI+ (carregado dinamicamente)
// Usado só para desenhar formas com anti-aliasing. Se não carregar, cai para GDI puro.

typedef int GpStatus;

struct GdipStartupInput {
    UINT32 GdiplusVersion;
    void* DebugEventCallback;
    BOOL SuppressBackgroundThread;
    BOOL SuppressExternalCodecs;
};

struct GdipApi {
    HMODULE mod;
    ULONG_PTR token;
    bool ok;
    GpStatus (WINAPI *Startup)(ULONG_PTR*, const GdipStartupInput*, void*);
    void (WINAPI *Shutdown)(ULONG_PTR);
    GpStatus (WINAPI *CreateFromHDC)(HDC, void**);
    GpStatus (WINAPI *DeleteGraphics)(void*);
    GpStatus (WINAPI *SetSmoothing)(void*, int);
    GpStatus (WINAPI *CreateSolidFill)(DWORD, void**);
    GpStatus (WINAPI *FreeBrush)(void*);
    GpStatus (WINAPI *FillEllipse)(void*, void*, float, float, float, float);
    GpStatus (WINAPI *FillPath)(void*, void*, void*);
    GpStatus (WINAPI *CreatePath)(int, void**);
    GpStatus (WINAPI *DeletePath)(void*);
    GpStatus (WINAPI *AddPathArc)(void*, float, float, float, float, float, float);
    GpStatus (WINAPI *ClosePathFigure)(void*);
    GpStatus (WINAPI *CreatePen)(DWORD, float, int, void**);
    GpStatus (WINAPI *FreePen)(void*);
    GpStatus (WINAPI *DrawPath)(void*, void*, void*);
    GpStatus (WINAPI *DrawLine)(void*, void*, float, float, float, float);
};

static GdipApi g_gp = {};

#define GP_LOAD(member, name)                                                   \
    g_gp.member = reinterpret_cast<decltype(g_gp.member)>(GetProcAddress(g_gp.mod, name)); \
    if (!g_gp.member) allOk = false;

static void GdipInit() {
    g_gp.mod = LoadLibraryW(L"gdiplus.dll");
    if (!g_gp.mod) return;
    bool allOk = true;
    GP_LOAD(Startup, "GdiplusStartup");
    GP_LOAD(Shutdown, "GdiplusShutdown");
    GP_LOAD(CreateFromHDC, "GdipCreateFromHDC");
    GP_LOAD(DeleteGraphics, "GdipDeleteGraphics");
    GP_LOAD(SetSmoothing, "GdipSetSmoothingMode");
    GP_LOAD(CreateSolidFill, "GdipCreateSolidFill");
    GP_LOAD(FreeBrush, "GdipDeleteBrush");
    GP_LOAD(FillEllipse, "GdipFillEllipse");
    GP_LOAD(FillPath, "GdipFillPath");
    GP_LOAD(CreatePath, "GdipCreatePath");
    GP_LOAD(DeletePath, "GdipDeletePath");
    GP_LOAD(AddPathArc, "GdipAddPathArc");
    GP_LOAD(ClosePathFigure, "GdipClosePathFigure");
    GP_LOAD(CreatePen, "GdipCreatePen1");
    GP_LOAD(FreePen, "GdipDeletePen");
    GP_LOAD(DrawPath, "GdipDrawPath");
    GP_LOAD(DrawLine, "GdipDrawLine");
    if (!allOk) return;

    GdipStartupInput in = {1, nullptr, FALSE, FALSE};
    if (g_gp.Startup(&g_gp.token, &in, nullptr) == 0) g_gp.ok = true;
}

static void GdipExit() {
    if (g_gp.ok && g_gp.Shutdown) g_gp.Shutdown(g_gp.token);
    g_gp.ok = false;
    // gdiplus.dll fica carregada (já costuma estar no explorer.exe)
}

static DWORD Argb(COLORREF c, int a = 255) {
    return ((DWORD)a << 24) | ((DWORD)GetRValue(c) << 16) | ((DWORD)GetGValue(c) << 8) | (DWORD)GetBValue(c);
}

// Desenho de formas: GDI+ com anti-aliasing, ou GDI como reserva.
struct Gfx {
    HDC dc;
    void* g = nullptr;

    explicit Gfx(HDC d) : dc(d) {
        if (g_gp.ok) {
            g_gp.CreateFromHDC(dc, &g);
            if (g) g_gp.SetSmoothing(g, 4);  // SmoothingModeAntiAlias
        }
    }
    ~Gfx() {
        if (g) g_gp.DeleteGraphics(g);
    }

    static void RRPath(void* path, float x, float y, float w, float h, float r) {
        if (r * 2 > h) r = h / 2;
        if (r * 2 > w) r = w / 2;
        float d = r * 2;
        g_gp.AddPathArc(path, x, y, d, d, 180, 90);
        g_gp.AddPathArc(path, x + w - d, y, d, d, 270, 90);
        g_gp.AddPathArc(path, x + w - d, y + h - d, d, d, 0, 90);
        g_gp.AddPathArc(path, x, y + h - d, d, d, 90, 90);
        g_gp.ClosePathFigure(path);
    }

    void FillRR(float x, float y, float w, float h, float r, COLORREF c) {
        if (g) {
            void* br = nullptr;
            void* path = nullptr;
            g_gp.CreateSolidFill(Argb(c), &br);
            g_gp.CreatePath(0, &path);
            if (br && path) {
                RRPath(path, x, y, w, h, r);
                g_gp.FillPath(g, br, path);
            }
            if (path) g_gp.DeletePath(path);
            if (br) g_gp.FreeBrush(br);
        } else {
            HBRUSH b = CreateSolidBrush(c);
            HGDIOBJ ob = SelectObject(dc, b);
            HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
            RoundRect(dc, (int)x, (int)y, (int)(x + w) + 1, (int)(y + h) + 1, (int)(r * 2), (int)(r * 2));
            SelectObject(dc, op);
            SelectObject(dc, ob);
            DeleteObject(b);
        }
    }

    void FillCircle(float cx, float cy, float r, COLORREF c) {
        if (g) {
            void* br = nullptr;
            g_gp.CreateSolidFill(Argb(c), &br);
            if (br) {
                g_gp.FillEllipse(g, br, cx - r, cy - r, r * 2, r * 2);
                g_gp.FreeBrush(br);
            }
        } else {
            HBRUSH b = CreateSolidBrush(c);
            HGDIOBJ ob = SelectObject(dc, b);
            HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
            Ellipse(dc, (int)(cx - r), (int)(cy - r), (int)(cx + r) + 1, (int)(cy + r) + 1);
            SelectObject(dc, op);
            SelectObject(dc, ob);
            DeleteObject(b);
        }
    }

    void StrokeRR(float x, float y, float w, float h, float r, float width, COLORREF c) {
        if (g) {
            void* pen = nullptr;
            void* path = nullptr;
            g_gp.CreatePen(Argb(c), width, 0, &pen);
            g_gp.CreatePath(0, &path);
            if (pen && path) {
                RRPath(path, x, y, w, h, r);
                g_gp.DrawPath(g, pen, path);
            }
            if (path) g_gp.DeletePath(path);
            if (pen) g_gp.FreePen(pen);
        } else {
            HPEN p = CreatePen(PS_SOLID, 1, c);
            HGDIOBJ op = SelectObject(dc, p);
            HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
            RoundRect(dc, (int)x, (int)y, (int)(x + w) + 1, (int)(y + h) + 1, (int)(r * 2), (int)(r * 2));
            SelectObject(dc, ob);
            SelectObject(dc, op);
            DeleteObject(p);
        }
    }

    void Line(float x1, float y1, float x2, float y2, float width, COLORREF c) {
        if (g) {
            void* pen = nullptr;
            g_gp.CreatePen(Argb(c), width, 0, &pen);
            if (pen) {
                g_gp.DrawLine(g, pen, x1, y1, x2, y2);
                g_gp.FreePen(pen);
            }
        } else {
            int w = (int)(width + 0.5f);
            if (w < 1) w = 1;
            HPEN p = CreatePen(PS_SOLID, w, c);
            HGDIOBJ op = SelectObject(dc, p);
            MoveToEx(dc, (int)x1, (int)y1, nullptr);
            LineTo(dc, (int)x2, (int)y2);
            SelectObject(dc, op);
            DeleteObject(p);
        }
    }
};

// ---------------------------------------------------------------- Tema do Windows (claro/escuro + destaque)

struct Theme {
    bool dark;
    COLORREF bg, border, text, text2, trackEmpty, thumbFill, thumbBorder, btnHover, meterBg, accent;
};

static Theme g_th = {};

static bool RegRead(const WCHAR* sub, const WCHAR* val, void* buf, DWORD* sz) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    DWORD type = 0;
    LONG r = RegQueryValueExW(k, val, nullptr, &type, (BYTE*)buf, sz);
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

static void LoadTheme() {
    DWORD light = 0, sz = sizeof(light);
    bool have = RegRead(L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                        L"SystemUsesLightTheme", &light, &sz);
    bool dark = !(have && light);
    g_th.dark = dark;

    if (dark) {
        g_th.bg = RGB(44, 44, 44);
        g_th.border = RGB(66, 66, 66);
        g_th.text = RGB(255, 255, 255);
        g_th.text2 = RGB(206, 206, 206);
        g_th.trackEmpty = RGB(158, 158, 158);
        g_th.thumbFill = RGB(69, 69, 69);
        g_th.thumbBorder = RGB(86, 86, 86);
        g_th.btnHover = RGB(57, 57, 57);
        g_th.meterBg = RGB(70, 70, 70);
        g_th.accent = RGB(76, 194, 255);
    } else {
        g_th.bg = RGB(243, 243, 243);
        g_th.border = RGB(220, 220, 220);
        g_th.text = RGB(26, 26, 26);
        g_th.text2 = RGB(96, 96, 96);
        g_th.trackEmpty = RGB(136, 136, 136);
        g_th.thumbFill = RGB(255, 255, 255);
        g_th.thumbBorder = RGB(222, 222, 222);
        g_th.btnHover = RGB(234, 234, 234);
        g_th.meterBg = RGB(222, 222, 222);
        g_th.accent = RGB(0, 95, 184);
    }

    // Cor de destaque do sistema: tema escuro usa "Light2", tema claro usa "Dark1"
    // (a mesma escolha dos controles do WinUI).
    BYTE pal[32] = {};
    DWORD psz = sizeof(pal);
    if (RegRead(L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                L"AccentPalette", pal, &psz) && psz >= 32) {
        int idx = dark ? 1 : 4;
        g_th.accent = RGB(pal[idx * 4], pal[idx * 4 + 1], pal[idx * 4 + 2]);
    }
}

// ---------------------------------------------------------------- Estado

static Mic g_mic;
static HICON g_curIcon = nullptr;
static bool g_added = false;
static DWORD g_lastRefresh = 0;
static float g_dispPeak = 0.0f;

static int g_lastPct = -2;
static int g_lastLvl = -2;
static bool g_lastMute = false;
static bool g_lastNoDev = false;
static WCHAR g_lastTip[128] = {};

// estado "ao vivo" usado pelo painel do slider
static int g_curPct = 0;
static bool g_curMute = false;
static bool g_curNoDev = true;
static float g_curPeak = 0.0f;

// painel do slider
static HWND g_popup = nullptr;
static DWORD g_hideTick = 0;
static bool g_drag = false;
static bool g_hoverBtn = false;
static bool g_hoverSlider = false;
static bool g_tracking = false;
static bool g_dwmRound = false;
static float g_sc = 1.0f;
static WCHAR g_faceText[64] = L"Segoe UI";
static WCHAR g_faceIcon[64] = L"Segoe MDL2 Assets";

static void ForceUpdate() {
    g_lastPct = -2;
    g_lastLvl = -2;
}

static void TrayApply(HWND hwnd, HICON icon, const WCHAR* tip) {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = icon;
    lstrcpynW(nid.szTip, tip, ARRAYSIZE(nid.szTip));

    if (!g_added) {
        g_added = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    } else if (!Shell_NotifyIconW(NIM_MODIFY, &nid)) {
        g_added = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    }
}

static void Tick(HWND hwnd) {
    DWORD now = GetTickCount();
    if (!g_mic.vol || now - g_lastRefresh > 1000) {
        g_mic.Refresh();
        g_lastRefresh = now;

        // acompanha mudança de tema claro/escuro e da cor de destaque
        bool wasDark = g_th.dark;
        COLORREF wasAccent = g_th.accent;
        LoadTheme();
        if (g_th.dark != wasDark || g_th.accent != wasAccent) ForceUpdate();
    }

    bool noDev = (g_mic.vol == nullptr);
    int pct = 0;
    BOOL muted = FALSE;
    bool showMeter = g_showMeter.load() != 0;
    float peak = 0.0f;

    if (!noDev) {
        float v = 0.0f;
        if (FAILED(g_mic.vol->GetMasterVolumeLevelScalar(&v))) {
            g_mic.ReleaseDevice();
            noDev = true;
        } else {
            pct = (int)(v * 100.0f + 0.5f);
            g_mic.vol->GetMute(&muted);
            if (showMeter && g_mic.meter) {
                g_mic.meter->GetPeakValue(&peak);
            }
        }
    }

    // raiz quadrada deixa o medidor mais sensível a sinais baixos; queda suave
    peak = sqrtf(peak);
    g_dispPeak = peak > g_dispPeak ? peak : g_dispPeak * 0.8f;
    if (!showMeter || noDev || muted) g_dispPeak = 0.0f;
    int lvl = (int)(g_dispPeak * 16.0f + 0.5f);
    if (lvl > 16) lvl = 16;

    // atualiza o estado usado pelo painel do slider
    if (!(g_drag && !noDev)) g_curPct = pct;
    g_curMute = (muted != 0);
    g_curNoDev = noDev;
    g_curPeak = g_dispPeak;
    if (g_popup && IsWindowVisible(g_popup)) {
        InvalidateRect(g_popup, nullptr, FALSE);
    }

    bool changed = !g_added || pct != g_lastPct || lvl != g_lastLvl ||
                   (muted != 0) != g_lastMute || noDev != g_lastNoDev;
    if (!changed) return;

    WCHAR tip[128];
    if (noDev) {
        lstrcpynW(tip, L"Nenhum microfone padrão encontrado", ARRAYSIZE(tip));
    } else {
        _snwprintf(tip, 127, L"Mic: %d%%%s\n%s", pct, muted ? L" (mudo)" : L"", g_mic.name);
        tip[127] = 0;
    }

    HICON icon = MakeIcon(pct, muted != 0, noDev, lvl, showMeter, g_th.dark, g_th.accent, g_faceText);
    if (icon) {
        TrayApply(hwnd, icon, tip);
        if (g_curIcon) DestroyIcon(g_curIcon);
        g_curIcon = icon;
    }

    g_lastPct = pct;
    g_lastLvl = lvl;
    g_lastMute = (muted != 0);
    g_lastNoDev = noDev;
    lstrcpynW(g_lastTip, tip, ARRAYSIZE(g_lastTip));
}

static void ToggleMute() {
    if (!g_mic.vol) return;
    BOOL m = FALSE;
    if (SUCCEEDED(g_mic.vol->GetMute(&m))) {
        g_mic.vol->SetMute(!m, nullptr);
        g_curMute = !m;
        ForceUpdate();
        if (g_popup && IsWindowVisible(g_popup)) InvalidateRect(g_popup, nullptr, FALSE);
    }
}

// ---------------------------------------------------------------- Fontes e DWM

static bool FontExists(const WCHAR* face) {
    HDC dc = GetDC(nullptr);
    HFONT f = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                          DEFAULT_PITCH | FF_DONTCARE, face);
    HGDIOBJ o = SelectObject(dc, f);
    WCHAR got[LF_FACESIZE] = {};
    GetTextFaceW(dc, LF_FACESIZE, got);
    SelectObject(dc, o);
    DeleteObject(f);
    ReleaseDC(nullptr, dc);
    return lstrcmpiW(got, face) == 0;
}

static void InitFaces() {
    lstrcpyW(g_faceText, FontExists(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI");
    lstrcpyW(g_faceIcon, FontExists(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets");
}

// DWM: cantos arredondados do Windows 11 e cor da borda (carregado dinamicamente)
typedef HRESULT(WINAPI* PFN_DwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);
static PFN_DwmSetWindowAttribute g_dwmSet = nullptr;

static void InitDwm(HWND h) {
    HMODULE m = LoadLibraryW(L"dwmapi.dll");
    if (!m) return;
    g_dwmSet = reinterpret_cast<PFN_DwmSetWindowAttribute>(GetProcAddress(m, "DwmSetWindowAttribute"));
    if (!g_dwmSet) return;
    int pref = 2;  // DWMWCP_ROUND (raio de 8px)
    g_dwmRound = SUCCEEDED(g_dwmSet(h, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &pref, sizeof(pref)));
}

static void ApplyDwmBorder(HWND h) {
    if (g_dwmRound && g_dwmSet) {
        COLORREF bc = g_th.border;
        g_dwmSet(h, 34 /*DWMWA_BORDER_COLOR*/, &bc, sizeof(bc));
    }
}

// ---------------------------------------------------------------- Painel do slider

struct Geo {
    int W, H;
    int trackL, trackR, trackCY, trackH, thumbR;
    int travelL, travelR;
    RECT btn;
    RECT num;
    RECT title;
    RECT sliderHit;
};

static int Sc(int v) { return (int)(v * g_sc + 0.5f); }

static Geo GetGeo() {
    Geo g = {};
    g.W = Sc(300);
    g.H = Sc(88);
    g.trackL = Sc(56);
    g.trackR = g.W - Sc(52);
    g.trackCY = Sc(56);
    g.trackH = Sc(4);
    g.thumbR = Sc(10);
    g.travelL = g.trackL + g.thumbR;
    g.travelR = g.trackR - g.thumbR;
    SetRect(&g.btn, Sc(12), Sc(40), Sc(44), Sc(72));
    SetRect(&g.num, g.W - Sc(48), Sc(40), g.W - Sc(14), Sc(72));
    SetRect(&g.title, Sc(16), Sc(12), g.W - Sc(16), Sc(32));
    SetRect(&g.sliderHit, g.trackL, Sc(40), g.trackR, Sc(72));
    return g;
}

static HFONT MkFont(int h, int weight, const WCHAR* face) {
    return CreateFontW(-h, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, face);
}

static void HidePopup() {
    if (g_popup && IsWindowVisible(g_popup)) {
        ShowWindow(g_popup, SW_HIDE);
        g_hideTick = GetTickCount();
    }
    g_drag = false;
    g_hoverBtn = false;
    g_hoverSlider = false;
}

static void SetVolumePct(int pct) {
    if (!g_mic.vol) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    g_mic.vol->SetMasterVolumeLevelScalar(pct / 100.0f, nullptr);
    g_curPct = pct;
    if (g_popup) InvalidateRect(g_popup, nullptr, FALSE);
}

static void SetPctFromX(int x) {
    Geo g = GetGeo();
    float v = (float)(x - g.travelL) / (float)(g.travelR - g.travelL);
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    SetVolumePct((int)(v * 100.0f + 0.5f));
}

static void PaintPopup(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    Geo g = GetGeo();
    const Theme& t = g_th;

    HDC dc = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, g.W, g.H);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);

    bool dis = g_curNoDev;
    bool muted = g_curMute;
    int pct = g_curPct;

    // fundo (os cantos são recortados pelo DWM, ou pela região no Windows 10)
    HBRUSH bgBrush = CreateSolidBrush(t.bg);
    RECT full = {0, 0, g.W, g.H};
    FillRect(dc, &full, bgBrush);
    DeleteObject(bgBrush);

    int cy = g.trackCY;
    int th = g.trackH;
    float span = (float)(g.travelR - g.travelL);
    int tx = g.travelL + (int)(span * pct / 100.0f + 0.5f);

    // ---- formas (anti-aliasing)
    {
        Gfx gf(dc);

        // borda própria só quando o DWM não desenha a dele
        if (!g_dwmRound) {
            gf.StrokeRR(0.5f, 0.5f, g.W - 1.0f, g.H - 1.0f, (float)Sc(8) - 0.5f, 1.0f, t.border);
        }

        // fundo do botão ao passar o mouse
        if (g_hoverBtn && !dis) {
            gf.FillRR((float)g.btn.left, (float)g.btn.top,
                      (float)(g.btn.right - g.btn.left), (float)(g.btn.bottom - g.btn.top),
                      (float)Sc(4), t.btnHover);
        }

        // trilho + parte preenchida
        float trackTop = (float)cy - th / 2.0f;
        gf.FillRR((float)g.trackL, trackTop, (float)(g.trackR - g.trackL), (float)th, th / 2.0f,
                  t.trackEmpty);
        if (!dis) {
            gf.FillRR((float)g.trackL, trackTop, (float)(tx - g.trackL), (float)th, th / 2.0f, t.accent);
        }

        // medidor ao vivo (fino, abaixo do slider)
        if (g_showMeter.load() && !dis) {
            float my = (float)Sc(75);
            float mh = (float)Sc(2);
            float mw = (float)(g.trackR - g.trackL);
            gf.FillRR((float)g.trackL, my, mw, mh, mh / 2, t.meterBg);
            if (!muted && g_curPeak > 0.0f) {
                float f = g_curPeak > 1.0f ? 1.0f : g_curPeak;
                float w = mw * f;
                if (w < mh) w = mh;
                COLORREF c = f < 0.7f ? RGB(60, 210, 90) : (f < 0.9f ? RGB(240, 200, 40) : RGB(235, 60, 60));
                gf.FillRR((float)g.trackL, my, w, mh, mh / 2, c);
            }
        }

        // thumb: anel externo + círculo interno de destaque (cresce ao passar o mouse)
        float R = (float)g.thumbR;
        float inner = g_drag ? Sc(5) : (g_hoverSlider ? Sc(7) : Sc(6));
        gf.FillCircle((float)tx, (float)cy, R, t.thumbBorder);
        gf.FillCircle((float)tx, (float)cy, R - 1.0f, t.thumbFill);
        gf.FillCircle((float)tx, (float)cy, inner, dis ? t.trackEmpty : t.accent);
    }

    // ---- textos
    SetBkMode(dc, TRANSPARENT);
    HFONT fCap = MkFont(Sc(12), FW_NORMAL, g_faceText);
    HFONT fBody = MkFont(Sc(14), FW_NORMAL, g_faceText);
    HFONT fIcon = MkFont(Sc(16), FW_NORMAL, g_faceIcon);
    HGDIOBJ oldFont = SelectObject(dc, fCap);

    // nome do dispositivo (e estado de mudo)
    WCHAR title[200];
    if (dis) {
        lstrcpynW(title, L"Nenhum microfone", ARRAYSIZE(title));
    } else {
        const WCHAR* nm = g_mic.name[0] ? g_mic.name : L"Microfone";
        if (muted) _snwprintf(title, 199, L"%s  \u2022  Mudo", nm);
        else lstrcpynW(title, nm, ARRAYSIZE(title));
        title[199] = 0;
    }
    SetTextColor(dc, t.text2);
    DrawTextW(dc, title, -1, &g.title, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    // número do volume
    WCHAR vt[16];
    if (dis) lstrcpyW(vt, L"--");
    else _snwprintf(vt, 15, L"%d", pct);
    vt[15] = 0;
    SelectObject(dc, fBody);
    SetTextColor(dc, dis ? t.text2 : t.text);
    DrawTextW(dc, vt, -1, &g.num, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    // ícone do microfone
    SelectObject(dc, fIcon);
    SetTextColor(dc, dis ? t.text2 : t.text);
    DrawTextW(dc, L"\xE720", -1, &g.btn, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    SelectObject(dc, oldFont);
    DeleteObject(fCap);
    DeleteObject(fBody);
    DeleteObject(fIcon);

    // ---- risco diagonal quando mudo (com "vão" na cor do fundo, como nos ícones nativos)
    if (muted && !dis) {
        Gfx gf(dc);
        float cx = (g.btn.left + g.btn.right) / 2.0f;
        float cyb = (g.btn.top + g.btn.bottom) / 2.0f;
        float d = (float)Sc(8);
        float wGap = (float)Sc(4);
        float wLine = Sc(2) > 1 ? (float)Sc(2) - 0.5f : 1.5f;
        COLORREF gapColor = g_hoverBtn ? t.btnHover : t.bg;
        gf.Line(cx - d, cyb + d, cx + d, cyb - d, wGap, gapColor);
        gf.Line(cx - d, cyb + d, cx + d, cyb - d, wLine, t.text);
    }

    BitBlt(hdc, 0, 0, g.W, g.H, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

static void ShowPopup() {
    if (!g_popup) return;
    if (IsWindowVisible(g_popup)) {
        HidePopup();
        return;
    }
    // o clique que acabou de fechar o painel não deve reabri-lo
    if (GetTickCount() - g_hideTick < 250) return;

    LoadTheme();

    HDC sdc = GetDC(nullptr);
    int dpi = GetDeviceCaps(sdc, LOGPIXELSX);
    ReleaseDC(nullptr, sdc);
    g_sc = dpi / 96.0f;
    if (g_sc < 1.0f) g_sc = 1.0f;

    Geo g = GetGeo();
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    RECT wa = mi.rcWork;
    int margin = Sc(12);

    // mesma posição dos flyouts nativos da bandeja: canto da tela, 12px da barra de tarefas
    bool tbLeft = wa.left > mi.rcMonitor.left;
    bool tbTop = wa.top > mi.rcMonitor.top;
    int x = tbLeft ? wa.left + margin : wa.right - g.W - margin;
    int y = tbTop ? wa.top + margin : wa.bottom - g.H - margin;

    SetWindowPos(g_popup, HWND_TOPMOST, x, y, g.W, g.H, SWP_NOACTIVATE);
    if (g_dwmRound) {
        ApplyDwmBorder(g_popup);
    } else {
        SetWindowRgn(g_popup, CreateRoundRectRgn(0, 0, g.W + 1, g.H + 1, Sc(16), Sc(16)), FALSE);
    }
    ShowWindow(g_popup, SW_SHOW);
    SetForegroundWindow(g_popup);
    InvalidateRect(g_popup, nullptr, FALSE);
}

static LRESULT CALLBACK PopupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            PaintPopup(hwnd);
            return 0;

        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) HidePopup();
            return 0;

        case WM_LBUTTONDOWN: {
            if (g_curNoDev) return 0;
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            POINT p = {x, y};
            Geo g = GetGeo();
            if (PtInRect(&g.btn, p)) {
                ToggleMute();
            } else if (PtInRect(&g.sliderHit, p)) {
                g_drag = true;
                g_hoverSlider = true;
                SetCapture(hwnd);
                SetPctFromX(x);
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            POINT p = {x, y};
            Geo g = GetGeo();
            if (!g_tracking) {
                TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
                g_tracking = true;
            }
            bool hb = !g_curNoDev && PtInRect(&g.btn, p);
            bool hs = !g_curNoDev && (g_drag || PtInRect(&g.sliderHit, p));
            if (hb != g_hoverBtn || hs != g_hoverSlider) {
                g_hoverBtn = hb;
                g_hoverSlider = hs;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (g_drag) SetPctFromX(x);
            return 0;
        }

        case WM_MOUSELEAVE:
            g_tracking = false;
            if (g_hoverBtn || g_hoverSlider) {
                g_hoverBtn = false;
                g_hoverSlider = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_LBUTTONUP:
            if (g_drag) {
                g_drag = false;
                ReleaseCapture();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_CAPTURECHANGED:
            g_drag = false;
            return 0;

        case WM_MOUSEWHEEL:
            if (!g_curNoDev) {
                int d = GET_WHEEL_DELTA_WPARAM(wp);
                SetVolumePct(g_curPct + (d > 0 ? 2 : -2));
            }
            return 0;

        case WM_KEYDOWN:
            switch (wp) {
                case VK_ESCAPE:
                    HidePopup();
                    break;
                case VK_LEFT:
                case VK_DOWN:
                    if (!g_curNoDev) SetVolumePct(g_curPct - 2);
                    break;
                case VK_RIGHT:
                case VK_UP:
                    if (!g_curNoDev) SetVolumePct(g_curPct + 2);
                    break;
                case 'M':
                    ToggleMute();
                    break;
            }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- Janela da bandeja

static void ShowMenu(HWND hwnd) {
    HidePopup();
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, IDM_MUTE, L"Mutar / reativar microfone");
    AppendMenuW(menu, MF_STRING, IDM_RECDEV, L"Abrir dispositivos de gravação");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                             pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);

    if (cmd == IDM_MUTE) {
        ToggleMute();
    } else if (cmd == IDM_RECDEV) {
        ShellExecuteW(nullptr, L"open", L"control.exe", L"mmsys.cpl,,1", nullptr, SW_SHOWNORMAL);
    }
}

static void StartTimer(HWND hwnd) {
    int ms = g_interval.load();
    if (ms < 20) ms = 20;
    if (ms > 500) ms = 500;
    SetTimer(hwnd, ID_TIMER, (UINT)ms, nullptr);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreated && g_taskbarCreated != 0) {
        // Explorer reiniciou a barra de tarefas: recria o ícone
        g_added = false;
        ForceUpdate();
        Tick(hwnd);
        return 0;
    }

    switch (msg) {
        case WM_TIMER:
            if (wp == ID_TIMER) Tick(hwnd);
            return 0;

        case WM_RELOAD:
            KillTimer(hwnd, ID_TIMER);
            StartTimer(hwnd);
            ForceUpdate();
            return 0;

        case WM_TRAY:
            switch (LOWORD(lp)) {
                case WM_LBUTTONUP:
                    ShowPopup();
                    break;
                case WM_MBUTTONUP:
                    if (g_middleMute.load()) ToggleMute();
                    break;
                case WM_RBUTTONUP:
                case WM_CONTEXTMENU:
                    ShowMenu(hwnd);
                    break;
            }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static DWORD WINAPI ThreadProc(LPVOID) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    GdipInit();
    InitFaces();
    LoadTheme();

    HINSTANCE hinst = GetModuleHandleW(nullptr);

    const WCHAR* cls = L"WindhawkMicVolumeTrayWnd";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hinst;
    wc.lpszClassName = cls;
    RegisterClassW(&wc);

    const WCHAR* popCls = L"WindhawkMicVolumePopupWnd";
    WNDCLASSW pc = {};
    pc.style = CS_DROPSHADOW;
    pc.lpfnWndProc = PopupProc;
    pc.hInstance = hinst;
    pc.lpszClassName = popCls;
    pc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&pc);

    // janela top-level escondida (message-only não recebe o broadcast TaskbarCreated)
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, cls, L"", WS_POPUP, 0, 0, 0, 0,
                                nullptr, nullptr, hinst, nullptr);
    if (hwnd) {
        g_hwnd = hwnd;

        g_popup = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, popCls, L"", WS_POPUP,
                                  0, 0, 10, 10, nullptr, nullptr, hinst, nullptr);
        if (g_popup) InitDwm(g_popup);

        g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        StartTimer(hwnd);
        Tick(hwnd);

        MSG m;
        while (GetMessageW(&m, nullptr, 0, 0) > 0) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }

        KillTimer(hwnd, ID_TIMER);

        NOTIFYICONDATAW nid = {};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd;
        nid.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        g_added = false;

        if (g_popup) {
            DestroyWindow(g_popup);
            g_popup = nullptr;
        }

        g_hwnd = nullptr;
        DestroyWindow(hwnd);
    }

    if (g_curIcon) {
        DestroyIcon(g_curIcon);
        g_curIcon = nullptr;
    }
    g_mic.ReleaseAll();
    UnregisterClassW(popCls, hinst);
    UnregisterClassW(cls, hinst);
    GdipExit();
    CoUninitialize();
    return 0;
}

// ---------------------------------------------------------------- Windhawk

static void LoadSettings() {
    g_showMeter = Wh_GetIntSetting(L"showLiveMeter") ? 1 : 0;
    g_interval = Wh_GetIntSetting(L"updateIntervalMs");
    g_middleMute = Wh_GetIntSetting(L"middleClickMute") ? 1 : 0;
}

BOOL Wh_ModInit() {
    LoadSettings();

    // só uma instância por sessão (pode haver vários processos explorer.exe)
    g_mutex = CreateMutexW(nullptr, FALSE, L"Local\\WindhawkMicVolumeTray");
    if (g_mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(g_mutex);
        g_mutex = nullptr;
        return TRUE;
    }

    g_thread = CreateThread(nullptr, 0, ThreadProc, nullptr, 0, &g_threadId);
    return TRUE;
}

void Wh_ModSettingsChanged() {
    LoadSettings();
    HWND h = g_hwnd.load();
    if (h) PostMessageW(h, WM_RELOAD, 0, 0);
}

void Wh_ModUninit() {
    if (g_thread) {
        for (int i = 0; i < 30; i++) {
            PostThreadMessageW(g_threadId, WM_QUIT, 0, 0);
            if (WaitForSingleObject(g_thread, 100) == WAIT_OBJECT_0) break;
        }
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    if (g_mutex) {
        CloseHandle(g_mutex);
        g_mutex = nullptr;
    }
}
