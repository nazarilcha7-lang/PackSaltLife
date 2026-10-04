// Coords Overlay v3
// Direct2D + DirectWrite, layered-окно с попиксельной прозрачностью.
//  * HUD с координатами (перетаскивается мышью в режиме меню)
//  * Меню настроек: по горячей клавише окно перестаёт быть "прозрачным для мыши", появляется курсор
//  * Любые клавиши/кнопки мыши (кроме ЛКМ/ПКМ) назначаются в меню и сохраняются в MCOverlay.ini
//  * Окно привязано к окну Minecraft (GLFW30), работает поверх оконного/безрамочного полноэкранного режима; эксклюзивный fullscreen зависит от рендерера игры
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <dxgiformat.h>
#include <d2d1.h>
#include <dwrite.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <cwctype>
#include <string>
#include <stdint.h>
#include <algorithm>
#include <vector>
#include <tlhelp32.h>
#include <memory>
#include <psapi.h>
#include <mmsystem.h>

#ifdef _MSC_VER
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "winmm.lib")
#endif

// ======================= Протокол пайпа (72 байта) =======================
#pragma pack(push, 1)
struct PlayerCoords {
    double x, y, z;
    float yaw, pitch;
    int32_t hasTarget;
    int32_t aiming;
    double targetX, targetY, targetZ;
    float aimYaw, aimPitch;
};
#pragma pack(pop)
static_assert(sizeof(PlayerCoords) == 72, "packet must be 72 bytes");

// ======================= Конфиг =======================
struct Config {
    int   keyMenu = VK_INSERT;
    int   keyHud = VK_RSHIFT;
    int   keyTarget = VK_F6;
    int   keyAuto = VK_F7;
    int   keyPanic = VK_END;
    bool  panicExit = false;      // закрывать программу при панике
    bool  hudOn = true;
    bool  showXYZ = true, showBlock = true, showAngles = true, showDir = true, showStatus = true;
    bool  showPlayerNew = true;
    bool  onlyInGame = true;
    float opacity = 0.85f;
    float scale = 1.0f;
    int   accent = 0;
    int   hudX = 24, hudY = 24;
    // --- Автоудар (одиночный мир) ---
    bool  autoOn = false;
    bool  autoCrit = false;
    bool  autoPlayers = true;
    bool  autoHostile = true;
    bool  autoPassive = false;
    bool  autoIgnoreTamed = true;
    float autoDist = 3.0f;        // блоков
    float autoDelayMin = 0.0f;    // тиков (1 тик = 50 мс)
    float autoDelayMax = 3.0f;
    bool  aimOn = false;      int keyAim = 0;
    float aimRange = 12, aimFov = 60, aimSpeed = 240, aimSlow = 25;
    float aimSpread = 0.6f, aimJitter = 0.15f, aimFlick = 0.4f;
    bool  aimHostile = true, aimPassive = false, aimTamed = true, aimLos = true, aimPlayers = false;
    // --- Визуалы (одиночный мир): ESP, Nametag, звук ---
    bool  soundOn = true;
    bool  espOn = false, espPlayers = true, espMobs = false, espFill = true;
    float espRange = 64.0f;
    bool  nametagOn = false, ntHp = true, ntArmor = true, ntHands = true, ntDist = false;
};
static Config g_cfg;

static const unsigned kAccents[6] = { 0x5CC8FF, 0x3B9EFF, 0x4FE3E0, 0xA5D8FF, 0x7C9CFF, 0x35D0BA };

static std::wstring CfgPath() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(NULL, p, MAX_PATH);
    std::wstring s = p;
    size_t i = s.find_last_of(L"\\/");
    return s.substr(0, i + 1) + L"MCOverlay.ini";
}
static int  RdI(const wchar_t* sec, const wchar_t* k, int d) { return (int)GetPrivateProfileIntW(sec, k, d, CfgPath().c_str()); }
static void WrI(const wchar_t* sec, const wchar_t* k, int v) { WritePrivateProfileStringW(sec, k, std::to_wstring(v).c_str(), CfgPath().c_str()); }

static void LoadConfig() {
    Config d;
    g_cfg.keyMenu = RdI(L"keys", L"menu", d.keyMenu);
    g_cfg.keyHud = RdI(L"keys", L"hud", d.keyHud);
    g_cfg.keyTarget = RdI(L"keys", L"target", d.keyTarget);
    g_cfg.keyAuto = RdI(L"keys", L"auto", d.keyAuto);
    g_cfg.keyPanic = RdI(L"keys", L"panic", d.keyPanic);
    g_cfg.panicExit = RdI(L"panic", L"exit", 0) != 0;
    g_cfg.autoOn = RdI(L"auto", L"on", 0) != 0;
    g_cfg.autoCrit = RdI(L"auto", L"crit", 0) != 0;
    g_cfg.autoPlayers = RdI(L"auto", L"players", 1) != 0;
    g_cfg.autoHostile = RdI(L"auto", L"hostile", 1) != 0;
    g_cfg.autoPassive = RdI(L"auto", L"passive", 0) != 0;
    g_cfg.autoIgnoreTamed = RdI(L"auto", L"ignoreTamed", 1) != 0;
    g_cfg.autoDist = std::min(6.0f, std::max(1.0f, RdI(L"auto", L"dist10", 30) / 10.0f));
    g_cfg.autoDelayMin = (float)std::min(10, std::max(0, RdI(L"auto", L"delayMin", 0)));
    g_cfg.autoDelayMax = (float)std::min(10, std::max(0, RdI(L"auto", L"delayMax", 3)));
    g_cfg.keyAim = RdI(L"keys", L"aim", 0);
    g_cfg.aimOn = RdI(L"aim", L"on", 0) != 0;
    g_cfg.aimHostile = RdI(L"aim", L"hostile", 1) != 0;
    g_cfg.aimPassive = RdI(L"aim", L"passive", 0) != 0;
    g_cfg.aimTamed = RdI(L"aim", L"tamed", 1) != 0;
    g_cfg.aimLos = RdI(L"aim", L"los", 1) != 0;
    g_cfg.aimPlayers = RdI(L"aim", L"players", 0) != 0;
    g_cfg.aimRange = std::min(32.0f, std::max(2.0f, (float)RdI(L"aim", L"range", 12)));
    g_cfg.aimFov = std::min(180.0f, std::max(5.0f, (float)RdI(L"aim", L"fov", 60)));
    g_cfg.aimSpeed = std::min(900.0f, std::max(30.0f, (float)RdI(L"aim", L"speed", 240)));
    g_cfg.aimSlow = std::min(90.0f, std::max(5.0f, (float)RdI(L"aim", L"slow", 25)));
    g_cfg.aimSpread = std::min(1.0f, std::max(0.0f, RdI(L"aim", L"spread100", 60) / 100.0f));
    g_cfg.aimJitter = std::min(1.0f, std::max(0.0f, RdI(L"aim", L"jitter100", 15) / 100.0f));
    g_cfg.aimFlick = std::min(3.0f, std::max(0.0f, RdI(L"aim", L"flick100", 40) / 100.0f));
    g_cfg.soundOn = RdI(L"ui", L"sound", 1) != 0;
    g_cfg.espOn = RdI(L"esp", L"on", 0) != 0;
    g_cfg.espPlayers = RdI(L"esp", L"players", 1) != 0;
    g_cfg.espMobs = RdI(L"esp", L"mobs", 0) != 0;
    g_cfg.espFill = RdI(L"esp", L"fill", 1) != 0;
    g_cfg.espRange = std::min(128.0f, std::max(8.0f, (float)RdI(L"esp", L"range", 64)));
    g_cfg.nametagOn = RdI(L"nametag", L"on", 0) != 0;
    g_cfg.ntHp = RdI(L"nametag", L"hp", 1) != 0;
    g_cfg.ntArmor = RdI(L"nametag", L"armor", 1) != 0;
    g_cfg.ntHands = RdI(L"nametag", L"hands", 1) != 0;
    g_cfg.ntDist = RdI(L"nametag", L"dist", 0) != 0;
    g_cfg.hudOn = RdI(L"ui", L"hudOn", 1) != 0;
    g_cfg.showXYZ = RdI(L"ui", L"xyz", 1) != 0;
    g_cfg.showBlock = RdI(L"ui", L"block", 1) != 0;
    g_cfg.showAngles = RdI(L"ui", L"angles", 1) != 0;
    g_cfg.showDir = RdI(L"ui", L"dir", 1) != 0;
    g_cfg.showStatus = RdI(L"ui", L"status", 1) != 0;
    g_cfg.showPlayerNew = RdI(L"ui", L"playerNew", RdI(L"ui", L"aimAssist", 1)) != 0;
    g_cfg.onlyInGame = RdI(L"ui", L"onlyInGame", 1) != 0;
    g_cfg.opacity = std::min(1.0f, std::max(0.2f, RdI(L"ui", L"opacity", 85) / 100.0f));
    g_cfg.scale = std::min(2.0f, std::max(0.7f, RdI(L"ui", L"scale", 100) / 100.0f));
    g_cfg.accent = std::min(5, std::max(0, RdI(L"ui", L"accent", 0)));
    g_cfg.hudX = RdI(L"ui", L"x", d.hudX);
    g_cfg.hudY = RdI(L"ui", L"y", d.hudY);
}
// Файл-мост для Java-мода: %APPDATA%\MCOverlay\autohit.properties
// Мод перечитывает его при изменении (проверка раз в секунду).
static void WriteAutoHitBridge() {
    wchar_t appdata[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    std::wstring dir = std::wstring(appdata) + L"\\MCOverlay";
    CreateDirectoryW(dir.c_str(), NULL);
    std::wstring file = dir + L"\\autohit.properties";
    int dmin = (int)(g_cfg.autoDelayMin + 0.5f), dmax = (int)(g_cfg.autoDelayMax + 0.5f);
    if (dmax < dmin) std::swap(dmin, dmax);
    FILE* f = _wfopen(file.c_str(), L"wb");
    if (!f) return;
    fprintf(f, "enabled=%d\r\ndistance=%.2f\r\ncritOnly=%d\r\nplayers=%d\r\nhostile=%d\r\npassive=%d\r\nignoreTamed=%d\r\ndelayMin=%d\r\ndelayMax=%d\r\n",
        g_cfg.autoOn ? 1 : 0, (double)g_cfg.autoDist, g_cfg.autoCrit ? 1 : 0,
        g_cfg.autoPlayers ? 1 : 0, g_cfg.autoHostile ? 1 : 0, g_cfg.autoPassive ? 1 : 0,
        g_cfg.autoIgnoreTamed ? 1 : 0, dmin, dmax);
    fclose(f);
}

// Файл-мост наводки: %APPDATA%\MCOverlay\aim.properties
static void WriteAimBridge() {
    wchar_t appdata[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    std::wstring dir = std::wstring(appdata) + L"\\MCOverlay";
    CreateDirectoryW(dir.c_str(), NULL);
    std::wstring file = dir + L"\\aim.properties";
    FILE* f = _wfopen(file.c_str(), L"wb");
    if (!f) return;
    fprintf(f, "enabled=%d\r\nrange=%.1f\r\nfov=%.1f\r\nspeed=%.1f\r\nslowRadius=%.1f\r\n"
               "spread=%.2f\r\njitter=%.2f\r\nflick=%.2f\r\nhostile=%d\r\npassive=%d\r\n"
               "ignoreTamed=%d\r\nrequireLos=%d\r\nplayers=%d\r\n",
        g_cfg.aimOn ? 1 : 0, (double)g_cfg.aimRange, (double)g_cfg.aimFov,
        (double)g_cfg.aimSpeed, (double)g_cfg.aimSlow,
        (double)g_cfg.aimSpread, (double)g_cfg.aimJitter, (double)g_cfg.aimFlick,
        g_cfg.aimHostile ? 1 : 0, g_cfg.aimPassive ? 1 : 0,
        g_cfg.aimTamed ? 1 : 0, g_cfg.aimLos ? 1 : 0, g_cfg.aimPlayers ? 1 : 0);
    fclose(f);
}

// Файл-мост ESP/Nametag: %APPDATA%\MCOverlay\visuals.properties
// Java-мод отправляет данные в оверлей только если хотя бы одна функция включена.
static void WriteVisualsBridge() {
    wchar_t appdata[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    std::wstring dir = std::wstring(appdata) + L"\\MCOverlay";
    CreateDirectoryW(dir.c_str(), NULL);
    std::wstring file = dir + L"\\visuals.properties";
    FILE* f = _wfopen(file.c_str(), L"wb");
    if (!f) return;
    fprintf(f, "esp=%d\r\nnametag=%d\r\nplayers=%d\r\nmobs=%d\r\nrange=%.0f\r\n",
        g_cfg.espOn ? 1 : 0, g_cfg.nametagOn ? 1 : 0,
        g_cfg.espPlayers ? 1 : 0, g_cfg.espMobs ? 1 : 0, (double)g_cfg.espRange);
    fclose(f);
}

static void SaveConfig() {
    WrI(L"keys", L"auto", g_cfg.keyAuto);
    WrI(L"keys", L"panic", g_cfg.keyPanic);
    WrI(L"panic", L"exit", g_cfg.panicExit);
    WrI(L"auto", L"on", g_cfg.autoOn);
    WrI(L"auto", L"crit", g_cfg.autoCrit);
    WrI(L"auto", L"players", g_cfg.autoPlayers);
    WrI(L"auto", L"hostile", g_cfg.autoHostile);
    WrI(L"auto", L"passive", g_cfg.autoPassive);
    WrI(L"auto", L"ignoreTamed", g_cfg.autoIgnoreTamed);
    WrI(L"auto", L"dist10", (int)(g_cfg.autoDist * 10.0f + 0.5f));
    WrI(L"auto", L"delayMin", (int)(g_cfg.autoDelayMin + 0.5f));
    WrI(L"auto", L"delayMax", (int)(g_cfg.autoDelayMax + 0.5f));
    WriteAutoHitBridge();
    WrI(L"keys", L"aim", g_cfg.keyAim);
    WrI(L"aim", L"on", g_cfg.aimOn);
    WrI(L"aim", L"hostile", g_cfg.aimHostile);
    WrI(L"aim", L"passive", g_cfg.aimPassive);
    WrI(L"aim", L"tamed", g_cfg.aimTamed);
    WrI(L"aim", L"los", g_cfg.aimLos);
    WrI(L"aim", L"players", g_cfg.aimPlayers);
    WrI(L"aim", L"range", (int)(g_cfg.aimRange + 0.5f));
    WrI(L"aim", L"fov", (int)(g_cfg.aimFov + 0.5f));
    WrI(L"aim", L"speed", (int)(g_cfg.aimSpeed + 0.5f));
    WrI(L"aim", L"slow", (int)(g_cfg.aimSlow + 0.5f));
    WrI(L"aim", L"spread100", (int)(g_cfg.aimSpread * 100.0f + 0.5f));
    WrI(L"aim", L"jitter100", (int)(g_cfg.aimJitter * 100.0f + 0.5f));
    WrI(L"aim", L"flick100", (int)(g_cfg.aimFlick * 100.0f + 0.5f));
    WriteAimBridge();
    WrI(L"ui", L"sound", g_cfg.soundOn);
    WrI(L"esp", L"on", g_cfg.espOn);
    WrI(L"esp", L"players", g_cfg.espPlayers);
    WrI(L"esp", L"mobs", g_cfg.espMobs);
    WrI(L"esp", L"fill", g_cfg.espFill);
    WrI(L"esp", L"range", (int)(g_cfg.espRange + 0.5f));
    WrI(L"nametag", L"on", g_cfg.nametagOn);
    WrI(L"nametag", L"hp", g_cfg.ntHp);
    WrI(L"nametag", L"armor", g_cfg.ntArmor);
    WrI(L"nametag", L"hands", g_cfg.ntHands);
    WrI(L"nametag", L"dist", g_cfg.ntDist);
    WriteVisualsBridge();
    WrI(L"keys", L"menu", g_cfg.keyMenu);
    WrI(L"keys", L"hud", g_cfg.keyHud);
    WrI(L"keys", L"target", g_cfg.keyTarget);
    WrI(L"ui", L"hudOn", g_cfg.hudOn);
    WrI(L"ui", L"xyz", g_cfg.showXYZ);
    WrI(L"ui", L"block", g_cfg.showBlock);
    WrI(L"ui", L"angles", g_cfg.showAngles);
    WrI(L"ui", L"dir", g_cfg.showDir);
    WrI(L"ui", L"status", g_cfg.showStatus);
    WrI(L"ui", L"playerNew", g_cfg.showPlayerNew);
    WrI(L"ui", L"onlyInGame", g_cfg.onlyInGame);
    WrI(L"ui", L"opacity", (int)(g_cfg.opacity * 100.0f + 0.5f));
    WrI(L"ui", L"scale", (int)(g_cfg.scale * 100.0f + 0.5f));
    WrI(L"ui", L"accent", g_cfg.accent);
    WrI(L"ui", L"x", g_cfg.hudX);
    WrI(L"ui", L"y", g_cfg.hudY);
}

// ======================= Глобальное состояние =======================
static volatile BOOL g_running = TRUE;
static volatile BOOL g_connected = FALSE;
static volatile ULONGLONG g_lastPacket = 0;
static volatile BOOL g_haveData = FALSE;
static CRITICAL_SECTION g_cs;
static PlayerCoords g_coords = {};
static ULONGLONG g_startTick = 0;

static HWND g_hwnd = NULL;
static HWND g_game = NULL;
static bool g_visible = false;
static bool g_menuOpen = false;
static ULONGLONG g_menuTick = 0;
static bool g_quit = false;
static int g_x = 0, g_y = 0, g_w = 800, g_h = 600;

// ======================= Pipe =======================
DWORD WINAPI PipeServerThread(LPVOID) {
    const wchar_t* pipeName = L"\\\\.\\pipe\\mc_coords";
    while (g_running) {
        HANDLE hPipe = CreateNamedPipeW(pipeName, PIPE_ACCESS_INBOUND,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1,
            sizeof(PlayerCoords) * 16, sizeof(PlayerCoords) * 16, 0, NULL);
        if (hPipe == INVALID_HANDLE_VALUE) { Sleep(500); continue; }

        BOOL ok = ConnectNamedPipe(hPipe, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (ok && g_running) {
            g_connected = TRUE;
            PlayerCoords buf;
            DWORD got = 0;
            while (g_running) {
                if (!ReadFile(hPipe, &buf, sizeof(buf), &got, NULL) || got < sizeof(buf)) break;
                EnterCriticalSection(&g_cs);
                g_coords = buf;
                LeaveCriticalSection(&g_cs);
                g_lastPacket = GetTickCount64();
                g_haveData = TRUE;
            }
            g_connected = FALSE;
        }
        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);
    }
    return 0;
}

// ======================= Direct2D / DirectWrite =======================
template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

static ID2D1Factory* g_d2d = nullptr;
static IDWriteFactory* g_dw = nullptr;
static ID2D1DCRenderTarget* g_rt = nullptr;
static ID2D1SolidColorBrush* g_brush = nullptr;
static HDC g_memDC = NULL;
static HBITMAP g_dib = NULL, g_oldBmp = NULL;
static int g_dibW = 0, g_dibH = 0;

static IDWriteTextFormat *fTitle, *fText, *fTextC, *fTextR, *fSmall, *fSmallR, *fMono, *fMonoL, *fSec, *fBig, *fHead, *fTitleL, *fTinyC, *fNameL, *fBadge;

static IDWriteTextFormat* MakeFmt(const wchar_t* font, float size, DWRITE_FONT_WEIGHT w, DWRITE_TEXT_ALIGNMENT al) {
    IDWriteTextFormat* f = nullptr;
    g_dw->CreateTextFormat(font, NULL, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"ru-ru", &f);
    if (f) {
        f->SetTextAlignment(al);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    return f;
}

static bool InitGraphics() {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), NULL, (void**)&g_d2d))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&g_dw))) return false;
    const wchar_t* ui = L"Segoe UI";
    fTitle  = MakeFmt(ui, 17, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
    fText   = MakeFmt(ui, 14, DWRITE_FONT_WEIGHT_REGULAR, DWRITE_TEXT_ALIGNMENT_LEADING);
    fTextC  = MakeFmt(ui, 14, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    fTextR  = MakeFmt(ui, 14, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING);
    fSmall  = MakeFmt(ui, 12, DWRITE_FONT_WEIGHT_REGULAR, DWRITE_TEXT_ALIGNMENT_LEADING);
    fSmallR = MakeFmt(ui, 12, DWRITE_FONT_WEIGHT_REGULAR, DWRITE_TEXT_ALIGNMENT_TRAILING);
    fNameL  = MakeFmt(ui, 15, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
    fBadge  = MakeFmt(ui, 12, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    fTinyC  = MakeFmt(ui, 11, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    fSec    = MakeFmt(ui, 11, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
    fBig    = MakeFmt(ui, 24, DWRITE_FONT_WEIGHT_LIGHT, DWRITE_TEXT_ALIGNMENT_CENTER);
    fHead   = MakeFmt(ui, 16, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
    fTitleL = MakeFmt(ui, 22, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
    fMono   = MakeFmt(L"Consolas", 18, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING);
    fMonoL  = MakeFmt(L"Consolas", 18, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
    return fTitle && fText && fMono;
}

static void DestroyTarget() {
    SafeRelease(g_brush);
    SafeRelease(g_rt);
    if (g_memDC) {
        if (g_oldBmp) SelectObject(g_memDC, g_oldBmp);
        if (g_dib) DeleteObject(g_dib);
        DeleteDC(g_memDC);
    }
    g_memDC = NULL; g_dib = NULL; g_oldBmp = NULL; g_dibW = g_dibH = 0;
}

static bool EnsureTarget(int w, int h) {
    if (g_rt && g_dibW == w && g_dibH == h) return true;
    DestroyTarget();

    HDC screen = GetDC(NULL);
    g_memDC = CreateCompatibleDC(screen);
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;       // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    g_dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, screen);
    if (!g_dib) return false;
    g_oldBmp = (HBITMAP)SelectObject(g_memDC, g_dib);
    g_dibW = w; g_dibH = h;

    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        0.0f, 0.0f, D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);
    if (FAILED(g_d2d->CreateDCRenderTarget(&props, &g_rt))) return false;
    g_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    if (FAILED(g_rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &g_brush))) return false;
    return true;
}

// ======================= Рисование: примитивы =======================
static D2D1_COLOR_F Col(unsigned rgb, float a = 1.0f) { return D2D1::ColorF(rgb, a); }
static D2D1_COLOR_F Lerp(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}
static unsigned Accent() { return kAccents[g_cfg.accent]; }

// Палитра: приятный голубой
static const unsigned C_BG   = 0x0A1828;   // фон окна
static const unsigned C_CARD = 0x0F2740;   // фон карточек
static const unsigned C_EDGE = 0x9CD6FF;   // светло-голубой для линий/ховера
static const unsigned C_TEXT = 0xEAF6FF;
static const unsigned C_DIM  = 0x8FB6D9;
static const unsigned C_MUTE = 0x6F93B5;
static const unsigned C_OFF  = 0x2A4766;
static const unsigned C_DARK = 0x06182A;
static const unsigned C_DANGER = 0xFF7A8A;

static void FillRR(float l, float t, float r, float b, float rad, D2D1_COLOR_F c) {
    g_brush->SetColor(c);
    g_rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(l, t, r, b), rad, rad), g_brush);
}
static void StrokeRR(float l, float t, float r, float b, float rad, D2D1_COLOR_F c, float w) {
    g_brush->SetColor(c);
    g_rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(l, t, r, b), rad, rad), g_brush, w);
}
static void FillRect(float l, float t, float r, float b, D2D1_COLOR_F c) {
    g_brush->SetColor(c);
    g_rt->FillRectangle(D2D1::RectF(l, t, r, b), g_brush);
}
static void Circle(float x, float y, float rad, D2D1_COLOR_F c, bool fill, float w = 1.0f) {
    g_brush->SetColor(c);
    if (fill) g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), rad, rad), g_brush);
    else g_rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), rad, rad), g_brush, w);
}
static void Line(float x1, float y1, float x2, float y2, D2D1_COLOR_F c, float w) {
    g_brush->SetColor(c);
    g_rt->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), g_brush, w);
}
static void Txt(const wchar_t* s, IDWriteTextFormat* f, float l, float t, float r, float b, D2D1_COLOR_F c) {
    g_brush->SetColor(c);
    g_rt->DrawText(s, (UINT32)wcslen(s), f, D2D1::RectF(l, t, r, b), g_brush);
}
static void SetXform(float s, float ox, float oy) {
    g_rt->SetTransform(D2D1::Matrix3x2F(s, 0, 0, s, ox, oy));
}

// ======================= Ввод =======================
static bool g_prev[256], g_cur[256];
static int g_mx = 0, g_my = 0;
static bool g_down = false, g_pressed = false, g_released = false;
static float g_lmx = 0, g_lmy = 0;     // координаты мыши в локальной системе меню
static int g_id = 0, g_active = -1;
static bool g_hoverClickable = false;
static float g_anim[128];
static int g_binding = -1;             // -1 = нет, 0 = меню, 1 = HUD, 2 = цель, 3 = автоудар
static ULONGLONG g_bindTick = 0;

static bool Pressed(int vk) { return vk > 0 && vk < 256 && g_cur[vk] && !g_prev[vk]; }

static std::wstring KeyName(int vk) {
    switch (vk) {
    case 0: return L"—";
    case VK_MBUTTON: return L"Mouse 3 (колесо)";
    case VK_XBUTTON1: return L"Mouse 4";
    case VK_XBUTTON2: return L"Mouse 5";
    case VK_LSHIFT: return L"Left Shift";
    case VK_RSHIFT: return L"Right Shift";
    case VK_LCONTROL: return L"Left Ctrl";
    case VK_RCONTROL: return L"Right Ctrl";
    case VK_LMENU: return L"Left Alt";
    case VK_RMENU: return L"Right Alt";
    case VK_SPACE: return L"Space";
    case VK_TAB: return L"Tab";
    case VK_RETURN: return L"Enter";
    case VK_CAPITAL: return L"Caps Lock";
    }
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LONG lp = (LONG)(sc << 16);
    switch (vk) {
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN: case VK_NUMLOCK: case VK_DIVIDE:
        lp |= (1 << 24);
    }
    wchar_t buf[64];
    if (GetKeyNameTextW(lp, buf, 64) > 0) return buf;
    return L"VK " + std::to_wstring(vk);
}

// ======================= Виджеты меню (immediate mode) =======================
static bool g_clipOn = false;
static float g_clipT = 0, g_clipB = 0;
// ======================= Время / частота кадров =======================
static double NowMs() {
    static LARGE_INTEGER f = {};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}
static double g_frameMs = 16.0;      // минимальный интервал между кадрами (под частоту монитора)
static float  g_dtScale = 1.0f;      // длительность кадра в "кадрах по 16.7 мс"
static float AnimK(float k) { return 1.0f - powf(1.0f - k, g_dtScale); }

// ======================= Звук переключателей =======================
// Короткий синтезированный "клик" (не системные звуки Windows), играет из памяти.
// Включение — восходящий тон, выключение — нисходящий.
static std::vector<unsigned char> g_sndOn, g_sndOff;

static std::vector<unsigned char> MakeClickWav(double f0, double f1, double ms) {
    const int sr = 44100;
    const int n = (int)(sr * ms / 1000.0);
    std::vector<unsigned char> w(44 + (size_t)n * 2);
    auto p32 = [&](size_t off, uint32_t v) { memcpy(&w[off], &v, 4); };
    auto p16 = [&](size_t off, uint16_t v) { memcpy(&w[off], &v, 2); };
    memcpy(&w[0], "RIFF", 4);  p32(4, 36 + (uint32_t)n * 2);
    memcpy(&w[8], "WAVEfmt ", 8);
    p32(16, 16); p16(20, 1); p16(22, 1); p32(24, sr); p32(28, sr * 2); p16(32, 2); p16(34, 16);
    memcpy(&w[36], "data", 4); p32(40, (uint32_t)n * 2);
    const double twoPi = 6.283185307179586;
    double ph = 0.0;
    for (int i = 0; i < n; ++i) {
        double t = (double)i / n;
        double f = f0 + (f1 - f0) * t;
        ph += twoPi * f / sr;
        double att = std::min(1.0, i / (sr * 0.003));          // мягкая атака 3 мс
        double env = att * pow(1.0 - t, 2.4);                  // быстрое затухание
        double v = (sin(ph) + 0.30 * sin(2.0 * ph)) / 1.3 * env * 0.32;
        int16_t sm = (int16_t)(v * 32767.0);
        memcpy(&w[44 + (size_t)i * 2], &sm, 2);
    }
    return w;
}

static void PlayToggleSound(bool on) {
    if (!g_cfg.soundOn) return;
    if (g_sndOn.empty()) {
        g_sndOn  = MakeClickWav(900.0, 1350.0, 55.0);
        g_sndOff = MakeClickWav(700.0, 430.0, 55.0);
    }
    const std::vector<unsigned char>& w = on ? g_sndOn : g_sndOff;
    PlaySoundW((LPCWSTR)w.data(), NULL, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

// ======================= Анимация наведения =======================
static float g_hv[256];
static float HoverAnim(int id, bool active) {
    float& a = g_hv[id & 255];
    a += ((active ? 1.0f : 0.0f) - a) * AnimK(0.28f);
    if (a < 0.003f) a = 0.0f;
    return a;
}
static D2D1_COLOR_F Lighten(D2D1_COLOR_F c, float t) {
    return Lerp(c, D2D1::ColorF(1.0f, 1.0f, 1.0f, c.a), std::min(1.0f, std::max(0.0f, t)));
}

static bool Hover(float l, float t, float r, float b) {
    if (g_clipOn && (g_lmy < g_clipT || g_lmy >= g_clipB)) return false;
    return g_lmx >= l && g_lmx < r && g_lmy >= t && g_lmy < b;
}

static bool Clicked(int id, bool hov) {
    if (hov) g_hoverClickable = true;
    if (hov && g_pressed) g_active = id;
    return g_released && hov && g_active == id;
}

static bool ButtonC(float l, float t, float r, float b, const wchar_t* label, unsigned col, bool filled) {
    int id = g_id++;
    bool hov = Hover(l, t, r, b);
    bool fired = Clicked(id, hov);
    float a = HoverAnim(id, hov);
    float e = (g_active == id && g_down) ? -1.0f : a * 2.5f;      // при наведении кнопка увеличивается
    l -= e; t -= e; r += e; b += e;
    if (filled) {
        FillRR(l, t, r, b, 10, Lighten(Col(col, 0.88f + 0.12f * a), a * 0.32f));
        Txt(label, fTextC, l, t, r, b, Col(C_DARK));
    } else {
        FillRR(l, t, r, b, 10, Lighten(Col(col, 0.14f + 0.12f * a), a * 0.30f));
        StrokeRR(l + 0.5f, t + 0.5f, r - 0.5f, b - 0.5f, 10, Lighten(Col(col, 0.45f + 0.40f * a), a * 0.45f), 1.0f);
        Txt(label, fTextC, l, t, r, b, Col(C_TEXT));
    }
    return fired;
}
static bool Button(float l, float t, float r, float b, const wchar_t* label, bool primary) {
    return ButtonC(l, t, r, b, label, primary ? Accent() : C_EDGE, primary);
}

static void Toggle(float l, float t, float r, float b, const wchar_t* label, bool& v) {
    int id = g_id++;
    bool hov = Hover(l, t, r, b);
    if (Clicked(id, hov)) { v = !v; PlayToggleSound(v); }
    float& an = g_anim[id & 127];
    an += ((v ? 1.0f : 0.0f) - an) * AnimK(0.35f);
    float a = HoverAnim(id, hov);
    float e = a * 2.0f;
    FillRR(l - e, t - e, r + e, b + e, 10, Lighten(Col(C_EDGE, 0.05f + 0.07f * a), a * 0.55f));
    Txt(label, fText, l + 12, t, r - 60, b, Col(C_TEXT));
    float cy = (t + b) * 0.5f, tr = r - 12, tl = tr - 40;
    float g = a * 1.5f;                                            // сам переключатель тоже растёт
    FillRR(tl - g, cy - 10 - g, tr + g, cy + 10 + g, 10 + g, Lighten(Lerp(Col(C_OFF), Col(Accent()), an), a * 0.35f));
    Circle(tl + 10 + an * 20.0f, cy, 7.5f + g, Col(0xFFFFFF), true);
}

// Переключатель в заголовке карточки (включение всего модуля)
static void HeaderSwitch(float l, float t, float r, float b, bool& v) {
    int id = g_id++;
    bool hov = Hover(l, t, r, b);
    if (Clicked(id, hov)) { v = !v; PlayToggleSound(v); }
    float& an = g_anim[id & 127];
    an += ((v ? 1.0f : 0.0f) - an) * AnimK(0.35f);
    float a = HoverAnim(id, hov);
    float g = a * 2.0f;
    float cy = (t + b) * 0.5f, tr = r - 16, tl = tr - 46;
    FillRR(tl - g, cy - 12 - g, tr + g, cy + 12 + g, 12 + g, Lighten(Lerp(Col(C_OFF), Col(Accent()), an), a * 0.35f));
    Circle(tl + 12 + an * 22.0f, cy, 9.0f + g * 0.6f, Col(0xFFFFFF), true);
}

static void Slider(float l, float t, float r, float b, const wchar_t* label, float& v, float mn, float mx, const wchar_t* fmt, float mul) {
    int id = g_id++;
    float tl = l + 4, tr = r - 4, ty = b - 10;
    bool hov = Hover(l, ty - 12, r, ty + 12);
    if (hov) g_hoverClickable = true;
    if (hov && g_pressed) g_active = id;
    if (g_active == id && g_down) {
        float k = (g_lmx - tl) / (tr - tl);
        k = std::min(1.0f, std::max(0.0f, k));
        v = mn + k * (mx - mn);
    }
    wchar_t buf[32];
    _snwprintf(buf, 32, fmt, v * mul);
    Txt(label, fText, l + 4, t, r - 80, t + 22, Col(C_TEXT));
    Txt(buf, fTextR, r - 80, t, r - 4, t + 22, Col(Accent()));
    float k = (v - mn) / (mx - mn);
    float kx = tl + k * (tr - tl);
    float a = HoverAnim(id, hov || (g_active == id && g_down));
    FillRR(tl, ty - 3 - a, tr, ty + 3 + a, 3, Lighten(Col(C_EDGE, 0.16f), a * 0.4f));
    FillRR(tl, ty - 3 - a, kx, ty + 3 + a, 3, Lighten(Col(Accent()), a * 0.30f));
    Circle(kx, ty, 8 + a * 3.0f, Col(0xFFFFFF), true);
    Circle(kx, ty, 8 + a * 3.0f, Lighten(Col(Accent()), a * 0.30f), false, 2.0f);
}

static void KeyBind(float l, float t, float r, float b, const wchar_t* label, int idx, int vk) {
    int id = g_id++;
    bool hov = Hover(l, t, r, b);
    if (Clicked(id, hov)) { g_binding = idx; g_bindTick = GetTickCount64(); }
    float a = HoverAnim(id, hov);
    float e = a * 2.0f;
    FillRR(l - e, t - e, r + e, b + e, 10, Lighten(Col(C_EDGE, 0.05f + 0.07f * a), a * 0.55f));
    float bl = r - 160 - e, br = r - 8 + e, bt = t + 4 - e, bb = b - 4 + e;
    Txt(label, fText, l + 12, t, bl - 4, b, Col(C_TEXT));
    bool binding = (g_binding == idx);
    FillRR(bl, bt, br, bb, 8, binding ? Col(Accent(), 0.18f) : Lighten(Col(C_EDGE, 0.12f + 0.12f * a), a * 0.5f));
    if (binding) StrokeRR(bl, bt, br, bb, 8, Col(Accent()), 1.5f);
    std::wstring name = binding ? L"Нажмите клавишу…" : KeyName(vk);
    Txt(name.c_str(), fTextC, bl, bt, br, bb, binding ? Col(Accent()) : Col(0xFFFFFF));
}

// ---------- Карточки (блоки) и раскладка по колонкам ----------
static int   g_tab = 0;
static float g_scroll = 0, g_scrollMax = 0;
static bool  g_hudEdit = false;             // режим перемещения HUD (меню скрыто)
static float g_MW = 1500, g_MH = 840;       // логический размер меню
static float g_colY[3], g_colX0 = 0, g_colW = 400, g_gap = 16;
static int   g_ncol = 3;
static float g_cx = 0, g_cw = 0, g_cy = 0;  // внутренняя область текущей карточки
static const float kHdr = 52.0f;

static void BeginLayout(float x0, float top, float w, int ncol) {
    g_ncol = ncol; g_colX0 = x0; g_gap = 16.0f;
    g_colW = (w - g_gap * (ncol - 1)) / ncol;
    for (int i = 0; i < 3; ++i) g_colY[i] = top - g_scroll;
}

static float CardH(int toggles, int sliders, int keys, int notes = 0) {
    return kHdr + 10.0f + toggles * 40.0f + sliders * 48.0f + keys * 40.0f + notes * 26.0f + 10.0f;
}

// en != nullptr -> в заголовке переключатель включения модуля
static void BeginCard(const wchar_t* title, float h, bool* en) {
    int c = 0;
    for (int i = 1; i < g_ncol; ++i) if (g_colY[i] < g_colY[c] - 0.5f) c = i;
    float x = g_colX0 + c * (g_colW + g_gap), y = g_colY[c];
    g_colY[c] += h + g_gap;
    FillRR(x, y, x + g_colW, y + h, 16, Col(C_CARD, 0.92f));
    StrokeRR(x + 0.5f, y + 0.5f, x + g_colW - 0.5f, y + h - 0.5f, 16, Col(C_EDGE, 0.10f), 1.0f);
    float tx = x + 18;
    if (en) {
        Circle(x + 22, y + kHdr * 0.5f, 4.5f, *en ? Col(Accent()) : Col(C_OFF), true);
        tx = x + 36;
    }
    Txt(title, fHead, tx, y, x + g_colW - 80, y + kHdr, Col(C_TEXT));
    if (en) HeaderSwitch(x, y, x + g_colW, y + kHdr, *en);
    FillRect(x + 16, y + kHdr - 1, x + g_colW - 16, y + kHdr, Col(C_EDGE, 0.10f));
    g_cx = x + 14; g_cw = g_colW - 28; g_cy = y + kHdr + 10;
}
static void CToggle(const wchar_t* l, bool& v) { Toggle(g_cx, g_cy, g_cx + g_cw, g_cy + 34, l, v); g_cy += 40; }
static void CSlider(const wchar_t* l, float& v, float mn, float mx, const wchar_t* fmt, float mul) {
    Slider(g_cx, g_cy, g_cx + g_cw, g_cy + 42, l, v, mn, mx, fmt, mul); g_cy += 48;
}
static void CKey(const wchar_t* l, int idx, int vk) { KeyBind(g_cx, g_cy, g_cx + g_cw, g_cy + 34, l, idx, vk); g_cy += 40; }
static void CNote(const wchar_t* s) { Txt(s, fSmall, g_cx + 4, g_cy, g_cx + g_cw, g_cy + 22, Col(C_MUTE)); g_cy += 26; }
static bool CButton(const wchar_t* l, unsigned col, bool filled) {
    bool r = ButtonC(g_cx, g_cy, g_cx + g_cw, g_cy + 42, l, col, filled); g_cy += 50; return r;
}
static void CRow(const wchar_t* l, const wchar_t* v, D2D1_COLOR_F vc) {
    Txt(l, fText, g_cx + 6, g_cy, g_cx + 140, g_cy + 26, Col(C_DIM));
    Txt(v, fTextR, g_cx + 140, g_cy, g_cx + g_cw - 6, g_cy + 26, vc);
    g_cy += 28;
}
static void CColors() {
    Txt(L"Цвет акцента", fText, g_cx + 4, g_cy, g_cx + 140, g_cy + 28, Col(C_TEXT));
    for (int i = 0; i < 6; ++i) {
        float cx = g_cx + g_cw - 14 - (5 - i) * 34.0f, cy = g_cy + 14;
        bool hov = Hover(cx - 14, cy - 14, cx + 14, cy + 14);
        int cid = g_id++;
        if (Clicked(cid, hov)) g_cfg.accent = i;
        float ha = HoverAnim(cid, hov);
        Circle(cx, cy, 10 + ha * 2.5f, Lighten(Col(kAccents[i]), ha * 0.35f), true);
        if (g_cfg.accent == i) Circle(cx, cy, 14, Col(0xFFFFFF), false, 2.0f);
    }
    g_cy += 44;
}

// ======================= ESP / Nametag =======================
// Java-мод шлёт по каналу \\.\pipe\mc_esp позицию/скорость камеры и существ (не готовые 2D-координаты).
// Оверлей сам проецирует их на КАЖДЫЙ свой кадр, с лёгкой экстраполяцией по скорости -
// поэтому рамки идут плавно, без дёрганья. Рисуется 3D-бокс со свечением и плашка с ником.
struct EspSlot {
    int dur = -1, count = 0;                 // прочность % (-1 нет), количество
    std::wstring id, name;                   // id предмета (diamond_helmet) и отображаемое имя
    bool empty() const { return name.empty(); }
};
struct EspEnt {
    double x = 0, y = 0, z = 0;              // ноги (интерполированные)
    float vx = 0, vy = 0, vz = 0;            // блоков/сек
    float w = 0.6f, h = 1.8f, hp = 0, maxHp = 20, absorb = 0;
    int isPlayer = 0;
    std::wstring name;
    EspSlot s[6];                            // 0 правая рука, 1 левая, 2 шлем, 3 нагрудник, 4 поножи, 5 ботинки
};
struct EspFrame {
    double cx = 0, cy = 0, cz = 0;
    float yaw = 0, pitch = 0, fov = 70;
    float cvx = 0, cvy = 0, cvz = 0, cvyaw = 0, cvpitch = 0;   // скорости камеры
    double arrival = 0;                      // NowMs() прихода пакета
    std::vector<EspEnt> ents;
};
static CRITICAL_SECTION g_espCs;
static std::shared_ptr<EspFrame> g_espFrame;
static volatile double g_espLastMs = 0;
static HANDLE g_espEvt = NULL;

struct EspReader {
    const unsigned char* p; size_t n; size_t i = 0; bool ok = true;
    template <class T> T get() {
        T v{};
        if (i + sizeof(T) > n) { ok = false; return v; }
        memcpy(&v, p + i, sizeof(T)); i += sizeof(T);
        return v;
    }
    std::wstring str() {
        std::wstring r;
        uint16_t len = get<uint16_t>();
        if (!ok || len > 200 || i + (size_t)len * 2 > n) { ok = false; return r; }
        r.resize(len);
        if (len) memcpy(&r[0], p + i, (size_t)len * 2);
        i += (size_t)len * 2;
        return r;
    }
};

static void ParseEsp(const unsigned char* d, size_t n) {
    EspReader r{ d, n };
    int32_t magic = r.get<int32_t>();
    int32_t cnt = r.get<int32_t>();
    if (!r.ok || magic != 0x3253454D || cnt < 0 || cnt > 128) return;
    auto f = std::make_shared<EspFrame>();
    f->cx = r.get<double>(); f->cy = r.get<double>(); f->cz = r.get<double>();
    f->yaw = r.get<float>(); f->pitch = r.get<float>(); f->fov = r.get<float>();
    f->cvx = r.get<float>(); f->cvy = r.get<float>(); f->cvz = r.get<float>();
    f->cvyaw = r.get<float>(); f->cvpitch = r.get<float>();
    f->ents.reserve((size_t)cnt);
    for (int k = 0; k < cnt; ++k) {
        EspEnt e;
        e.x = r.get<double>(); e.y = r.get<double>(); e.z = r.get<double>();
        e.vx = r.get<float>(); e.vy = r.get<float>(); e.vz = r.get<float>();
        e.w = r.get<float>(); e.h = r.get<float>();
        e.hp = r.get<float>(); e.maxHp = r.get<float>(); e.absorb = r.get<float>();
        e.isPlayer = r.get<int32_t>();
        e.name = r.str();
        for (int j = 0; j < 6; ++j) {
            e.s[j].dur = r.get<int32_t>(); e.s[j].count = r.get<int32_t>();
            e.s[j].id = r.str(); e.s[j].name = r.str();
        }
        if (!r.ok) return;
        f->ents.push_back(std::move(e));
    }
    f->arrival = NowMs();
    EnterCriticalSection(&g_espCs);
    g_espFrame = f;
    LeaveCriticalSection(&g_espCs);
    g_espLastMs = f->arrival;
    if (g_espEvt) SetEvent(g_espEvt);       // будим главный цикл: рисуем сразу, не ждём таймер
}

DWORD WINAPI EspPipeThread(LPVOID) {
    const wchar_t* pipeName = L"\\\\.\\pipe\\mc_esp";
    std::vector<unsigned char> buf(65536);
    while (g_running) {
        HANDLE h = CreateNamedPipeW(pipeName, PIPE_ACCESS_INBOUND,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1, 0, 65536, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) { Sleep(500); continue; }
        BOOL ok = ConnectNamedPipe(h, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (ok && g_running) {
            while (g_running) {
                DWORD got = 0;
                if (!ReadFile(h, buf.data(), (DWORD)buf.size(), &got, NULL)) break;
                if (got > 0) ParseEsp(buf.data(), got);
            }
        }
        DisconnectNamedPipe(h);
        CloseHandle(h);
        EnterCriticalSection(&g_espCs);
        g_espFrame.reset();
        LeaveCriticalSection(&g_espCs);
    }
    return 0;
}

static float TextWidth(const std::wstring& t, IDWriteTextFormat* f) {
    if (t.empty()) return 0.0f;
    IDWriteTextLayout* l = nullptr;
    if (FAILED(g_dw->CreateTextLayout(t.c_str(), (UINT32)t.size(), f, 1000.0f, 100.0f, &l)) || !l) return 6.0f * t.size();
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    l->Release();
    return m.widthIncludingTrailingWhitespace;
}
static void TxtClip(const wchar_t* t, IDWriteTextFormat* f, float l, float tp, float r, float b, D2D1_COLOR_F c) {
    g_brush->SetColor(c);
    g_rt->DrawText(t, (UINT32)wcslen(t), f, D2D1::RectF(l, tp, r, b), g_brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}
static D2D1_COLOR_F HealthColor(float frac) {
    frac = std::min(1.0f, std::max(0.0f, frac));
    D2D1_COLOR_F red = Col(0xFF5C5C), yel = Col(0xFBBF24), grn = Col(0x34D399);
    return frac < 0.5f ? Lerp(red, yel, frac * 2.0f) : Lerp(yel, grn, (frac - 0.5f) * 2.0f);
}
static unsigned MaterialColor(const std::wstring& id) {
    if (id.find(L"netherite") == 0) return 0x9A8794;
    if (id.find(L"diamond") == 0)   return 0x4FE3E0;
    if (id.find(L"golden") == 0 || id.find(L"gold") == 0) return 0xFFD34D;
    if (id.find(L"iron") == 0)      return 0xDDE3EA;
    if (id.find(L"chainmail") == 0) return 0x9AA0A6;
    if (id.find(L"leather") == 0)   return 0xB9784A;
    if (id.find(L"turtle") == 0)    return 0x4ADE80;
    return 0xC8D0DA;
}

// Слот 22x22: пустой = "×", предмет = цветная плитка + полоска прочности + количество
static void DrawSlot(float x, float y, const EspSlot& sl, bool armor) {
    const float S = 22.0f;
    if (sl.empty()) {
        Txt(L"×", fBadge, x, y, x + S, y + S, Col(0xFFFFFF, 0.28f));
        return;
    }
    unsigned c = armor ? MaterialColor(sl.id) : 0xFFFFFF;
    FillRR(x, y, x + S, y + S, 6, Col(c, armor ? 0.30f : 0.16f));
    StrokeRR(x + 0.5f, y + 0.5f, x + S - 0.5f, y + S - 0.5f, 6, Col(c, 0.75f), 1.0f);
    if (!armor) {
        wchar_t ch[2] = { (wchar_t)towupper(sl.name[0]), 0 };
        Txt(ch, fBadge, x, y, x + S, y + S, Col(0xFFFFFF));
    }
    if (sl.dur >= 0 && sl.dur < 100) {
        float f = sl.dur / 100.0f;
        FillRR(x + 3, y + S - 5, x + S - 3, y + S - 3, 1, Col(0x000000, 0.45f));
        FillRR(x + 3, y + S - 5, x + 3 + (S - 6) * f, y + S - 3, 1, HealthColor(f));
    }
    if (sl.count > 1) {
        wchar_t b[8]; _snwprintf(b, 8, L"%d", sl.count);
        Txt(b, fTinyC, x, y - 1, x + S - 1, y + S - 1, Col(0xFFFFFF, 0.95f));
    }
}

// Одна компактная строка: [рука] Ник [20HP] [рука][шлем][грудь][ноги][ботинки]
static void DrawNametag(const EspEnt& e, float cx, float topY, float W, float H, float dist) {
    const float PH = 30.0f, S = 22.0f, G = 4.0f;
    std::wstring hpTxt;
    float hpW = 0;
    if (g_cfg.ntHp) {
        wchar_t b[24];
        _snwprintf(b, 24, L"%.0fHP", ceilf(e.hp));
        hpTxt = b;
        hpW = TextWidth(hpTxt, fBadge) + 14.0f;
    }
    float nameW = TextWidth(e.name, fNameL);
    std::wstring distTxt;
    float distW = 0;
    if (g_cfg.ntDist) {
        wchar_t b[24]; _snwprintf(b, 24, L"%.0fм", dist);
        distTxt = b; distW = TextWidth(distTxt, fSmall) + 4.0f;
    }

    // ширина левого слота (левая рука) и правой группы
    auto handW = [&](const EspSlot& sl) -> float {
        if (sl.empty()) return S;
        float tw = std::min(78.0f, TextWidth(sl.name, fSmall));
        return S + 5.0f + tw + 2.0f;
    };
    float leftW = g_cfg.ntHands ? handW(e.s[1]) : 0.0f;
    float rightHandW = g_cfg.ntHands ? handW(e.s[0]) : 0.0f;
    float rightW = rightHandW + (g_cfg.ntArmor ? 4 * S + 3 * G + (g_cfg.ntHands ? G : 0) : 0.0f);

    float PW = 10.0f;
    if (leftW > 0) PW += leftW + 8.0f;
    PW += nameW;
    if (distW > 0) PW += 6.0f + distW;
    if (hpW > 0) PW += 8.0f + hpW;
    if (rightW > 0) PW += 8.0f + rightW;
    PW += 10.0f;

    float sc = std::max(0.72f, std::min(1.05f, 1.30f - dist / 55.0f));
    float ox = cx - PW * sc * 0.5f, oy = topY - 8.0f - PH * sc;
    ox = std::max(4.0f, std::min(ox, W - PW * sc - 4.0f));
    oy = std::max(4.0f, std::min(oy, H - PH * sc - 4.0f));
    SetXform(sc, ox, oy);

    for (int i = 3; i >= 1; --i)
        FillRR(-i * 1.5f, -i * 1.5f + 3, PW + i * 1.5f, PH + i * 1.5f + 3, 13.0f + i, Col(0x000000, 0.05f));
    FillRR(0, 0, PW, PH, 13, Col(0x0D0D12, 0.86f));

    float x = 10.0f, ty = (PH - S) * 0.5f;
    auto drawHand = [&](const EspSlot& sl, float w) {
        DrawSlot(x, ty, sl, false);
        if (!sl.empty())
            TxtClip(sl.name.c_str(), fSmall, x + S + 5.0f, 0, x + w, PH, Col(0xFFFFFF, 0.85f));
        x += w;
    };
    if (leftW > 0) { drawHand(e.s[1], leftW); x += 8.0f; }

    Txt(e.name.c_str(), fNameL, x, 0, x + nameW + 4.0f, PH, Col(0xFFFFFF));
    x += nameW;
    if (distW > 0) { x += 6.0f; Txt(distTxt.c_str(), fSmall, x, 0, x + distW, PH, Col(0xFFFFFF, 0.5f)); x += distW; }

    if (hpW > 0) {
        x += 8.0f;
        float frac = e.maxHp > 0.01f ? e.hp / e.maxHp : 0.0f;
        D2D1_COLOR_F hc = HealthColor(frac);
        FillRR(x, 5, x + hpW, PH - 5, 8, Col(0x000000, 0.0f));
        FillRR(x, 5, x + hpW, PH - 5, 8, D2D1::ColorF(hc.r, hc.g, hc.b, 0.28f));
        Txt(hpTxt.c_str(), fBadge, x, 5, x + hpW, PH - 5, Lighten(hc, 0.35f));
        x += hpW;
    }

    if (rightW > 0) {
        x += 8.0f;
        if (g_cfg.ntHands) { drawHand(e.s[0], rightHandW); x += G; }
        if (g_cfg.ntArmor) {
            for (int i = 0; i < 4; ++i) { DrawSlot(x, ty, e.s[2 + i], true); x += S + G; }
        }
    }
    SetXform(1.0f, 0, 0);
}

static void DrawGlowLine(float x1, float y1, float x2, float y2, bool glow, unsigned col) {
    if (glow) {
        Line(x1, y1, x2, y2, Col(col, 0.10f), 7.0f);
        Line(x1, y1, x2, y2, Col(col, 0.22f), 4.0f);
    }
    Line(x1, y1, x2, y2, Col(col, 0.97f), 1.5f);
}

static void DrawEsp(float W, float H) {
    if (!g_cfg.espOn && !g_cfg.nametagOn) return;
    std::shared_ptr<EspFrame> fr;
    EnterCriticalSection(&g_espCs);
    fr = g_espFrame;
    LeaveCriticalSection(&g_espCs);
    if (!fr) return;
    double now = NowMs();
    if (now - fr->arrival > 1500.0) return;

    // Экстраполяция до момента отрисовки (компенсирует задержку конвейера)
    double dt = std::min(0.030, std::max(0.0, (now - fr->arrival + 3.0) * 0.001));
    double cx = fr->cx + fr->cvx * dt, cy = fr->cy + fr->cvy * dt, cz = fr->cz + fr->cvz * dt;
    double yaw = (fr->yaw + fr->cvyaw * dt) * 3.14159265358979323846 / 180.0;
    double pit = (fr->pitch + fr->cvpitch * dt) * 3.14159265358979323846 / 180.0;
    double cyw = cos(yaw), syw = sin(yaw), cpt = cos(pit), spt = sin(pit);
    double fx = -syw * cpt, fy = -spt, fz = cyw * cpt;      // вперёд
    double rx = -cyw, ry = 0.0, rz = -syw;                  // вправо
    double ux = ry * fz - rz * fy, uy = rz * fx - rx * fz, uz = rx * fy - ry * fx;   // вверх
    double tanH = tan(fr->fov * 3.14159265358979323846 / 360.0);
    double aspect = (double)W / std::max(1.0f, H);
    const double NEAR_Z = 0.05;

    struct P3 { double x, y, z; };
    auto toCam = [&](double wx, double wy, double wz) -> P3 {
        double dx = wx - cx, dy = wy - cy, dz = wz - cz;
        return { dx * rx + dy * ry + dz * rz, dx * ux + dy * uy + dz * uz, dx * fx + dy * fy + dz * fz };
    };
    auto proj = [&](const P3& p, float& sx, float& sy) {
        sx = (float)((p.x / (p.z * tanH * aspect) + 1.0) * 0.5 * W);
        sy = (float)((1.0 - p.y / (p.z * tanH)) * 0.5 * H);
    };

    // дальние первыми
    std::vector<const EspEnt*> order;
    for (const EspEnt& e : fr->ents) order.push_back(&e);
    auto d2 = [&](const EspEnt* e) { double dx = e->x - cx, dy = e->y - cy, dz = e->z - cz; return dx * dx + dy * dy + dz * dz; };
    std::sort(order.begin(), order.end(), [&](const EspEnt* a, const EspEnt* b) { return d2(a) > d2(b); });

    SetXform(1.0f, 0, 0);
    static const int edges[12][2] = { {0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7} };
    for (const EspEnt* pe : order) {
        const EspEnt& e = *pe;
        double ex = e.x + e.vx * dt, ey = e.y + e.vy * dt, ez = e.z + e.vz * dt;
        double hw = e.w * 0.5, hh = e.h;
        unsigned col = e.isPlayer ? 0xFFFFFF : 0xFFD97A;

        if (g_cfg.espOn) {
            P3 c[8];
            for (int i = 0; i < 8; ++i)
                c[i] = toCam(ex + ((i & 1) ? hw : -hw), ey + ((i & 2) ? hh : 0.0), ez + ((i & 4) ? hw : -hw));
            for (int k = 0; k < 12; ++k) {
                P3 a = c[edges[k][0]], b = c[edges[k][1]];
                if (a.z < NEAR_Z && b.z < NEAR_Z) continue;
                if (a.z < NEAR_Z) { double t = (NEAR_Z - a.z) / (b.z - a.z); a = { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, NEAR_Z }; }
                if (b.z < NEAR_Z) { double t = (NEAR_Z - b.z) / (a.z - b.z); b = { b.x + (a.x - b.x) * t, b.y + (a.y - b.y) * t, NEAR_Z }; }
                float x1, y1, x2, y2;
                proj(a, x1, y1); proj(b, x2, y2);
                if ((x1 < -2000 && x2 < -2000) || (x1 > W + 2000 && x2 > W + 2000)) continue;
                DrawGlowLine(x1, y1, x2, y2, g_cfg.espFill, col);
            }
        }
        if (g_cfg.nametagOn) {
            P3 t = toCam(ex, ey + hh + 0.1, ez);
            if (t.z > NEAR_Z) {
                float sx, sy;
                proj(t, sx, sy);
                double dx = ex - cx, dy = ey - cy, dz = ez - cz;
                DrawNametag(e, sx, sy, W, H, (float)sqrt(dx * dx + dy * dy + dz * dz));
            }
        }
    }
}

// ======================= HUD =======================
static float g_hudW = 276, g_hudH = 200;
static bool g_dragging = false;
static float g_dragOffX = 0, g_dragOffY = 0;

static double WrapDegrees(double v) {
    while (v > 180.0) v -= 360.0;
    while (v < -180.0) v += 360.0;
    return v;
}

static void DrawTargetMarker(float W, float H, const PlayerCoords& c) {
    if (!g_cfg.showPlayerNew || c.hasTarget == 0) return;

    const double pi = 3.14159265358979323846;
    const double eyeY = c.y + 1.62;
    const double dx = c.targetX - c.x;
    const double dy = c.targetY - eyeY;
    const double dz = c.targetZ - c.z;
    const double horizontal = hypot(dx, dz);
    if (horizontal < 0.001) return;

    const double targetYaw = atan2(-dx, dz) * 180.0 / pi;
    const double targetPitch = -atan2(dy, horizontal) * 180.0 / pi;
    const double yawError = WrapDegrees(targetYaw - c.yaw);
    const double pitchError = targetPitch - c.pitch;
    if (fabs(yawError) >= 89.0 || fabs(pitchError) >= 89.0) return;

    // Approximate Minecraft's view projection for a HUD-only marker.
    const double horizontalFov = 90.0;
    const double verticalFov = horizontalFov * H / std::max(1.0f, W);
    const double px = tan(yawError * pi / 180.0) / tan(horizontalFov * pi / 360.0);
    const double py = tan(pitchError * pi / 180.0) / tan(verticalFov * pi / 360.0);
    const float sx = (float)(W * 0.5 + px * W * 0.5);
    const float sy = (float)(H * 0.5 - py * H * 0.5);
    if (sx < -40 || sx > W + 40 || sy < -40 || sy > H + 40) return;

    unsigned acc = Accent();
    Circle(sx, sy, 14.0f, Col(acc, 0.95f), false, 2.0f);
    Line(sx - 22, sy, sx - 7, sy, Col(acc, 0.8f), 1.5f);
    Line(sx + 7, sy, sx + 22, sy, Col(acc, 0.8f), 1.5f);
    Line(sx, sy - 22, sx, sy - 7, Col(acc, 0.8f), 1.5f);
    Line(sx, sy + 7, sx, sy + 22, Col(acc, 0.8f), 1.5f);
    Txt(L"PlayerNew", fSmall, sx + 18, sy - 12, sx + 110, sy + 12, Col(0xFFFFFF, 0.9f));
}

static void DrawHud(float W, float H) {
    PlayerCoords c;
    EnterCriticalSection(&g_cs);
    c = g_coords;
    LeaveCriticalSection(&g_cs);
    bool conn = g_connected && (GetTickCount64() - g_lastPacket < 2000);
    bool have = g_haveData != FALSE;

    const float pw = 276.0f;
    float s = g_cfg.scale;
    bool hint = g_menuOpen || !conn || (GetTickCount64() - g_startTick < 10000);

    float h = 48.0f;
    if (g_cfg.showXYZ) h += 3 * 30.0f;
    if (g_cfg.showBlock) h += 26.0f;
    if (g_cfg.showAngles) h += 28.0f;
    if (g_cfg.showDir) h += 32.0f;
    if (g_cfg.showPlayerNew) h += 78.0f;
    h += 26.0f;                       // строка статуса автоудара
    if (hint) h += 26.0f;
    h += 10.0f;

    g_hudW = pw * s; g_hudH = h * s;
    g_cfg.hudX = (int)std::max(0.0f, std::min((float)g_cfg.hudX, W - g_hudW));
    g_cfg.hudY = (int)std::max(0.0f, std::min((float)g_cfg.hudY, H - g_hudH));
    SetXform(s, (float)g_cfg.hudX, (float)g_cfg.hudY);

    unsigned acc = Accent();
    for (int i = 5; i >= 1; --i)
        FillRR(-i * 2.0f, -i * 2.0f + 4, pw + i * 2.0f, h + i * 2.0f + 4, 14.0f + i * 2.0f, Col(0x000000, 0.05f));
    FillRR(0, 0, pw, h, 14, Col(0x0B1B2E, g_cfg.opacity));
    StrokeRR(0.5f, 0.5f, pw - 0.5f, h - 0.5f, 14, g_menuOpen ? Col(acc, 0.9f) : Col(0xFFFFFF, 0.09f), g_menuOpen ? 1.5f : 1.0f);
    FillRR(0, 14, 3, 26, 1.5f, Col(acc));

    // Заголовок
    Circle(22, 20, 5, conn ? Col(0x34D399) : Col(0xF87171), true);
    Txt(L"COORDS", fTitle, 36, 0, 160, 40, Col(0xFFFFFF));
    if (g_cfg.showStatus)
        Txt(conn ? L"в игре" : L"ожидание", fSmallR, pw - 130, 0, pw - 16, 40, conn ? Col(0x34D399) : Col(0xF87171));
    FillRect(16, 40, pw - 16, 41, Col(0xFFFFFF, 0.08f));

    float y = 48.0f;
    D2D1_COLOR_F valCol = (conn || have) ? Col(0xFFFFFF, conn ? 1.0f : 0.55f) : Col(0xFFFFFF, 0.4f);
    wchar_t buf[96];

    if (g_cfg.showXYZ) {
        const wchar_t* ax[3] = { L"X", L"Y", L"Z" };
        unsigned axc[3] = { 0xFF6B6B, 0x6BFF9E, 0x6BA8FF };
        double v[3] = { c.x, c.y, c.z };
        for (int i = 0; i < 3; ++i) {
            Txt(ax[i], fMonoL, 20, y, 50, y + 30, Col(axc[i]));
            if (have) _snwprintf(buf, 96, L"%.3f", v[i]); else wcscpy(buf, L"—");
            Txt(buf, fMono, 50, y, pw - 20, y + 30, valCol);
            y += 30.0f;
        }
    }
    if (g_cfg.showBlock) {
        Txt(L"Блок", fSmall, 20, y, 90, y + 26, Col(0x8FB6D9));
        if (have) _snwprintf(buf, 96, L"%d   %d   %d", (int)floor(c.x), (int)floor(c.y), (int)floor(c.z)); else wcscpy(buf, L"—");
        Txt(buf, fSmallR, 90, y, pw - 20, y + 26, Col(0xDCEEFF));
        y += 26.0f;
    }
    if (g_cfg.showAngles) {
        if (have) _snwprintf(buf, 96, L"Yaw  %.1f°", c.yaw); else wcscpy(buf, L"Yaw  —");
        Txt(buf, fSmall, 20, y, pw / 2, y + 28, Col(0xDCEEFF));
        if (have) _snwprintf(buf, 96, L"Pitch  %.1f°", c.pitch); else wcscpy(buf, L"Pitch  —");
        Txt(buf, fSmallR, pw / 2, y, pw - 20, y + 28, Col(0xDCEEFF));
        y += 28.0f;
    }
    if (g_cfg.showDir) {
        static const wchar_t* abbr[8] = { L"S", L"SW", L"W", L"NW", L"N", L"NE", L"E", L"SE" };
        static const wchar_t* ru[8] = { L"юг", L"юго-запад", L"запад", L"северо-запад", L"север", L"северо-восток", L"восток", L"юго-восток" };
        float yaw = fmodf(c.yaw, 360.0f);
        if (yaw < 0) yaw += 360.0f;
        int idx = ((int)floorf((yaw + 22.5f) / 45.0f)) % 8;
        float cx = 32, cy = y + 16, rad = 10;
        Circle(cx, cy, rad, Col(0xFFFFFF, 0.25f), false, 1.2f);
        Line(cx, cy - rad, cx, cy - rad + 3, Col(0xFFFFFF, 0.5f), 1.5f);
        float yr = yaw * 3.14159265f / 180.0f;
        Line(cx, cy, cx - sinf(yr) * 8.0f, cy + cosf(yr) * 8.0f, Col(acc), 2.0f);
        Circle(cx - sinf(yr) * 8.0f, cy + cosf(yr) * 8.0f, 2.2f, Col(acc), true);
        if (have) _snwprintf(buf, 96, L"%ls · %ls", abbr[idx], ru[idx]); else wcscpy(buf, L"—");
        Txt(buf, fTextR, 52, y, pw - 20, y + 32, Col(0xFFFFFF));
        y += 32.0f;
    }
    if (g_cfg.showPlayerNew) {
        bool target = have && c.hasTarget != 0;
        bool active = target && c.aiming != 0;
        FillRect(16, y, pw - 16, y + 1, Col(0xFFFFFF, 0.08f));
        Txt(L"PLAYERNEW", fSec, 20, y + 8, 100, y + 28, Col(0x8FB6D9));
        Txt(target ? (active ? L"наведение" : L"найдена") : L"нет цели",
            fSmallR, 120, y + 8, pw - 20, y + 28,
            active ? Col(acc) : (target ? Col(0xFBBF24) : Col(0x6F93B5)));
        if (target) {
            _snwprintf(buf, 96, L"XYZ  %.1f  %.1f  %.1f", c.targetX, c.targetY, c.targetZ);
            Txt(buf, fSmall, 20, y + 31, pw - 20, y + 51, Col(0xDCEEFF));
            _snwprintf(buf, 96, L"Yaw %.1f°   Pitch %.1f°", c.aimYaw, c.aimPitch);
            Txt(buf, fSmall, 20, y + 52, pw - 20, y + 72, Col(0xDCEEFF));
        }
        y += 78.0f;
    }
    {
        wchar_t ab[64];
        _snwprintf(ab, 64, L"Автоудар: %ls  ·  %.1f бл.", g_cfg.autoOn ? L"ВКЛ" : L"выкл", (double)g_cfg.autoDist);
        Txt(ab, fSmall, 20, y, pw - 16, y + 26, g_cfg.autoOn ? Col(acc) : Col(0x6F93B5));
        y += 26.0f;
    }
    if (hint) {
        std::wstring t;
        if (g_menuOpen) t = L"Перетащите панель мышью";
        else if (!conn) t = L"Ожидание Minecraft…  Меню: " + KeyName(g_cfg.keyMenu);
        else t = L"Меню настроек: " + KeyName(g_cfg.keyMenu);
        Txt(t.c_str(), fSmall, 20, y, pw - 16, y + 26, Col(0x6F93B5));
    }
    SetXform(1.0f, 0, 0);
}

// ======================= Меню =======================
static void MenuPlacement(float W, float H, float& ox, float& oy, float& ms) {
    ms = std::min(W / 1600.0f, H / 900.0f);
    ms = std::max(0.7f, std::min(1.6f, ms));
    g_MW = floorf(W * 0.95f / ms);
    g_MH = floorf(H * 0.93f / ms);
    ox = floorf((W - g_MW * ms) * 0.5f);
    oy = floorf((H - g_MH * ms) * 0.5f);
}

static void ForceForeground(HWND h) {
    if (!h) return;
    HWND fg = GetForegroundWindow();
    DWORD ft = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
    DWORD mt = GetCurrentThreadId();
    if (ft && ft != mt) AttachThreadInput(mt, ft, TRUE);
    SetForegroundWindow(h);
    if (ft && ft != mt) AttachThreadInput(mt, ft, FALSE);
}

static void SetMenuOpen(bool open, bool refocusGame) {
    if (open == g_menuOpen) return;
    g_menuOpen = open;
    LONG_PTR ex = GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE);
    if (open) {
        ex &= ~(LONG_PTR)(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
        SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, ex);
        SetWindowPos(g_hwnd, HWND_TOPMOST, g_x, g_y, g_w, g_h, SWP_FRAMECHANGED);
        ShowWindow(g_hwnd, SW_SHOW);
        BringWindowToTop(g_hwnd);
        g_visible = true;
        g_menuTick = GetTickCount64();
        ForceForeground(g_hwnd);
    } else {
        ex |= (WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
        SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, ex);
        SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        g_binding = -1;
        g_dragging = false;
        g_hudEdit = false;
        g_active = -1;
        SaveConfig();
        if (refocusGame && g_game) ForceForeground(g_game);
    }
}

// Panic: мгновенно выключает функции, скрывает HUD/маркер, закрывает меню
static void DoPanic() {
    g_cfg.autoOn = false;
    g_cfg.aimOn = false;
    g_cfg.espOn = false;
    g_cfg.nametagOn = false;
    g_cfg.hudOn = false;
    g_cfg.showPlayerNew = false;
    g_binding = -1;
    g_hudEdit = false;
    if (g_menuOpen) SetMenuOpen(false, true);
    SaveConfig();                    // заодно перезаписывает файлы-мосты (enabled=0)
    if (g_cfg.panicExit) g_quit = true;
}

static void ResetConfig() {
    int km = g_cfg.keyMenu, kh = g_cfg.keyHud, kt = g_cfg.keyTarget, ka = g_cfg.keyAuto,
        kaim = g_cfg.keyAim, kp = g_cfg.keyPanic;
    g_cfg = Config();
    g_cfg.keyMenu = km; g_cfg.keyHud = kh; g_cfg.keyTarget = kt; g_cfg.keyAuto = ka;
    g_cfg.keyAim = kaim; g_cfg.keyPanic = kp;
}

// ---------- Вкладки ----------
static void TabCombat() {
    BeginCard(L"Auto Hit", CardH(5, 3, 1, 1), &g_cfg.autoOn);
    CNote(L"Только одиночный мир");
    CToggle(L"Только криты", g_cfg.autoCrit);
    CToggle(L"Игроки", g_cfg.autoPlayers);
    CToggle(L"Враждебные мобы", g_cfg.autoHostile);
    CToggle(L"Мирные мобы", g_cfg.autoPassive);
    CToggle(L"Не бить питомцев", g_cfg.autoIgnoreTamed);
    CSlider(L"Дистанция удара (блоки)", g_cfg.autoDist, 1.0f, 6.0f, L"%.1f", 1.0f);
    CSlider(L"Задержка от (тики)", g_cfg.autoDelayMin, 0.0f, 10.0f, L"%.0f", 1.0f);
    CSlider(L"Задержка до (тики)", g_cfg.autoDelayMax, 0.0f, 10.0f, L"%.0f", 1.0f);
    CKey(L"Клавиша", 3, g_cfg.keyAuto);

    BeginCard(L"Aim Assist", CardH(5, 2, 1, 1), &g_cfg.aimOn);
    CNote(L"Только одиночный мир");
    CToggle(L"Только в прямой видимости", g_cfg.aimLos);
    CToggle(L"Враждебные мобы", g_cfg.aimHostile);
    CToggle(L"Мирные мобы", g_cfg.aimPassive);
    CToggle(L"Не наводить на питомцев", g_cfg.aimTamed);
    CToggle(L"Игроки (тест, LAN)", g_cfg.aimPlayers);
    CSlider(L"Дальность (блоки)", g_cfg.aimRange, 2.0f, 32.0f, L"%.0f", 1.0f);
    CSlider(L"Угол захвата (градусы)", g_cfg.aimFov, 5.0f, 180.0f, L"%.0f", 1.0f);
    CKey(L"Клавиша", 4, g_cfg.keyAim);

    BeginCard(L"Aim Tuning", CardH(0, 5, 0, 1), NULL);
    CNote(L"Параметры плавности наводки");
    CSlider(L"Скорость (град/с)", g_cfg.aimSpeed, 30.0f, 900.0f, L"%.0f", 1.0f);
    CSlider(L"Радиус замедления (град)", g_cfg.aimSlow, 5.0f, 90.0f, L"%.0f", 1.0f);
    CSlider(L"Разброс точки", g_cfg.aimSpread, 0.0f, 1.0f, L"%.0f%%", 100.0f);
    CSlider(L"Дрожь", g_cfg.aimJitter, 0.0f, 1.0f, L"%.0f%%", 100.0f);
    CSlider(L"Резкие рывки (в сек)", g_cfg.aimFlick, 0.0f, 3.0f, L"%.1f", 1.0f);
}

static void TabVisuals() {
    BeginCard(L"HUD", CardH(1, 2, 1, 0) + 50.0f, &g_cfg.hudOn);
    CToggle(L"Только поверх игры", g_cfg.onlyInGame);
    CSlider(L"Прозрачность панели", g_cfg.opacity, 0.2f, 1.0f, L"%.0f%%", 100.0f);
    CSlider(L"Размер HUD", g_cfg.scale, 0.7f, 2.0f, L"%.0f%%", 100.0f);
    CKey(L"Клавиша", 1, g_cfg.keyHud);
    if (CButton(L"Переместить HUD", Accent(), false)) g_hudEdit = true;

    BeginCard(L"ESP (через стены)", CardH(3, 1, 0, 1), &g_cfg.espOn);
    CNote(L"Только одиночный мир");
    CToggle(L"Игроки", g_cfg.espPlayers);
    CToggle(L"Мобы", g_cfg.espMobs);
    CToggle(L"Свечение рамки", g_cfg.espFill);
    CSlider(L"Дальность (блоки)", g_cfg.espRange, 8.0f, 128.0f, L"%.0f", 1.0f);

    BeginCard(L"Nametag", CardH(4, 0, 0, 1), &g_cfg.nametagOn);
    CNote(L"Одна строка: ник, HP, руки, броня");
    CToggle(L"Здоровье", g_cfg.ntHp);
    CToggle(L"Броня", g_cfg.ntArmor);
    CToggle(L"Предметы в руках", g_cfg.ntHands);
    CToggle(L"Дистанция", g_cfg.ntDist);

    BeginCard(L"PlayerNew Target", CardH(0, 0, 1, 2), &g_cfg.showPlayerNew);
    CNote(L"Маркер цели на экране и блок в HUD");
    CNote(L"Подсвечивает найденную цель");
    CKey(L"Клавиша", 2, g_cfg.keyTarget);

    BeginCard(L"Тема и интерфейс", CardH(1, 0, 1, 0) + 44.0f, NULL);
    CColors();
    CToggle(L"Звук переключателей", g_cfg.soundOn);
    CKey(L"Клавиша меню", 0, g_cfg.keyMenu);
}

static void TabPlayer() {
    PlayerCoords c;
    EnterCriticalSection(&g_cs);
    c = g_coords;
    LeaveCriticalSection(&g_cs);
    bool conn = g_connected && (GetTickCount64() - g_lastPacket < 2000);
    bool have = g_haveData != FALSE;

    BeginCard(L"Player Info", CardH(5, 0, 0, 1), NULL);
    CNote(L"Что показывать в панели HUD");
    CToggle(L"Координаты XYZ", g_cfg.showXYZ);
    CToggle(L"Координаты блока", g_cfg.showBlock);
    CToggle(L"Углы Yaw / Pitch", g_cfg.showAngles);
    CToggle(L"Направление", g_cfg.showDir);
    CToggle(L"Статус подключения", g_cfg.showStatus);

    BeginCard(L"Live", kHdr + 20.0f + 8 * 28.0f, NULL);
    wchar_t b[64];
    CRow(L"Статус", conn ? L"в игре" : L"ожидание", conn ? Col(0x34D399) : Col(0xF87171));
    D2D1_COLOR_F vc = Col(C_TEXT);
    const wchar_t* ax[3] = { L"X", L"Y", L"Z" };
    double v[3] = { c.x, c.y, c.z };
    for (int i = 0; i < 3; ++i) {
        if (have) _snwprintf(b, 64, L"%.3f", v[i]); else wcscpy(b, L"—");
        CRow(ax[i], b, vc);
    }
    if (have) _snwprintf(b, 64, L"%.1f°", c.yaw); else wcscpy(b, L"—");
    CRow(L"Yaw", b, vc);
    if (have) _snwprintf(b, 64, L"%.1f°", c.pitch); else wcscpy(b, L"—");
    CRow(L"Pitch", b, vc);
    bool target = have && c.hasTarget != 0;
    CRow(L"PlayerNew", target ? (c.aiming ? L"наведение" : L"найдена") : L"нет цели",
         c.aiming && target ? Col(Accent()) : (target ? Col(0xFBBF24) : Col(C_MUTE)));
    CRow(L"Автоудар", g_cfg.autoOn ? L"ВКЛ" : L"выкл", g_cfg.autoOn ? Col(Accent()) : Col(C_MUTE));
}

static void TabPanic() {
    BeginCard(L"Panic", kHdr + 10.0f + 2 * 26.0f + 50.0f + 40.0f + 40.0f + 10.0f, NULL);
    CNote(L"Выключает автоудар и наводку, прячет HUD,");
    CNote(L"маркер и закрывает меню. Файлы-мосты → 0.");
    if (CButton(L"PANIC — отключить всё", C_DANGER, true)) DoPanic();
    CKey(L"Клавиша паники", 5, g_cfg.keyPanic);
    CToggle(L"Закрыть программу", g_cfg.panicExit);

    BeginCard(L"Конфигурация", kHdr + 10.0f + 2 * 26.0f + 2 * 50.0f + 10.0f, NULL);
    CNote(L"Настройки хранятся в MCOverlay.ini");
    CNote(L"Клавиши при сбросе сохраняются");
    if (CButton(L"Сбросить настройки", Accent(), false)) ResetConfig();
    if (CButton(L"Выход из программы", C_DANGER, false)) g_quit = true;

    BeginCard(L"Горячие клавиши", kHdr + 20.0f + 6 * 28.0f + 26.0f, NULL);
    CRow(L"Меню", KeyName(g_cfg.keyMenu).c_str(), Col(C_TEXT));
    CRow(L"HUD", KeyName(g_cfg.keyHud).c_str(), Col(C_TEXT));
    CRow(L"Цель PlayerNew", KeyName(g_cfg.keyTarget).c_str(), Col(C_TEXT));
    CRow(L"Auto Hit", KeyName(g_cfg.keyAuto).c_str(), Col(C_TEXT));
    CRow(L"Aim Assist", KeyName(g_cfg.keyAim).c_str(), Col(C_TEXT));
    CRow(L"Panic", KeyName(g_cfg.keyPanic).c_str(), Col(C_DANGER));
    CNote(L"Вернуть HUD после паники — клавиша HUD");
}

// ======================= Выбор процесса и внедрение агента =======================
struct ProcEntry { DWORD pid; std::wstring exe, title; };
static std::vector<ProcEntry> g_procs;
static int g_stage = 0;                 // 0 = выбор процесса, 1 = внедрение, 2 = готово (основное меню)
static DWORD g_selPid = 0;                 // PID выбранного клиентского процесса (окно)
static DWORD g_agentPid = 0;               // PID JVM, в которую отправляется Java-agent
static HANDLE g_injProc = NULL;
static ULONGLONG g_injStart = 0;
static std::wstring g_injMsg;
static bool g_injErr = false;
static std::wstring g_procSearch;
static bool g_procSearchFocus = false;
static ULONGLONG g_procSearchTick = 0;

static std::wstring ExeDir() {
    std::wstring p = CfgPath();
    return p.substr(0, p.find_last_of(L"\\/") + 1);
}
static std::wstring RdS(const wchar_t* sec, const wchar_t* k, const std::wstring& d) {
    wchar_t b[MAX_PATH * 2];
    GetPrivateProfileStringW(sec, k, d.c_str(), b, MAX_PATH * 2, CfgPath().c_str());
    return b;
}

static BOOL CALLBACK CollectTitles(HWND h, LPARAM lp) {
    if (!IsWindowVisible(h)) return TRUE;
    wchar_t t[256];
    if (GetWindowTextW(h, t, 256) <= 0) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    ((std::vector<std::pair<DWORD, std::wstring> >*)lp)->push_back(std::make_pair(pid, std::wstring(t)));
    return TRUE;
}
static bool IsMcTitle(const std::wstring& s) { return s.find(L"Minecraft") != std::wstring::npos; }

// Проверка JVM. Для чужих процессов CreateToolhelp32Snapshot может не сработать
// (права, WOW64 и т.п.), поэтому используем два способа.
static bool HasJvm(DWORD pid) {
    if (!pid) return false;

    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (h != NULL) {
        HMODULE mods[1024]; DWORD needed = 0;
        if (EnumProcessModulesEx(h, mods, sizeof(mods), &needed, LIST_MODULES_ALL)) {
            const unsigned count = std::min<unsigned>(needed / sizeof(HMODULE), 1024);
            wchar_t name[MAX_PATH];
            for (unsigned i = 0; i < count; ++i) {
                if (GetModuleBaseNameW(h, mods[i], name, MAX_PATH) &&
                    _wcsicmp(name, L"jvm.dll") == 0) {
                    CloseHandle(h);
                    return true;
                }
            }
        }
        CloseHandle(h);
    }

    HANDLE m = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (m == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me;
    me.dwSize = sizeof(me);
    bool found = false;
    if (Module32FirstW(m, &me)) {
        do {
            if (_wcsicmp(me.szModule, L"jvm.dll") == 0) { found = true; break; }
        } while (Module32NextW(m, &me));
    }
    CloseHandle(m);
    return found;
}

// Важно: имя процесса само по себе ничего не говорит о том, что это Minecraft.
// Поэтому сначала строим дерево от выбранного окна/клиента.
struct ProcessNode { DWORD pid; DWORD parentPid; std::wstring exe; };

static std::vector<ProcessNode> SnapshotProcesses() {
    std::vector<ProcessNode> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            ProcessNode n;
            n.pid = pe.th32ProcessID;
            n.parentPid = pe.th32ParentProcessID;
            n.exe = pe.szExeFile;
            out.push_back(n);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return out;
}

static std::wstring LowerW(const std::wstring& s);

static std::wstring GetProcessCommandLine(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h) h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"";

    typedef LONG NTSTATUS;
    typedef NTSTATUS (WINAPI *NtQueryInformationProcessFn)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static NtQueryInformationProcessFn fn = (NtQueryInformationProcessFn)GetProcAddress(
        GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
    if (!fn) { CloseHandle(h); return L""; }

    // ProcessCommandLineInformation = 60.
    ULONG size = 0;
    NTSTATUS st = fn(h, 60, NULL, 0, &size);
    struct LocalUnicodeString { USHORT Length; USHORT MaximumLength; PWSTR Buffer; };
    if (size < sizeof(LocalUnicodeString)) { CloseHandle(h); return L""; }
    std::vector<unsigned char> buf(size + 2 * sizeof(wchar_t), 0);
    st = fn(h, 60, buf.data(), size, &size);
    if (st < 0) { CloseHandle(h); return L""; }

    LocalUnicodeString* us = reinterpret_cast<LocalUnicodeString*>(buf.data());
    std::wstring result;
    if (us->Buffer && us->Length) {
        result.assign(us->Buffer, us->Length / sizeof(wchar_t));
    }
    CloseHandle(h);
    return result;
}

static bool LooksLikeMinecraftCommandLine(const std::wstring& cmd) {
    if (cmd.empty()) return false;
    const std::wstring s = LowerW(cmd);
    return s.find(L"net.minecraft") != std::wstring::npos ||
           s.find(L"net.fabricmc") != std::wstring::npos ||
           s.find(L"knotclient") != std::wstring::npos ||
           s.find(L"minecraft") != std::wstring::npos ||
           s.find(L"forge") != std::wstring::npos;
}

static bool IsJavaExe(const std::wstring& exe) {
    return _wcsicmp(exe.c_str(), L"java.exe") == 0 ||
           _wcsicmp(exe.c_str(), L"javaw.exe") == 0;
}

static bool IsDescendantOf(DWORD pid, DWORD rootPid, const std::vector<ProcessNode>& all) {
    if (!pid || !rootPid) return false;
    if (pid == rootPid) return true;
    DWORD cur = pid;
    for (int depth = 0; depth < 32; ++depth) {
        DWORD parent = 0;
        for (const auto& n : all) {
            if (n.pid == cur) { parent = n.parentPid; break; }
        }
        if (!parent || parent == cur) return false;
        if (parent == rootPid) return true;
        cur = parent;
    }
    return false;
}

static bool IsMinecraftWindowPid(DWORD pid, const std::vector<std::pair<DWORD, std::wstring>>& wins) {
    for (const auto& w : wins) {
        if (w.first == pid && IsMcTitle(w.second)) return true;
    }
    return false;
}

static bool IsMinecraftRelatedPid(DWORD pid, const std::vector<ProcessNode>& all,
                                  const std::vector<std::pair<DWORD, std::wstring>>& wins) {
    if (IsMinecraftWindowPid(pid, wins)) return true;
    DWORD cur = pid;
    for (int depth = 0; depth < 32; ++depth) {
        DWORD parent = 0;
        for (const auto& n : all) {
            if (n.pid == cur) { parent = n.parentPid; break; }
        }
        if (!parent || parent == cur) return false;
        if (IsMinecraftWindowPid(parent, wins)) return true;
        cur = parent;
    }
    return false;
}

// Возвращает JVM, связанную с выбранным клиентом. Приоритет:
// 1) сам выбранный процесс, если в нём JVM;
// 2) JVM в дочернем дереве клиента;
// 3) Java-процесс, находящийся в том же дереве, даже если между ними несколько уровней.
// Чужие java.exe (например IntelliJ IDEA) не подходят, потому что не являются
// потомками выбранного launcher/client.
static DWORD FindJvmInProcessTree(DWORD rootPid) {
    if (!rootPid) return 0;
    const std::vector<ProcessNode> all = SnapshotProcesses();

    // 1. Некоторые клиенты действительно держат JVM внутри launcher_base.exe.
    if (HasJvm(rootPid)) return rootPid;

    // 2. Основной безопасный путь: JVM является потомком выбранного клиента.
    DWORD best = 0;
    int bestScore = -1;
    for (const auto& n : all) {
        if (n.pid == GetCurrentProcessId() || n.pid == rootPid) continue;
        if (!IsDescendantOf(n.pid, rootPid, all) || !IsJavaExe(n.exe)) continue;
        int score = HasJvm(n.pid) ? 100 : 0;
        if (LooksLikeMinecraftCommandLine(GetProcessCommandLine(n.pid))) score += 50;
        if (score > bestScore) { bestScore = score; best = n.pid; }
    }
    if (bestScore >= 100) return best;

    // 3. Fallback для нестандартного launcher: ищем JVM по командной строке.
    // Это позволяет найти Minecraft, даже если launcher отделил Java-процесс
    // в другую ветку. IDEA и другие java.exe без Minecraft/Fabric аргументов
    // сюда не попадут.
    for (const auto& n : all) {
        if (n.pid == GetCurrentProcessId() || !IsJavaExe(n.exe)) continue;
        if (!HasJvm(n.pid)) continue;
        const std::wstring cmd = GetProcessCommandLine(n.pid);
        if (!LooksLikeMinecraftCommandLine(cmd)) continue;

        int score = 0;
        if (IsDescendantOf(n.pid, rootPid, all)) score += 1000;
        score += 100;
        if (cmd.find(L"1.21.4") != std::wstring::npos) score += 25;
        if (score > bestScore) { bestScore = score; best = n.pid; }
    }
    return best;
}

static std::wstring LowerW(const std::wstring& s) {
    std::wstring out = s;
    for (wchar_t& c : out) c = (wchar_t)towlower(c);
    return out;
}

static bool ContainsNoCase(const std::wstring& value, const std::wstring& needle) {
    if (needle.empty()) return true;
    return LowerW(value).find(LowerW(needle)) != std::wstring::npos;
}

static void RefreshProcs() {
    g_procs.clear();
    std::vector<std::pair<DWORD, std::wstring> > wins;
    EnumWindows(CollectTitles, (LPARAM)&wins);

    const std::vector<ProcessNode> allProcs = SnapshotProcesses();
    const bool customSearch = !g_procSearch.empty();

    for (const ProcessNode& node : allProcs) {
        PROCESSENTRY32W pe{};
        pe.th32ProcessID = node.pid;
        pe.th32ParentProcessID = node.parentPid;
        wcsncpy_s(pe.szExeFile, node.exe.c_str(), _TRUNCATE);
        {
            if (pe.th32ProcessID == GetCurrentProcessId()) continue;

            ProcEntry e;
            e.pid = pe.th32ProcessID;
            e.exe = pe.szExeFile;

            for (size_t i = 0; i < wins.size(); ++i) {
                if (wins[i].first == e.pid &&
                    (e.title.empty() || IsMcTitle(wins[i].second))) {
                    e.title = wins[i].second;
                }
            }

            bool javaName = _wcsicmp(pe.szExeFile, L"javaw.exe") == 0 ||
                            _wcsicmp(pe.szExeFile, L"java.exe") == 0;
            bool minecraftTitle = IsMcTitle(e.title);

            // Без поиска показываем только вероятные Minecraft/JVM процессы.
            // При ручном поиске разрешаем любое имя процесса или заголовок окна.
            bool matches = ContainsNoCase(e.exe, g_procSearch) ||
                           ContainsNoCase(e.title, g_procSearch);
            if (customSearch) {
                if (!matches) continue;
            } else {
                // Не сканируем jvm.dll у каждого процесса: это дорого и может
                // давать ложные результаты из-за прав доступа. Без поиска
                // показываем окна Minecraft и обычные Java-процессы.
                if (!javaName && !minecraftTitle) continue;
                if (javaName && !minecraftTitle &&
                    !IsMinecraftRelatedPid(e.pid, allProcs, wins)) continue;
            }

            g_procs.push_back(e);
        }
    }

    std::stable_sort(g_procs.begin(), g_procs.end(),
        [](const ProcEntry& a, const ProcEntry& b) {
            const bool am = IsMcTitle(a.title);
            const bool bm = IsMcTitle(b.title);
            if (am != bm) return am > bm;
            const bool aj = _wcsicmp(a.exe.c_str(), L"java.exe") == 0 ||
                            _wcsicmp(a.exe.c_str(), L"javaw.exe") == 0;
            const bool bj = _wcsicmp(b.exe.c_str(), L"java.exe") == 0 ||
                            _wcsicmp(b.exe.c_str(), L"javaw.exe") == 0;
            if (aj != bj) return aj > bj;
            return _wcsicmp(a.exe.c_str(), b.exe.c_str()) < 0;
        });

    g_procSearchTick = GetTickCount64();
}


struct FindPidWindowCtx { DWORD pid; HWND hwnd; };

static BOOL CALLBACK FindWindowForPidProc(HWND h, LPARAM lp) {
    FindPidWindowCtx* c = (FindPidWindowCtx*)lp;
    if (!IsWindowVisible(h) || IsIconic(h) || GetWindow(h, GW_OWNER) != NULL)
        return TRUE;
    DWORD p = 0;
    GetWindowThreadProcessId(h, &p);
    if (p != c->pid) return TRUE;
    c->hwnd = h;
    wchar_t title[256];
    GetWindowTextW(h, title, 256);
    if (title[0] != 0) return FALSE;
    return TRUE;
}

static HWND FindWindowForPid(DWORD pid) {
    if (!pid) return NULL;
    FindPidWindowCtx c = { pid, NULL };
    EnumWindows(FindWindowForPidProc, (LPARAM)&c);
    return c.hwnd;
}


// Destra Visuals: launcher_base.exe owns the actual GLFW30 Minecraft window.
// Do not confuse an unrelated java.exe (for example IntelliJ IDEA) with the game.
static DWORD FindProcessIdByName(const wchar_t* wantedName) {
    if (!wantedName || !*wantedName) return 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    DWORD result = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, wantedName) == 0) {
                result = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return result;
}

struct DestraWindowCtx { DWORD pid; HWND hwnd; };

static BOOL CALLBACK FindDestraWindowProc(HWND h, LPARAM lp) {
    DestraWindowCtx* c = reinterpret_cast<DestraWindowCtx*>(lp);
    if (!IsWindowVisible(h) || IsIconic(h) || GetWindow(h, GW_OWNER) != NULL)
        return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != c->pid) return TRUE;

    wchar_t cls[64]{};
    wchar_t title[256]{};
    GetClassNameW(h, cls, 64);
    GetWindowTextW(h, title, 256);

    if (wcscmp(cls, L"GLFW30") != 0) return TRUE;
    if (!ContainsNoCase(title, L"Minecraft")) return TRUE;

    c->hwnd = h;
    return FALSE;
}

static HWND FindDestraMinecraftWindow(DWORD* outPid = nullptr) {
    const DWORD pid = FindProcessIdByName(L"launcher_base.exe");
    if (outPid) *outPid = pid;
    if (!pid) return NULL;

    DestraWindowCtx c{ pid, NULL };
    EnumWindows(FindDestraWindowProc, (LPARAM)&c);
    return c.hwnd;
}

static std::wstring LastLogLine(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return L"";
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    std::string s(buf);
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
    size_t nl = s.find_last_of('\n');
    if (nl != std::string::npos) s = s.substr(nl + 1);
    if (s.empty()) return L"";
    wchar_t w[512];
    int m = MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, w, 512);
    return m > 0 ? std::wstring(w) : L"";
}

static void StartInject(DWORD pid) {
    g_injErr = true;
    g_selPid = pid;

    // Защита от случайного выбора java.exe от IntelliJ IDEA/другой программы.
    // Если пользователь выбрал Java напрямую, она должна находиться под окном Minecraft.
    if (IsJavaExe([&]() -> std::wstring {
        for (const auto& e : g_procs) if (e.pid == pid) return e.exe;
        return L"";
    }())) {
        std::vector<std::pair<DWORD, std::wstring>> wins;
        EnumWindows(CollectTitles, (LPARAM)&wins);
        const auto all = SnapshotProcesses();
        if (!IsMinecraftRelatedPid(pid, all, wins)) {
            g_agentPid = 0;
            g_injMsg = L"Этот java.exe не связан с окном Minecraft. Выбран процесс клиента launcher_base.exe или Minecraft.";
            return;
        }
    }

    // Если в списке уже выбран java.exe/javaw.exe, это ИМЕННО JVM-процесс.
    // Не надо проверять наличие jvm.dll через EnumProcessModulesEx:
    // Windows может не разрешить перечисление модулей, хотя Attach API
    // нормально подключается к этому PID. Связь с Minecraft уже проверена
    // выше через IsMinecraftRelatedPid(), поэтому PID можно передавать
    // рабочему Injector напрямую.
    DWORD targetPid = 0;
    std::wstring selectedExe;
    for (const auto& e : g_procs) {
        if (e.pid == pid) {
            selectedExe = e.exe;
            break;
        }
    }

    if (IsJavaExe(selectedExe)) {
        targetPid = pid;
    } else {
        // Для launcher_base.exe/других клиентов сохраняем старый рабочий
        // поиск JVM по дереву и fallback без изменения его логики.
        targetPid = FindJvmInProcessTree(pid);
    }
    if (!targetPid) {
        g_agentPid = 0;
        g_injMsg = L"JVM не найдена в процессе клиента или его дочернем дереве. "
                   L"Проверьте, что Minecraft уже запущен.";
        return;
    }
    g_agentPid = targetPid;

    {
        std::wstring logPath = ExeDir() + L"MCOverlay_selected_jvm.log";
        FILE* f = _wfopen(logPath.c_str(), L"wb");
        if (f) {
            std::wstring msg = L"client_pid=" + std::to_wstring(pid) +
                               L" selected_exe=" + selectedExe +
                               L" target_jvm_pid=" + std::to_wstring(targetPid) + L"\r\n";
            int need = WideCharToMultiByte(CP_UTF8, 0, msg.c_str(), -1, NULL, 0, NULL, NULL);
            if (need > 0) {
                std::string u8((size_t)need - 1, '\0');
                WideCharToMultiByte(CP_UTF8, 0, msg.c_str(), -1, &u8[0], need, NULL, NULL);
                fwrite(u8.data(), 1, u8.size(), f);
            }
            fclose(f);
        }
    }

    std::wstring dir = ExeDir();
    std::wstring jar = RdS(L"agent", L"jar", dir + L"mcagent.jar");
    if (GetFileAttributesW(jar.c_str()) == INVALID_FILE_ATTRIBUTES) {
        g_injMsg = L"Не найден файл агента: " + jar;
        return;
    }
    // java с модулем jdk.attach: [agent] java=..., иначе JAVA_HOME, иначе java из PATH
    std::wstring java = RdS(L"agent", L"java", L"");
    if (java.empty()) {
        wchar_t jh[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"JAVA_HOME", jh, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            std::wstring cand = std::wstring(jh) + L"\\bin\\java.exe";
            if (GetFileAttributesW(cand.c_str()) != INVALID_FILE_ATTRIBUTES) java = cand;
        }
    }
    if (java.empty()) java = L"java.exe";

    std::wstring log = dir + L"MCOverlay_inject.log";
    SECURITY_ATTRIBUTES sa;
    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE hLog = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hLog;
    si.hStdError = hLog;
    std::wstring cmd = L"\"" + java + L"\" -cp \"" + jar + L"\" com.overlay.agent.Injector " +
                       std::to_wstring((unsigned long)targetPid) + L" \"" + jar + L"\"";
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    BOOL ok = CreateProcessW(NULL, &cmd[0], NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    if (hLog != INVALID_HANDLE_VALUE) CloseHandle(hLog);
    if (!ok) {
        g_injMsg = L"Не удалось запустить java (укажите путь JDK в MCOverlay.ini: [agent] java=...)";
        return;
    }
    CloseHandle(pi.hThread);
    g_injProc = pi.hProcess;
    g_selPid = pid;
    g_injStart = GetTickCount64();
    g_injMsg.clear();
    g_injErr = false;
    g_stage = 1;
}

static void PollInject() {
    if (g_stage != 1 || !g_injProc) return;
    DWORD w = WaitForSingleObject(g_injProc, 0);
    bool timeout = (w != WAIT_OBJECT_0) && GetTickCount64() - g_injStart > 20000;
    if (w != WAIT_OBJECT_0 && !timeout) return;
    DWORD code = 1;
    if (timeout) TerminateProcess(g_injProc, 1); else GetExitCodeProcess(g_injProc, &code);
    CloseHandle(g_injProc);
    g_injProc = NULL;
    if (!timeout && code == 0) {
        g_stage = 2;
        g_injErr = false;
        g_injMsg = L"Агент внедрён";
    } else {
        g_stage = 0;
        g_injErr = true;
        std::wstring last = LastLogLine(ExeDir() + L"MCOverlay_inject.log");
        g_injMsg = (timeout ? L"Тайм-аут внедрения. " : L"Ошибка внедрения. ") + last;
    }
}

static void DrawPicker(float MW, float MH, unsigned acc) {
    Circle(42, 36, 12, Col(acc, 0.22f), true);
    Circle(42, 36, 5, Col(acc), true);
    Txt(L"Coords Overlay", fTitleL, 64, 12, 340, 40, Col(C_TEXT));
    Txt(L"Выбор процесса Minecraft", fSmall, 64, 38, 400, 58, Col(C_MUTE));
    {
        float cl = MW - 64, ct = 20, cr = MW - 28, cb = 52;
        bool hov = Hover(cl, ct, cr, cb);
        if (Clicked(g_id++, hov)) g_quit = true;
        FillRR(cl, ct, cr, cb, 10, Col(C_EDGE, hov ? 0.14f : 0.0f));
        Txt(L"×", fBig, cl, ct, cr, cb, Col(C_TEXT, 0.85f));
    }
    FillRect(24, 72, MW - 24, 73, Col(C_EDGE, 0.10f));

    float cw = std::min(MW - 80, 920.0f), cl = (MW - cw) * 0.5f, y = 100;
    if (g_stage == 1) {
        wchar_t b[128];
        Txt(L"Внедрение агента…", fHead, cl, y, cl + cw, y + 36, Col(C_TEXT));
        _snwprintf(b, 128, L"Клиент PID %lu · JVM PID %lu · %.0f с",
                   (unsigned long)g_selPid, (unsigned long)g_agentPid,
                   (double)(GetTickCount64() - g_injStart) / 1000.0);
        Txt(b, fText, cl, y + 38, cl + cw, y + 66, Col(C_DIM));
        return;
    }

    static ULONGLONG lastRef = 0;
    ULONGLONG now = GetTickCount64();
    if (lastRef == 0 || now - lastRef > 2000) { RefreshProcs(); lastRef = now; }

    Txt(L"Можно ввести любое имя процесса или часть имени. Для клиента без jvm.dll будет проверено дочернее дерево процессов.",
        fText, cl, y, cl + cw, y + 28, Col(C_DIM));
    y += 38;

    // Поле ручного поиска по имени процесса или заголовку окна.
    float sl = cl, st = y, sr = cl + cw - 150, sb = y + 42;
    bool shov = Hover(sl, st, sr, sb);
    if (Clicked(g_id++, shov)) {
        g_procSearchFocus = true;
        SetFocus(g_hwnd);
    }
    FillRR(sl, st, sr, sb, 10, Col(C_EDGE, g_procSearchFocus ? 0.14f : 0.06f));
    StrokeRR(sl + 0.5f, st + 0.5f, sr - 0.5f, sb - 0.5f, 10,
             Col(g_procSearchFocus ? acc : C_EDGE, g_procSearchFocus ? 0.75f : 0.25f), 1.0f);
    std::wstring searchText = g_procSearch.empty() && !g_procSearchFocus
        ? L"Например: LunarClient.exe, javaw.exe, Minecraft.exe"
        : g_procSearch;
    Txt(searchText.c_str(), fText, sl + 14, st, sr - 12, sb, 
        g_procSearch.empty() ? Col(C_MUTE) : Col(C_TEXT));

    if (ButtonC(sr + 12, st, cl + cw, sb, L"Найти", acc, true)) {
        RefreshProcs();
        g_procSearchFocus = false;
    }
    y += 54;

    if (g_procs.empty())
        Txt(g_procSearch.empty()
            ? L"Подходящие процессы не найдены. Запустите Minecraft — список обновляется сам."
            : L"По этому запросу процессов не найдено. Введите другое имя или его часть.",
            fText, cl, y, cl + cw, y + 28, Col(C_MUTE));

    for (size_t i = 0; i < g_procs.size() && y + 56 < MH - 150; ++i) {
        ProcEntry e = g_procs[i];
        bool mc = IsMcTitle(e.title);
        FillRR(cl, y, cl + cw, y + 56, 12, Col(mc ? acc : C_EDGE, mc ? 0.10f : 0.05f));
        if (mc) StrokeRR(cl + 0.5f, y + 0.5f, cl + cw - 0.5f, y + 55.5f, 12, Col(acc, 0.5f), 1.0f);
        Txt(e.title.empty() ? L"(без окна)" : e.title.c_str(), fText, cl + 16, y + 4, cl + cw - 190, y + 30, Col(C_TEXT));
        wchar_t sub[128];
        _snwprintf(sub, 128, L"%ls · PID %lu", e.exe.c_str(), (unsigned long)e.pid);
        Txt(sub, fSmall, cl + 16, y + 28, cl + cw - 190, y + 52, Col(C_MUTE));
        if (ButtonC(cl + cw - 170, y + 8, cl + cw - 12, y + 48, L"Внедрить", acc, true)) StartInject(e.pid);
        y += 64;
    }

    float by = MH - 84;
    if (!g_injMsg.empty())
        Txt(g_injMsg.c_str(), fText, cl, by - 40, cl + cw, by - 8, g_injErr ? Col(C_DANGER) : Col(0x34D399));
    if (ButtonC(cl, by, cl + 160, by + 42, L"Обновить", C_EDGE, false)) RefreshProcs();
    if (ButtonC(cl + 176, by, cl + 316, by + 42, L"Очистить поиск", C_EDGE, false)) {
        g_procSearch.clear();
        g_procSearchFocus = false;
        RefreshProcs();
    }
    if (ButtonC(cl + 332, by, cl + 612, by + 42, L"Продолжить без внедрения", C_EDGE, false)) {
        g_stage = 2; g_injMsg.clear();
    }
    if (ButtonC(cl + cw - 160, by, cl + cw, by + 42, L"Выход", C_DANGER, false)) g_quit = true;
}

static const wchar_t* kTabs[4] = { L"Combat", L"Visuals", L"Player", L"Panic" };

static void DrawMenu(float W, float H) {
    float ox, oy, ms;
    MenuPlacement(W, H, ox, oy, ms);
    const float MW = g_MW, MH = g_MH;
    g_lmx = ((float)g_mx - ox) / ms;
    g_lmy = ((float)g_my - oy) / ms;
    SetXform(ms, ox, oy);
    g_id = 0;
    g_clipOn = false;
    unsigned acc = Accent();

    // Окно
    for (int i = 6; i >= 1; --i)
        FillRR(-i * 3.0f, -i * 3.0f + 8, MW + i * 3.0f, MH + i * 3.0f + 8, 22.0f + i * 3.0f, Col(0x000814, 0.07f));
    FillRR(0, 0, MW, MH, 22, Col(C_BG, 0.95f));
    StrokeRR(0.5f, 0.5f, MW - 0.5f, MH - 0.5f, 22, Col(acc, 0.28f), 1.0f);

    if (g_stage != 2) { DrawPicker(MW, MH, acc); SetXform(1.0f, 0, 0); return; }

    // Верхняя панель
    const float topH = 72.0f;
    Circle(42, 36, 12, Col(acc, 0.22f), true);
    Circle(42, 36, 5, Col(acc), true);
    Txt(L"Coords Overlay", fTitleL, 64, 12, 340, 40, Col(C_TEXT));
    Txt(L"Меню · v3", fSmall, 64, 38, 340, 58, Col(C_MUTE));
    {
        const float tw = 128, th = 40, ty = 16;
        float tx = (MW - tw * 4 - 8 * 3) * 0.5f;
        for (int i = 0; i < 4; ++i) {
            float l = tx + i * (tw + 8);
            bool hov = Hover(l, ty, l + tw, ty + th);
            int tid = g_id++;
            float ha = HoverAnim(tid, hov);
            if (Clicked(tid, hov) && g_tab != i) { g_tab = i; g_scroll = 0; g_binding = -1; }
            bool sel = (g_tab == i);
            float he = ha * 2.0f;
            FillRR(l - he, ty - he, l + tw + he, ty + th + he, 12,
                   sel ? Lighten(Col(acc, 0.20f + 0.10f * ha), ha * 0.30f) : Lighten(Col(C_EDGE, 0.13f * ha), ha * 0.5f));
            if (sel) StrokeRR(l + 0.5f, ty + 0.5f, l + tw - 0.5f, ty + th - 0.5f, 12, Col(acc, 0.8f), 1.2f);
            Txt(kTabs[i], fTextC, l, ty, l + tw, ty + th, sel ? Col(acc) : Col(hov ? C_TEXT : C_DIM));
        }
        float cl = MW - 64, ct = 20, cr = MW - 28, cb = 52;
        bool hov = Hover(cl, ct, cr, cb);
        int xid = g_id++;
        float xa = HoverAnim(xid, hov);
        if (Clicked(xid, hov)) SetMenuOpen(false, true);
        FillRR(cl - xa * 2, ct - xa * 2, cr + xa * 2, cb + xa * 2, 10, Lighten(Col(C_EDGE, 0.16f * xa), xa * 0.5f));
        Txt(L"×", fBig, cl, ct, cr, cb, Col(C_TEXT, 0.85f));
    }
    FillRect(24, topH, MW - 24, topH + 1, Col(C_EDGE, 0.10f));

    // Область контента
    const float vl = 24, vr = MW - 30, vt = topH + 16, vb = MH - 70;
    int ncol = MW >= 1100 ? 3 : (MW >= 700 ? 2 : 1);
    BeginLayout(vl, vt, vr - vl, ncol);
    g_clipOn = true; g_clipT = vt; g_clipB = vb;
    g_rt->PushAxisAlignedClip(D2D1::RectF(vl - 4, vt, vr + 4, vb), D2D1_ANTIALIAS_MODE_ALIASED);
    switch (g_tab) {
    case 0: TabCombat(); break;
    case 1: TabVisuals(); break;
    case 2: TabPlayer(); break;
    default: TabPanic(); break;
    }
    g_rt->PopAxisAlignedClip();
    g_clipOn = false;

    float bottom = 0;
    for (int i = 0; i < ncol; ++i) bottom = std::max(bottom, g_colY[i]);
    float contentH = (bottom + g_scroll) - vt - g_gap;
    g_scrollMax = std::max(0.0f, contentH - (vb - vt));
    if (g_scroll > g_scrollMax) g_scroll = g_scrollMax;
    if (g_scrollMax > 0.5f) {
        float th = vb - vt, kh = std::max(40.0f, th * th / (th + g_scrollMax));
        float ky = vt + (th - kh) * (g_scroll / g_scrollMax);
        FillRR(MW - 18, vt, MW - 14, vb, 2, Col(C_EDGE, 0.08f));
        FillRR(MW - 18, ky, MW - 14, ky + kh, 2, Col(acc, 0.65f));
    }

    // Нижняя панель
    float by = MH - 56;
    FillRect(24, by - 8, MW - 24, by - 7, Col(C_EDGE, 0.10f));
    std::wstring hint = L"Клик по клавише — назначить · Esc — отмена · Backspace — очистить · Меню: " + KeyName(g_cfg.keyMenu);
    Txt(hint.c_str(), fSmall, 28, by, MW - 200, by + 44, Col(C_MUTE));
    if (Button(MW - 164, by + 2, MW - 28, by + 42, L"Готово", true)) SetMenuOpen(false, true);

    SetXform(1.0f, 0, 0);
}

// ======================= Кадр =======================
static void RenderFrame() {
    int W = g_w, H = g_h;
    if (!EnsureTarget(W, H)) return;

    RECT rc = { 0, 0, W, H };
    g_rt->BindDC(g_memDC, &rc);
    g_rt->BeginDraw();
    g_rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    g_hoverClickable = false;


    float fw = (float)W, fh = (float)H;
    PlayerCoords markerCoords;
    EnterCriticalSection(&g_cs);
    markerCoords = g_coords;
    LeaveCriticalSection(&g_cs);
    if (g_menuOpen) {
        FillRect(0, 0, fw, fh, Col(0x020C18, g_hudEdit ? 0.35f : 0.58f));
        DrawTargetMarker(fw, fh, markerCoords);
        if (g_hudEdit) {
            // Режим перемещения HUD: меню скрыто, панель можно таскать мышью
            g_id = 0; g_clipOn = false;
            g_lmx = (float)g_mx; g_lmy = (float)g_my;
            if (g_pressed && g_mx >= g_cfg.hudX && g_mx < g_cfg.hudX + g_hudW &&
                g_my >= g_cfg.hudY && g_my < g_cfg.hudY + g_hudH) {
                g_dragging = true;
                g_dragOffX = (float)(g_mx - g_cfg.hudX);
                g_dragOffY = (float)(g_my - g_cfg.hudY);
            }
            if (g_dragging) {
                if (g_down) {
                    g_cfg.hudX = (int)(g_mx - g_dragOffX);
                    g_cfg.hudY = (int)(g_my - g_dragOffY);
                    g_hoverClickable = true;
                } else g_dragging = false;
            }
            DrawHud(fw, fh);
            SetXform(1.0f, 0, 0);
            Txt(L"Перетащите панель мышью · Esc — вернуться в меню", fTextC,
                fw * 0.5f - 320, fh - 132, fw * 0.5f + 320, fh - 98, Col(C_TEXT));
            if (Button(fw * 0.5f - 90, fh - 88, fw * 0.5f + 90, fh - 44, L"Готово", true)) g_hudEdit = false;
        } else {
            DrawMenu(fw, fh);
        }
    } else {
        DrawEsp(fw, fh);
        if (g_cfg.hudOn) {
            DrawTargetMarker(fw, fh, markerCoords);
            DrawHud(fw, fh);
        }
    }

    HRESULT hr = g_rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) { DestroyTarget(); return; }

    HDC screen = GetDC(NULL);
    POINT dst = { g_x, g_y }, src = { 0, 0 };
    SIZE sz = { W, H };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_hwnd, screen, &dst, &sz, g_memDC, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(NULL, screen);

    g_pressed = g_released = false;
    if (!g_down) g_active = -1;
}

// ======================= Окно игры =======================
static BOOL CALLBACK FindMc(HWND h, LPARAM lp) {
    if (!IsWindowVisible(h) || IsIconic(h) || GetWindow(h, GW_OWNER) != NULL) return TRUE;
    wchar_t cls[64];
    GetClassNameW(h, cls, 64);
    if (wcscmp(cls, L"GLFW30") != 0) return TRUE;
    *(HWND*)lp = h;
    return FALSE;
}

static void UpdateGameWindow() {
    static ULONGLONG lastScan = 0;
    if (g_game && (!IsWindow(g_game) || !IsWindowVisible(g_game))) g_game = NULL;

    ULONGLONG now = GetTickCount64();
    if (now - lastScan <= 250) return;
    lastScan = now;

    // Destra Visuals: сначала привязываемся к launcher_base.exe и его
    // реальному окну Minecraft (GLFW30). JVM для этого не требуется.
    DWORD destraPid = 0;
    HWND destra = FindDestraMinecraftWindow(&destraPid);
    if (destra) {
        g_game = destra;
        g_selPid = destraPid;
        return;
    }

    // После ручного выбора процесса ищем окно именно этого PID.
    if (g_selPid) {
        HWND selected = FindWindowForPid(g_selPid);
        if (selected) {
            g_game = selected;
            return;
        }
    }

    // Fallback для обычного Minecraft.
    HWND found = NULL;
    EnumWindows(FindMc, (LPARAM)&found);
    if (found) g_game = found;
}

static void UpdateTargetRect() {
    int x = 0, y = 0, w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
    if (g_game && !IsIconic(g_game)) {
        RECT r;
        POINT p = { 0, 0 };
        if (GetClientRect(g_game, &r) && ClientToScreen(g_game, &p) &&
            r.right >= 64 && r.bottom >= 64) {
            x = p.x; y = p.y; w = r.right; h = r.bottom;
        } else {
            // Fallback for GLFW windowed configurations where the client rect
            // is not available for one frame during resize/fullscreen switch.
            RECT wr;
            if (GetWindowRect(g_game, &wr) && wr.right - wr.left >= 64 && wr.bottom - wr.top >= 64) {
                x = wr.left; y = wr.top; w = wr.right - wr.left; h = wr.bottom - wr.top;
            }
        }
    }
    if (x != g_x || y != g_y || w != g_w || h != g_h) {
        g_x = x; g_y = y; g_w = w; g_h = h;
        SetWindowPos(g_hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
    }
}

// ======================= Основной тик =======================
static void Tick() {
    UpdateGameWindow();
    PollInject();

    static double lastPoll = 0;
    double nowMs = NowMs();
    bool poll = (nowMs - lastPoll) >= 6.0;
    if (poll) lastPoll = nowMs;
    if (poll) {
    for (int vk = 1; vk < 256; ++vk) g_cur[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;

    if (g_binding >= 0) {
        if (GetTickCount64() - g_bindTick > 150) {
            for (int vk = 3; vk < 256; ++vk) {
                if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) continue;
                if (!Pressed(vk)) continue;
                int* keys[] = { &g_cfg.keyMenu, &g_cfg.keyHud, &g_cfg.keyTarget, &g_cfg.keyAuto, &g_cfg.keyAim, &g_cfg.keyPanic };
                int* target = keys[g_binding];
                if (vk == VK_ESCAPE) { g_binding = -1; break; }
                int newKey = (vk == VK_BACK) ? 0 : vk;
                *target = newKey;
                if (newKey != 0) {
                    for (int i = 0; i < 6; ++i) {
                        if (i != g_binding && *keys[i] == newKey) *keys[i] = 0;
                    }
                }
                g_binding = -1;
                SaveConfig();
                break;
            }
        }
    } else {
        if (Pressed(g_cfg.keyPanic)) DoPanic();
        else if (Pressed(g_cfg.keyMenu)) SetMenuOpen(!g_menuOpen, true);
        else if (Pressed(g_cfg.keyHud)) { g_cfg.hudOn = !g_cfg.hudOn; PlayToggleSound(g_cfg.hudOn); if (!g_menuOpen) SaveConfig(); }
        else if (Pressed(g_cfg.keyTarget)) { g_cfg.showPlayerNew = !g_cfg.showPlayerNew; PlayToggleSound(g_cfg.showPlayerNew); if (!g_menuOpen) SaveConfig(); }
        else if (Pressed(g_cfg.keyAuto)) { g_cfg.autoOn = !g_cfg.autoOn; PlayToggleSound(g_cfg.autoOn); if (!g_menuOpen) SaveConfig(); }
        else if (Pressed(g_cfg.keyAim)) { g_cfg.aimOn = !g_cfg.aimOn; PlayToggleSound(g_cfg.aimOn); if (!g_menuOpen) SaveConfig(); }
        else if (g_stage == 2 && g_menuOpen && Pressed(VK_ESCAPE)) { if (g_hudEdit) g_hudEdit = false; else SetMenuOpen(false, true); }
    }
    }

    HWND fg = GetForegroundWindow();
    // Меню закрываем, если пользователь ушёл в другое приложение
    if (g_stage == 2 && g_menuOpen && fg != g_hwnd && GetTickCount64() - g_menuTick > 700) SetMenuOpen(false, false);

    bool inGame = !g_game || fg == g_game || fg == g_hwnd || !g_cfg.onlyInGame;
    bool gameMin = g_game && IsIconic(g_game);
    bool want = g_menuOpen || ((g_cfg.hudOn || g_cfg.espOn || g_cfg.nametagOn) && inGame && !gameMin);

    static ULONGLONG lastTop = 0;
    if (want) {
        UpdateTargetRect();
        if (!g_visible) {
            ShowWindow(g_hwnd, g_menuOpen ? SW_SHOW : SW_SHOWNOACTIVATE);
            g_visible = true;
            SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        } else if (GetTickCount64() - lastTop > 250) {
            lastTop = GetTickCount64();
            SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        bool espLive = (g_cfg.espOn || g_cfg.nametagOn) && (nowMs - g_espLastMs < 1500.0);
        static double lastRender = 0;
        double interval = (g_menuOpen || espLive) ? g_frameMs : 33.0;     // без ESP достаточно ~30 кадров/с
        if (nowMs - lastRender >= interval) {
            g_dtScale = (float)std::min(3.0, std::max(0.2, (nowMs - lastRender) / 16.667));
            lastRender = nowMs;
            RenderFrame();
        }
    } else if (g_visible) {
        ShowWindow(g_hwnd, SW_HIDE);
        g_visible = false;
    }

    if (poll) memcpy(g_prev, g_cur, sizeof(g_prev));
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEMOVE:
        g_mx = GET_X_LPARAM(lp); g_my = GET_Y_LPARAM(lp);
        return 0;
    case WM_LBUTTONDOWN:
        g_mx = GET_X_LPARAM(lp); g_my = GET_Y_LPARAM(lp);
        g_down = true; g_pressed = true;
        SetCapture(hwnd);
        return 0;
    case WM_LBUTTONUP:
        g_mx = GET_X_LPARAM(lp); g_my = GET_Y_LPARAM(lp);
        g_down = false; g_released = true;
        ReleaseCapture();
        return 0;
    case WM_CHAR:
        if (g_stage == 0 && g_menuOpen && g_procSearchFocus) {
            wchar_t ch = (wchar_t)wp;
            if (ch >= 32 && ch != 127 && g_procSearch.size() < 96) {
                g_procSearch.push_back(ch);
                RefreshProcs();
            }
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (g_stage == 0 && g_menuOpen && g_procSearchFocus) {
            if (wp == VK_BACK) {
                if (!g_procSearch.empty()) {
                    g_procSearch.pop_back();
                    RefreshProcs();
                }
                return 0;
            }
            if (wp == VK_ESCAPE) {
                g_procSearchFocus = false;
                return 0;
            }
        }
        break;
    case WM_MOUSEWHEEL:
        if (g_menuOpen && !g_hudEdit) {
            g_scroll -= (float)GET_WHEEL_DELTA_WPARAM(wp) / 120.0f * 56.0f;
            g_scroll = std::max(0.0f, std::min(g_scroll, g_scrollMax));
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursor(NULL, g_hoverClickable ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    SetProcessDPIAware();
    // Повышенный приоритет CPU для плавности оверлея. REALTIME намеренно не используется.
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    InitializeCriticalSection(&g_cs);
    InitializeCriticalSection(&g_espCs);
    timeBeginPeriod(1);
    g_espEvt = CreateEventW(NULL, FALSE, FALSE, NULL);
    {
        DEVMODEW dm; ZeroMemory(&dm, sizeof(dm)); dm.dmSize = sizeof(dm);
        int hz = 60;
        if (EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 30) hz = (int)dm.dmDisplayFrequency;
        hz = std::min(hz, 165);
        g_frameMs = 1000.0 / hz - 0.6;
    }
    g_startTick = GetTickCount64();
    LoadConfig();
    WriteAutoHitBridge();
    WriteVisualsBridge();

    // Один экземпляр
    HANDLE mutex = CreateMutexW(NULL, TRUE, L"MCOverlay_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"Оверлей уже запущен.", L"Coords Overlay", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    if (!InitGraphics()) {
        MessageBoxW(NULL, L"Не удалось инициализировать Direct2D / DirectWrite.", L"Coords Overlay", MB_OK | MB_ICONERROR);
        return 1;
    }

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"MCOverlayWindowClass";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    if (!RegisterClassExW(&wc)) return 1;

    g_w = GetSystemMetrics(SM_CXSCREEN);
    g_h = GetSystemMetrics(SM_CYSCREEN);
    DWORD ex = WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    g_hwnd = CreateWindowExW(ex, wc.lpszClassName, L"Coords Overlay", WS_POPUP, 0, 0, 100, 100, NULL, NULL, hInst, NULL);
    if (!g_hwnd) return 1;

    HANDLE hThread = CreateThread(NULL, 0, PipeServerThread, NULL, 0, NULL);
    HANDLE hEspThread = CreateThread(NULL, 0, EspPipeThread, NULL, 0, NULL);
    SetMenuOpen(true, false);        // сначала экран выбора процесса

    MSG msg;
    while (!g_quit) {
        MsgWaitForMultipleObjects(1, &g_espEvt, FALSE, 2, QS_ALLINPUT);
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) g_quit = true;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Tick();
    }

    SaveConfig();
    g_running = FALSE;
    HANDLE hDummy = CreateFileW(L"\\\\.\\pipe\\mc_coords", GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (hDummy != INVALID_HANDLE_VALUE) CloseHandle(hDummy);
    if (hThread) { WaitForSingleObject(hThread, 1000); CloseHandle(hThread); }
    HANDLE hDummy2 = CreateFileW(L"\\\\.\\pipe\\mc_esp", GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (hDummy2 != INVALID_HANDLE_VALUE) CloseHandle(hDummy2);
    if (hEspThread) { WaitForSingleObject(hEspThread, 1000); CloseHandle(hEspThread); }

    DestroyTarget();
    DestroyWindow(g_hwnd);
    DeleteCriticalSection(&g_cs);
    DeleteCriticalSection(&g_espCs);
    if (g_espEvt) CloseHandle(g_espEvt);
    timeEndPeriod(1);
    if (mutex) CloseHandle(mutex);
    return 0;
}