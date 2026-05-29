// HelixSeedInstaller.cpp
//
// Win32 GUI installer for HelixSeed.
//
// Visual goal: match the HelixSeed app's dark, near-black aesthetic:
//   - Background #0a0a0a, panels #111, thin #222 borders, 5-8px radius.
//   - Inter / Segoe UI body, JetBrains Mono / Consolas log.
//   - Owner-drawn rounded buttons (normal + primary), hover + pressed states.
//   - Dark title bar via DwmSetWindowAttribute.
//   - Painted brand mark, painted status pills, painted panels.
//
// Behaviour:
//   - Tool detection uses SearchPathW (instant, no console flashes).
//   - Visual Studio is discovered via vswhere; cl works without a Developer
//     Command Prompt by routing the C++ build through vcvars64.bat.
//   - Per-tool Install buttons use winget for the required build tools.
//   - Maven can be bundled beside the installer as tools\apache-maven-*-bin.zip.
//   - The build runs in a worker thread; output is piped into the log via
//     chunked WM_APP messages so the UI stays responsive.
//
// Build:  installer\build.bat  (needs MSVC, Developer Command Prompt for VS)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#define IDI_APP_ICON 101

namespace fs = std::filesystem;

// =========================================================================
// Constants
// =========================================================================
namespace {

constexpr const char*    kRepoUrl   = "https://github.com/HelixSeed-MC/HelixSeed.git";
constexpr const wchar_t* kAppTitle  = L"HelixSeed Installer";
constexpr const wchar_t* kClassName = L"HelixSeedInstallerWnd";

// HelixSeed palette
constexpr COLORREF C_BG         = RGB(10, 10, 10);
constexpr COLORREF C_PANEL      = RGB(17, 17, 17);
constexpr COLORREF C_PANEL_HIGH = RGB(22, 22, 22);
constexpr COLORREF C_LINE       = RGB(34, 34, 34);
constexpr COLORREF C_LINE_SOFT  = RGB(26, 26, 26);
constexpr COLORREF C_INPUT_BG   = RGB(13, 13, 13);
constexpr COLORREF C_TEXT       = RGB(230, 230, 230);
constexpr COLORREF C_MUTED      = RGB(138, 138, 138);
constexpr COLORREF C_SUBTLE     = RGB(106, 106, 106);
constexpr COLORREF C_ACCENT     = RGB(255, 255, 255);
constexpr COLORREF C_GREEN      = RGB(111, 191, 138);
constexpr COLORREF C_RED        = RGB(201, 112, 112);
constexpr COLORREF C_DIVIDER    = RGB(48, 48, 52);
constexpr COLORREF C_APP_GREEN  = RGB(95, 150, 64);
constexpr COLORREF C_APP_GREEN_DARK = RGB(38, 62, 31);

// Layout
constexpr int W_WINDOW = 800;
constexpr int H_WINDOW = 760;
constexpr int X_PAD = 20;

constexpr int Y_TOPBAR_H = 72;

// Path panel
constexpr int X_PANEL = X_PAD;
constexpr int W_PANEL = W_WINDOW - 2 * X_PAD - 16;  // window has caption / scroll, leave breath
constexpr int Y_PATH_PANEL  = Y_TOPBAR_H + 16;
constexpr int H_PATH_PANEL  = 110;

constexpr int Y_TOOLS_PANEL = Y_PATH_PANEL + H_PATH_PANEL + 12;
// Sized so all 6 tool rows fit: 56 header strip + 6 * 30 row height + 0 padding.
constexpr int H_TOOLS_PANEL = 236;
constexpr int H_TOOL_ROW    = 30;

constexpr int Y_LOG_PANEL   = Y_TOOLS_PANEL + H_TOOLS_PANEL + 12;
// Reduced to compensate for the taller toolchain panel so the action bar
// stays at the same y; preserves the existing fixed window size.
constexpr int H_LOG_PANEL   = 204;

constexpr int Y_ACTION_BAR  = Y_LOG_PANEL + H_LOG_PANEL + 16;
constexpr int H_ACTION      = 38;

// Control IDs
enum : int {
    ID_PATH_EDIT     = 101,
    ID_BROWSE_BTN    = 102,
    ID_DEFAULT_BTN   = 103,
    ID_LOG_EDIT      = 110,
    ID_INSTALL_BTN   = 120,
    ID_LAUNCH_BTN    = 121,
    ID_OPEN_BTN      = 122,
    ID_CLOSE_BTN     = 123,
    ID_BACKEND_COMBO = 124,
    ID_TOOL_BASE     = 200,  // ID_TOOL_BASE + i*10 + 1 = install button for tool i
};

enum BackendChoice : int {
    BACKEND_BOTH    = 0,
    BACKEND_CUDA    = 1,
    BACKEND_OPENCL  = 2,
};

constexpr UINT WM_APP_LOG_CHUNK = WM_APP + 1;
constexpr UINT WM_APP_DONE      = WM_APP + 2;
constexpr UINT WM_APP_RESCAN    = WM_APP + 3;

// =========================================================================
// Tool catalogue
// =========================================================================
struct ToolDef {
    const wchar_t* exeName;
    const wchar_t* displayName;
    const wchar_t* description;
    const wchar_t* wingetId;
    const wchar_t* downloadUrl;
    bool           required;
};

const ToolDef kTools[] = {
    { L"git.exe",  L"git",  L"Source clone",
      L"Git.Git",
      L"https://git-scm.com/download/win", true },
    { L"npm.cmd",  L"npm",  L"Electron / Vite UI build",
      L"OpenJS.NodeJS.LTS",
      L"https://nodejs.org/", true },
    { L"cl.exe",   L"cl",   L"MSVC C++ compiler",
      L"Microsoft.VisualStudio.2022.BuildTools",
      L"https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022",
      true },
    { L"nvcc.exe", L"nvcc", L"CUDA prefilter compiler",
      L"Nvidia.CUDA",
      L"https://developer.nvidia.com/cuda-toolkit", true },
    { L"javac.exe", L"JDK", L"Loot / Java validation compiler",
      L"Microsoft.OpenJDK.21",
      L"https://learn.microsoft.com/java/openjdk/download", true },
    { L"mvn.cmd",  L"mvn",  L"Bundled loot build tool",
      nullptr,
      L"https://maven.apache.org/download.cgi", false },
};
constexpr int kToolCount = static_cast<int>(sizeof(kTools) / sizeof(kTools[0]));

// =========================================================================
// UI globals
// =========================================================================
HFONT g_fontBrand = nullptr;   // Segoe UI 17 semibold (title)
HFONT g_fontBody  = nullptr;   // Segoe UI 12
HFONT g_fontSmall = nullptr;   // Segoe UI 11
HFONT g_fontStatus = nullptr;  // Segoe UI 12 semibold (status pill)
HFONT g_fontMono  = nullptr;   // Consolas 11 (log)
HICON g_appIcon   = nullptr;

HBRUSH g_brushBg        = nullptr;
HBRUSH g_brushPanel     = nullptr;
HBRUSH g_brushPanelHigh = nullptr;
HBRUSH g_brushInput     = nullptr;

HWND g_hwnd       = nullptr;
HWND g_pathEdit   = nullptr;
HWND g_browseBtn  = nullptr;
HWND g_defaultBtn = nullptr;
HWND g_logEdit    = nullptr;
HWND g_installBtn = nullptr;
HWND g_backendLabel = nullptr;
HWND g_backendCombo = nullptr;
HWND g_launchBtn  = nullptr;
HWND g_openBtn    = nullptr;
HWND g_closeBtn   = nullptr;

struct ToolUi {
    HWND nameLabel   = nullptr;  // "git" (white)
    HWND descLabel   = nullptr;  // " Source clone" (muted)
    HWND statusLabel = nullptr;  // colored "found" / "missing"
    HWND actionBtn   = nullptr;  // "Install"
    bool found       = false;
};
ToolUi g_toolUi[kToolCount];

bool g_hasWinget = false;
bool g_hasSystemMaven = false;
bool g_hasBundledMaven = false;
std::wstring g_vcvarsPath;
std::wstring g_jdkHome;
std::wstring g_installedExePath;
std::atomic<bool> g_buildRunning{false};

// =========================================================================
// Hover tracking for owner-drawn buttons
// =========================================================================
struct ButtonState {
    HWND hwnd = nullptr;
    bool hover = false;
    bool primary = false;  // Install HelixSeed = primary white button
    // Color to fill the button's bounding rect *outside* the rounded shape.
    // The BUTTON window class erases its background to COLOR_BTNFACE before
    // sending WM_DRAWITEM; without painting over those corners with the
    // parent's color, light-gray pixels bleed through at every button corner.
    COLORREF bgColor = C_BG;
};
std::vector<ButtonState> g_buttons;

ButtonState* findButtonState(HWND h) {
    for (auto& b : g_buttons) if (b.hwnd == h) return &b;
    return nullptr;
}

// =========================================================================
// String helpers
// =========================================================================
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::wstring defaultInstallPath() {
    if (const wchar_t* env = _wgetenv(L"LOCALAPPDATA")) {
        return std::wstring(env) + L"\\HelixSeed";
    }
    return L"C:\\HelixSeed";
}

// =========================================================================
// Drawing helpers
// =========================================================================
void fillSolid(HDC dc, RECT r, COLORREF color) {
    HBRUSH br = CreateSolidBrush(color);
    FillRect(dc, &r, br);
    DeleteObject(br);
}

void fillRoundRect(HDC dc, RECT r, int radius, COLORREF fill, COLORREF border) {
    HBRUSH br = CreateSolidBrush(fill);
    HPEN   pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, br);
    HGDIOBJ op = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);
}

void drawCenteredText(HDC dc, RECT r, const wchar_t* text, COLORREF color, HFONT font) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    HGDIOBJ of = SelectObject(dc, font);
    DrawTextW(dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, of);
}

void drawLeftText(HDC dc, RECT r, const wchar_t* text, COLORREF color, HFONT font) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    HGDIOBJ of = SelectObject(dc, font);
    DrawTextW(dc, text, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, of);
}

// =========================================================================
// Owner-drawn buttons
// =========================================================================
void drawCustomButton(LPDRAWITEMSTRUCT dis) {
    const ButtonState* st = findButtonState(dis->hwndItem);
    bool primary  = st && st->primary;
    bool hover    = st && st->hover;
    bool pressed  = (dis->itemState & ODS_SELECTED) != 0;
    bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    bool focused  = (dis->itemState & ODS_FOCUS) != 0;
    COLORREF parentBg = st ? st->bgColor : C_BG;

    // Repaint the full bounding rect with the parent's background color
    // first. The rounded fill below leaves four corner triangles uncovered;
    // without this step the BUTTON class's default COLOR_BTNFACE erase
    // shows through as bright pixels at every corner.
    fillSolid(dis->hDC, dis->rcItem, parentBg);

    COLORREF fill, border, text;
    if (primary) {
        if (disabled) {
            fill = RGB(60, 60, 60); border = fill; text = C_MUTED;
        } else if (pressed) {
            fill = RGB(210, 210, 210); border = fill; text = RGB(10, 10, 10);
        } else if (hover) {
            fill = RGB(245, 245, 245); border = fill; text = RGB(10, 10, 10);
        } else {
            fill = C_ACCENT; border = C_ACCENT; text = RGB(10, 10, 10);
        }
    } else {
        if (disabled) {
            fill = parentBg; border = C_LINE_SOFT; text = C_SUBTLE;
        } else if (pressed) {
            fill = RGB(8, 8, 8); border = RGB(58, 58, 58); text = C_TEXT;
        } else if (hover) {
            fill = C_PANEL_HIGH; border = RGB(58, 58, 58); text = C_TEXT;
        } else {
            fill = C_PANEL; border = C_LINE; text = C_TEXT;
        }
    }

    // Focus is shown by brightening the border only; no second inset
    // round-rect. Two concentric rounded rects with different radii leave
    // sub-pixel mismatches at the corners that look like tiny line/dot
    // artifacts, especially when focus jumps from one button to another.
    if (focused && !disabled && !pressed) {
        border = primary ? RGB(255, 255, 255) : RGB(96, 96, 102);
    }

    fillRoundRect(dis->hDC, dis->rcItem, 6, fill, border);

    wchar_t buf[256];
    GetWindowTextW(dis->hwndItem, buf, 256);
    drawCenteredText(dis->hDC, dis->rcItem, buf, text, g_fontBody);
}

LRESULT CALLBACK ButtonSubclassProc(HWND h, UINT msg, WPARAM wp, LPARAM lp,
                                    UINT_PTR /*id*/, DWORD_PTR /*ref*/) {
    ButtonState* st = findButtonState(h);
    switch (msg) {
        case WM_MOUSEMOVE:
            if (st && !st->hover) {
                st->hover = true;
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 };
                TrackMouseEvent(&tme);
                InvalidateRect(h, nullptr, FALSE);
            }
            break;
        case WM_MOUSELEAVE:
            if (st) { st->hover = false; InvalidateRect(h, nullptr, FALSE); }
            break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

void registerOwnerButton(HWND h, bool primary, COLORREF bgColor) {
    g_buttons.push_back(ButtonState{ h, false, primary, bgColor });
    SetWindowSubclass(h, ButtonSubclassProc, 1, 0);
}

HWND createButton(HWND parent, const wchar_t* text, int id, int x, int y, int w, int h,
                  bool primary = false, bool defaultBtn = false, COLORREF bgColor = C_PANEL) {
    (void)defaultBtn;
    DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW
                  | BS_PUSHBUTTON;
    HWND b = CreateWindowW(L"BUTTON", text, style, x, y, w, h, parent,
                           reinterpret_cast<HMENU>((INT_PTR)id), nullptr, nullptr);
    SendMessageW(b, WM_SETFONT, reinterpret_cast<WPARAM>(g_fontBody), TRUE);
    registerOwnerButton(b, primary, bgColor);
    return b;
}

// =========================================================================
// Static labels
// =========================================================================
HWND createLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h,
                 HFONT font, int id = -1, DWORD extraStyle = SS_LEFT) {
    HWND lbl = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE | extraStyle,
                             x, y, w, h, parent,
                             id >= 0 ? reinterpret_cast<HMENU>((INT_PTR)id) : nullptr,
                             nullptr, nullptr);
    SendMessageW(lbl, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return lbl;
}

// =========================================================================
// Tool detection
// =========================================================================
bool toolFound(const wchar_t* exeName) {
    wchar_t buf[MAX_PATH];
    return SearchPathW(nullptr, exeName, nullptr, MAX_PATH, buf, nullptr) > 0;
}

bool canInstallTool(const ToolDef& tool) {
    return g_hasWinget && tool.wingetId != nullptr;
}

bool startsWith(const std::wstring& s, const std::wstring& prefix) {
    return s.rfind(prefix, 0) == 0;
}

bool endsWith(const std::wstring& s, const std::wstring& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::wstring moduleDirectory() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L".";
    return fs::path(buf).parent_path().wstring();
}

std::wstring findBundledMavenZip() {
    const fs::path dir = fs::path(moduleDirectory()) / L"tools";
    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return {};
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const std::wstring name = entry.path().filename().wstring();
        if (startsWith(name, L"apache-maven-") && endsWith(name, L"-bin.zip")) {
            return entry.path().wstring();
        }
    }
    return {};
}

bool bundledMavenAvailable() {
    return !findBundledMavenZip().empty();
}

std::wstring runCaptureSilent(const std::wstring& command) {
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return {};
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr; si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::wstring c = L"cmd.exe /c " + command;
    std::vector<wchar_t> mut(c.begin(), c.end()); mut.push_back(L'\0');
    BOOL ok = CreateProcessW(nullptr, mut.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) { CloseHandle(rd); return {}; }

    std::string out;
    char buf[1024]; DWORD n = 0;
    while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n > 0) out.append(buf, n);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);

    while (!out.empty() && (out.back() == '\r' || out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return widen(out);
}

bool discoverVisualStudio() {
    g_vcvarsPath.clear();
    const wchar_t* candidates[] = {
        L"C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe",
        L"C:\\Program Files\\Microsoft Visual Studio\\Installer\\vswhere.exe",
    };
    std::wstring vswhere;
    for (const auto* p : candidates) {
        if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) { vswhere = p; break; }
    }
    if (vswhere.empty()) return false;

    std::wstring cmd = L"\"" + vswhere + L"\""
                       L" -latest -products * "
                       L"-requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 "
                       L"-property installationPath";
    std::wstring vsRoot = runCaptureSilent(cmd);
    if (vsRoot.empty()) return false;

    std::wstring vcvars = vsRoot + L"\\VC\\Auxiliary\\Build\\vcvars64.bat";
    if (GetFileAttributesW(vcvars.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    g_vcvarsPath = vcvars;
    return true;
}

bool isJdkRoot(const fs::path& root) {
    return fs::exists(root / L"bin" / L"javac.exe") && fs::exists(root / L"bin" / L"java.exe");
}

bool considerJdkRoot(const fs::path& root) {
    if (!isJdkRoot(root)) return false;
    g_jdkHome = root.wstring();
    return true;
}

bool scanJdkChildren(const fs::path& base) {
    std::error_code ec;
    if (!fs::exists(base, ec) || !fs::is_directory(base, ec)) return false;
    std::vector<fs::path> candidates;
    for (const auto& entry : fs::directory_iterator(base, ec)) {
        if (!entry.is_directory(ec)) continue;
        candidates.push_back(entry.path());
    }
    std::sort(candidates.begin(), candidates.end(), std::greater<fs::path>());
    for (const fs::path& candidate : candidates) {
        if (considerJdkRoot(candidate)) return true;
    }
    return false;
}

bool discoverJdk() {
    g_jdkHome.clear();
    if (SearchPathW(nullptr, L"javac.exe", nullptr, 0, nullptr, nullptr) > 0) {
        return true;
    }
    if (const wchar_t* javaHome = _wgetenv(L"JAVA_HOME")) {
        if (considerJdkRoot(javaHome)) return true;
    }
    const wchar_t* programFiles = _wgetenv(L"ProgramFiles");
    if (programFiles != nullptr && *programFiles != L'\0') {
        const fs::path pf(programFiles);
        if (scanJdkChildren(pf / L"Eclipse Adoptium")) return true;
        if (scanJdkChildren(pf / L"Microsoft")) return true;
        if (scanJdkChildren(pf / L"Java")) return true;
    }
    return false;
}

void rescanTools() {
    g_hasWinget = toolFound(L"winget.exe");
    g_hasSystemMaven = toolFound(L"mvn.cmd");
    g_hasBundledMaven = bundledMavenAvailable();
    discoverVisualStudio();
    discoverJdk();
    for (int i = 0; i < kToolCount; ++i) {
        bool found = toolFound(kTools[i].exeName);
        const std::wstring exeName = kTools[i].exeName;
        if (!found && exeName == L"mvn.cmd" && g_hasBundledMaven) {
            found = true;
        }
        if (!found && std::wstring(kTools[i].exeName) == L"cl.exe" && !g_vcvarsPath.empty()) {
            found = true;
        }
        if (!found && std::wstring(kTools[i].exeName) == L"javac.exe" && !g_jdkHome.empty()) {
            found = true;
        }
        g_toolUi[i].found = found;

        std::wstring statusText;
        if (found) {
            statusText = L"found";
            if (exeName == L"mvn.cmd" && !g_hasSystemMaven && g_hasBundledMaven) {
                statusText = L"bundled";
            }
            if (exeName == L"cl.exe" && !g_vcvarsPath.empty()
                && SearchPathW(nullptr, L"cl.exe", nullptr, 0, nullptr, nullptr) == 0) {
                statusText = L"found via vswhere";
            }
            if (exeName == L"javac.exe" && !g_jdkHome.empty()
                && SearchPathW(nullptr, L"javac.exe", nullptr, 0, nullptr, nullptr) == 0) {
                statusText = L"found via JDK folder";
            }
        } else {
            statusText = kTools[i].required ? L"missing" : L"optional";
        }
        SetWindowTextW(g_toolUi[i].statusLabel, statusText.c_str());
        InvalidateRect(g_toolUi[i].statusLabel, nullptr, TRUE);

        EnableWindow(g_toolUi[i].actionBtn, !found && canInstallTool(kTools[i]));
        InvalidateRect(g_toolUi[i].actionBtn, nullptr, FALSE);
    }
}

// =========================================================================
// Logging (worker -> UI, chunked)
// =========================================================================
void postLogChunk(const wchar_t* text, size_t chars) {
    if (chars == 0) return;
    size_t bytes = (chars + 1) * sizeof(wchar_t);
    HLOCAL h = LocalAlloc(LMEM_FIXED, bytes);
    if (!h) return;
    wchar_t* buf = static_cast<wchar_t*>(h);
    std::memcpy(buf, text, chars * sizeof(wchar_t));
    buf[chars] = L'\0';
    PostMessageW(g_hwnd, WM_APP_LOG_CHUNK, reinterpret_cast<WPARAM>(h), 0);
}

void postLog(const std::wstring& line) {
    std::wstring s = line;
    s.push_back(L'\r'); s.push_back(L'\n');
    postLogChunk(s.data(), s.size());
}
void postLog(const std::string& s) { postLog(widen(s)); }

void appendToLog(const wchar_t* text) {
    int len = GetWindowTextLengthW(g_logEdit);
    SendMessageW(g_logEdit, EM_SETSEL, len, len);
    SendMessageW(g_logEdit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text));
}

// =========================================================================
// Process runner with captured output
// =========================================================================
int runCaptured(const std::wstring& workDir, const std::wstring& command) {
    postLog(L"$ " + command);

    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) { postLog("[err] CreatePipe failed."); return -1; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr; si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::wstring c = L"cmd.exe /c " + command;
    std::vector<wchar_t> mut(c.begin(), c.end()); mut.push_back(L'\0');
    BOOL ok = CreateProcessW(nullptr, mut.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr,
                             workDir.empty() ? nullptr : workDir.c_str(),
                             &si, &pi);
    CloseHandle(wr);
    if (!ok) { CloseHandle(rd); postLog("[err] CreateProcess failed."); return -1; }

    char chunk[4096]; DWORD n = 0; std::string remainder;
    while (ReadFile(rd, chunk, sizeof(chunk), &n, nullptr) && n > 0) {
        std::string s = remainder + std::string(chunk, n);
        remainder.clear();
        std::string out; out.reserve(s.size());
        for (char ch : s) {
            if (ch == '\r') continue;
            if (ch == '\n') { out.push_back('\r'); out.push_back('\n'); continue; }
            out.push_back(ch);
        }
        if (!out.empty()) {
            std::wstring w = widen(out);
            postLogChunk(w.data(), w.size());
        }
        Sleep(0);
    }
    if (!remainder.empty()) postLog(remainder);

    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return static_cast<int>(exitCode);
}

// =========================================================================
// Per-tool install
// =========================================================================
void launchInstallForTool(int idx) {
    const ToolDef& t = kTools[idx];
    const wchar_t* packageId = t.wingetId;

    if (std::wstring(t.exeName) == L"mvn.cmd") {
        MessageBoxW(g_hwnd,
            L"Maven is bundled in installer.zip as tools\\apache-maven-*-bin.zip. "
            L"Put that tools folder beside HelixSeedInstaller.exe before running the installer.",
            kAppTitle, MB_ICONINFORMATION | MB_OK);
    } else if (!g_hasWinget) {
        MessageBoxW(g_hwnd,
            L"winget is required to install missing build tools. Install App Installer from Microsoft, "
            L"then reopen this installer.",
            kAppTitle, MB_ICONWARNING | MB_OK);
    } else if (packageId) {
        std::wstring cmd = L"winget install --id ";
        cmd += packageId;
        cmd += L" -e --accept-package-agreements --accept-source-agreements";
        if (std::wstring(packageId).find(L"BuildTools") != std::wstring::npos) {
            cmd += L" --override \"--add Microsoft.VisualStudio.Workload.VCTools "
                   L"--add Microsoft.VisualStudio.Component.Windows10SDK "
                   L"--includeRecommended --quiet --norestart\"";
        }
        ShellExecuteW(g_hwnd, L"open", L"cmd.exe", (L"/k " + cmd).c_str(), nullptr, SW_SHOWNORMAL);
        MessageBoxW(g_hwnd,
            L"Installing the package via winget. When the console finishes, "
            L"close it and the toolchain status here will refresh.",
            kAppTitle, MB_ICONINFORMATION | MB_OK);
    } else {
        MessageBoxW(g_hwnd,
            L"This tool is bundled or handled by the build and has no winget package configured here.",
            kAppTitle, MB_ICONWARNING | MB_OK);
    }
    PostMessageW(g_hwnd, WM_APP_RESCAN, 0, 0);
}

// =========================================================================
// Build pipeline (worker thread)
// =========================================================================
struct WorkerArgs {
    std::wstring installPath;
    bool hasMvn;
    std::wstring vcvarsPath;
    std::wstring jdkHome;
    BackendChoice backend;
};

std::wstring withVcvars(const std::wstring& vcvarsPath, const std::wstring& command) {
    if (vcvarsPath.empty()) return command;
    return L"call \"" + vcvarsPath + L"\" >nul && " + command;
}

bool pathExists(const std::wstring& p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring psSingleQuote(const std::wstring& s) {
    std::wstring out = L"'";
    for (wchar_t ch : s) {
        if (ch == L'\'') out += L"''";
        else out.push_back(ch);
    }
    out.push_back(L'\'');
    return out;
}

std::wstring findMavenHomeUnder(const fs::path& toolsRoot) {
    std::error_code ec;
    if (!fs::exists(toolsRoot, ec) || !fs::is_directory(toolsRoot, ec)) return {};
    std::vector<fs::path> candidates;
    for (const auto& entry : fs::directory_iterator(toolsRoot, ec)) {
        if (entry.is_directory(ec)) candidates.push_back(entry.path());
    }
    std::sort(candidates.begin(), candidates.end(), std::greater<fs::path>());
    for (const auto& candidate : candidates) {
        if (fs::exists(candidate / L"bin" / L"mvn.cmd", ec)) {
            return candidate.wstring();
        }
    }
    return {};
}

std::wstring ensureBundledMaven(const std::wstring& installPath) {
    const std::wstring zip = findBundledMavenZip();
    if (zip.empty()) return {};

    const fs::path toolsRoot = fs::path(installPath) / L".tools";
    if (std::wstring existing = findMavenHomeUnder(toolsRoot); !existing.empty()) {
        return existing;
    }

    std::error_code ec;
    fs::create_directories(toolsRoot, ec);
    if (ec) return {};

    postLog(L"[*] Extracting bundled Maven from " + zip);
    std::wstring cmd = L"powershell -NoProfile -ExecutionPolicy Bypass -Command ";
    cmd += L"\"Expand-Archive -LiteralPath ";
    cmd += psSingleQuote(zip);
    cmd += L" -DestinationPath ";
    cmd += psSingleQuote(toolsRoot.wstring());
    cmd += L" -Force\"";
    if (runCaptured(L"", cmd) != 0) return {};
    return findMavenHomeUnder(toolsRoot);
}

std::wstring withJdk(const std::wstring& jdkHome, const std::wstring& command) {
    if (jdkHome.empty()) return command;
    return L"set \"JAVA_HOME=" + jdkHome + L"\" && set \"PATH=" + jdkHome + L"\\bin;%PATH%\" && " + command;
}

std::wstring withMavenHome(const std::wstring& mavenHome, const std::wstring& command) {
    if (mavenHome.empty()) return command;
    return L"set \"MAVEN_HOME=" + mavenHome + L"\" && set \"PATH=" + mavenHome + L"\\bin;%PATH%\" && " + command;
}

void buildWorker(WorkerArgs args) {
    auto fail = [&](const std::string& msg) {
        postLog("[err] " + msg);
        PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
    };

    postLog(L"--- Starting install ---");
    postLog(L"Install path: " + args.installPath);

    std::error_code ec;
    if (fs::exists(args.installPath) && !fs::is_empty(args.installPath)) {
        postLog("[*] Path exists, wiping...");
        fs::remove_all(args.installPath, ec);
        if (ec) { fail("Wipe failed: " + ec.message()); return; }
    }
    fs::create_directories(fs::path(args.installPath).parent_path(), ec);

    postLog("[*] git clone...");
    {
        std::wstring cmd = L"git clone --depth=1 ";
        cmd += widen(kRepoUrl);
        cmd += L" \"" + args.installPath + L"\"";
        if (runCaptured(L"", cmd) != 0) { fail("git clone failed."); return; }
    }
    postLog("[ok] Source downloaded.");

    if (!args.vcvarsPath.empty()) postLog(L"[*] Using MSVC environment from " + args.vcvarsPath);

    const bool wantCuda   = (args.backend == BACKEND_CUDA || args.backend == BACKEND_BOTH);
    const bool wantOpenCL = (args.backend == BACKEND_OPENCL || args.backend == BACKEND_BOTH);

    if (wantCuda) {
        postLog("[*] Building CUDA prefilter...");
        if (runCaptured(args.installPath, withVcvars(args.vcvarsPath, L"cuda\\build_cuda.bat")) != 0) {
            // In "both" mode treat CUDA failure as non-fatal so OpenCL-only
            // users with nvcc installed but a broken CUDA toolchain still
            // get a working build. CUDA-only selection remains hard-fail.
            if (args.backend == BACKEND_CUDA) {
                fail("CUDA build failed."); return;
            }
            postLog("[warn] CUDA prefilter build failed; continuing with OpenCL only.");
        } else {
            postLog("[ok] CUDA prefilter built.");
        }
    } else {
        postLog("[*] Skipping CUDA prefilter (OpenCL-only selected).");
    }

    if (wantOpenCL) {
        postLog("[*] Building OpenCL prefilter...");
        if (runCaptured(args.installPath, withVcvars(args.vcvarsPath, L"opencl\\build_opencl.bat")) != 0) {
            if (args.backend == BACKEND_OPENCL) {
                fail("OpenCL build failed."); return;
            }
            postLog("[warn] OpenCL prefilter build failed; continuing with CUDA only.");
        } else {
            postLog("[ok] OpenCL prefilter built.");
        }
    } else {
        postLog("[*] Skipping OpenCL prefilter (CUDA-only selected).");
    }

    postLog("[*] Building native scanner...");
    if (runCaptured(args.installPath, withVcvars(args.vcvarsPath, L"native\\build_scanner_core.bat")) != 0) {
        fail("Native scanner build failed."); return;
    }
    postLog("[ok] Native scanner built.");

    // cubiomes_26.1.2_fork must be built before stage-resources can stage lib.dll
    if (fs::exists(fs::path(args.installPath) / "cubiomes_26.1.2_fork" / "build_windows.bat")) {
        postLog("[*] Building cubiomes (cubiomes_26.1.2_fork\\build_windows.bat)...");
        if (runCaptured(args.installPath + L"\\cubiomes_26.1.2_fork",
                        withVcvars(args.vcvarsPath, L"build_windows.bat")) != 0) {
            fail("cubiomes build failed."); return;
        }
        postLog("[ok] cubiomes built.");
    }

    {
        std::wstring lootDir = args.installPath + L"\\GPULootSeedFinder";
        if (!fs::exists(fs::path(lootDir) / "pom.xml")) {
            fail("GPULootSeedFinder pom.xml missing."); return;
        }

        const bool hasWrapper = pathExists(lootDir + L"\\mvnw.cmd");
        std::wstring mavenHome;
        std::wstring mvnCmd;
        if (args.hasMvn) {
            mvnCmd = L"mvn.cmd";
        } else {
            mavenHome = ensureBundledMaven(args.installPath);
            if (!mavenHome.empty()) {
                mvnCmd = L"\"" + mavenHome + L"\\bin\\mvn.cmd\"";
            } else if (hasWrapper) {
                mvnCmd = L"mvnw.cmd";
            }
        }
        if (mvnCmd.empty()) {
            fail("Maven is missing. installer.zip should include tools\\apache-maven-*-bin.zip."); return;
        }

        if (args.hasMvn) {
            postLog(L"[*] Building GPULootSeedFinder (system mvn.cmd)...");
        } else if (!mavenHome.empty()) {
            postLog(L"[*] Building GPULootSeedFinder (bundled Maven)...");
        } else {
            postLog(L"[*] Building GPULootSeedFinder (mvnw.cmd)...");
        }
        const std::wstring lootBuild =
            mvnCmd + L" -q -DskipTests package dependency:build-classpath "
                     L"\"-Dmdep.outputFile=target\\runtime-classpath.txt\"";
        if (runCaptured(lootDir, withJdk(args.jdkHome, withMavenHome(mavenHome, lootBuild))) != 0) {
            fail("Loot validator build failed."); return;
        }

        if (!fs::exists(fs::path(lootDir) / "target" / "classes" / "GPULootSeedFinder" / "LootValidationServer.class") ||
            !fs::exists(fs::path(lootDir) / "target" / "runtime-classpath.txt")) {
            fail("Loot validator built but required runtime files are missing."); return;
        }
        postLog("[ok] Loot validator built.");
    }

    std::wstring uiDir = args.installPath + L"\\ui-ts";
    if (!fs::exists(uiDir)) { fail("ui-ts directory missing."); return; }

    postLog("[*] npm install...");
    if (runCaptured(uiDir, L"npm install --no-audit --no-fund") != 0) {
        fail("npm install failed."); return;
    }
    postLog("[ok] npm dependencies installed.");

    postLog("[*] npm run dist:win (this is the long one)...");
    if (runCaptured(uiDir, L"npm run dist:win") != 0) {
        fail("dist:win build failed."); return;
    }
    postLog("[ok] UI built and packaged.");

    fs::path exePath = fs::path(args.installPath) / L"dist-ts" / L"HelixSeed-win32-x64" / L"HelixSeed.exe";
    if (!fs::exists(exePath)) { fail("Built but exe missing at " + exePath.string()); return; }

    std::wstring exeStr = exePath.wstring();
    size_t bytes = (exeStr.size() + 1) * sizeof(wchar_t);
    HLOCAL h = LocalAlloc(LMEM_FIXED, bytes);
    if (h) std::memcpy(h, exeStr.c_str(), bytes);
    PostMessageW(g_hwnd, WM_APP_DONE, 1, reinterpret_cast<LPARAM>(h));
}

// =========================================================================
// Folder picker
// =========================================================================
std::wstring pickFolder(HWND owner) {
    BROWSEINFOW bi{};
    bi.hwndOwner = owner;
    bi.lpszTitle = L"Choose install folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return {};
    wchar_t buf[MAX_PATH] = {};
    SHGetPathFromIDListW(pidl, buf);
    CoTaskMemFree(pidl);
    return std::wstring(buf);
}

// =========================================================================
// WM_PAINT - paint background, brand mark, panels, separators
// =========================================================================
void paintWindow(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);

    RECT clientRect; GetClientRect(hwnd, &clientRect);
    fillSolid(dc, clientRect, C_BG);

    // Topbar bottom border
    RECT divider{ 0, Y_TOPBAR_H - 1, clientRect.right, Y_TOPBAR_H };
    fillSolid(dc, divider, C_LINE_SOFT);

    // Brand mark
    RECT mark{ X_PAD, 20, X_PAD + 36, 56 };
    fillRoundRect(dc, mark, 7, C_APP_GREEN_DARK, C_APP_GREEN);
    if (g_appIcon) {
        DrawIconEx(dc, mark.left + 4, mark.top + 4, g_appIcon, 28, 28, 0, nullptr, DI_NORMAL);
    } else {
        drawCenteredText(dc, mark, L"HS", C_TEXT, g_fontStatus);
    }

    // Brand title + subtitle
    RECT title{ X_PAD + 48, 18, clientRect.right - X_PAD, 40 };
    drawLeftText(dc, title, L"HelixSeed Installer", C_TEXT, g_fontBrand);
    RECT sub{ X_PAD + 48, 40, clientRect.right - X_PAD, 60 };
    drawLeftText(dc, sub, L"Source installer: clones, builds, and packages the desktop app.",
                 C_MUTED, g_fontSmall);

    // ---- Panels (rounded fill + border, then header) ----
    auto paintPanel = [&](int x, int y, int w, int h, const wchar_t* title) {
        RECT r{ x, y, x + w, y + h };
        fillRoundRect(dc, r, 8, C_PANEL, C_LINE_SOFT);
        // Header divider line beneath the title
        RECT headerLine{ x + 1, y + 44, x + w - 1, y + 45 };
        fillSolid(dc, headerLine, C_LINE_SOFT);
        // Title text in the header strip
        RECT titleR{ x + 16, y, x + w, y + 44 };
        drawLeftText(dc, titleR, title, C_TEXT, g_fontBody);
    };

    paintPanel(X_PANEL, Y_PATH_PANEL,  W_PANEL, H_PATH_PANEL,  L"Install path");
    paintPanel(X_PANEL, Y_TOOLS_PANEL, W_PANEL, H_TOOLS_PANEL, L"Toolchain");
    paintPanel(X_PANEL, Y_LOG_PANEL,   W_PANEL, H_LOG_PANEL,   L"Log");

    // Status dots inside the toolchain panel
    int dotX = X_PANEL + 22;
    int rowY = Y_TOOLS_PANEL + 60;
    for (int i = 0; i < kToolCount; ++i) {
        COLORREF c;
        if (g_toolUi[i].found)         c = C_GREEN;
        else if (kTools[i].required)   c = C_RED;
        else                           c = C_SUBTLE;
        RECT dot{ dotX, rowY + 11, dotX + 8, rowY + 19 };
        // small filled circle (rounded rect)
        fillRoundRect(dc, dot, 4, c, c);
        rowY += H_TOOL_ROW;
    }

    EndPaint(hwnd, &ps);
}
// =========================================================================
// Layout (controls placed inside painted panels)
// =========================================================================
void enableDarkTitleBar(HWND h) {
    BOOL dark = TRUE;
    DwmSetWindowAttribute(h, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    DwmSetWindowAttribute(h, 19, &dark, sizeof(dark));  // older Win10
}

void buildUi(HWND hwnd) {
    g_brushBg        = CreateSolidBrush(C_BG);
    g_brushPanel     = CreateSolidBrush(C_PANEL);
    g_brushPanelHigh = CreateSolidBrush(C_PANEL_HIGH);
    g_brushInput     = CreateSolidBrush(C_INPUT_BG);

    auto mkFont = [](int height, int weight, bool mono) {
        return CreateFontW(-height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY,
                           mono ? (FIXED_PITCH | FF_MODERN) : (DEFAULT_PITCH | FF_SWISS),
                           mono ? L"Consolas" : L"Segoe UI");
    };
    g_fontBrand  = mkFont(18, FW_SEMIBOLD, false);
    g_fontBody   = mkFont(13, FW_NORMAL,   false);
    g_fontSmall  = mkFont(11, FW_NORMAL,   false);
    g_fontStatus = mkFont(13, FW_SEMIBOLD, false);
    g_fontMono   = mkFont(12, FW_NORMAL,   true);

    // ---- Path panel contents ----
    g_pathEdit = CreateWindowExW(0, L"EDIT", defaultInstallPath().c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        X_PANEL + 16, Y_PATH_PANEL + 56,
        W_PANEL - 232, 32,
        hwnd, reinterpret_cast<HMENU>((INT_PTR)ID_PATH_EDIT), nullptr, nullptr);
    SendMessageW(g_pathEdit, WM_SETFONT, reinterpret_cast<WPARAM>(g_fontBody), TRUE);
    SetWindowTheme(g_pathEdit, L"DarkMode_Explorer", nullptr);

    g_browseBtn  = createButton(hwnd, L"Browse...", ID_BROWSE_BTN,
                                X_PANEL + W_PANEL - 212, Y_PATH_PANEL + 56, 95, 32);
    g_defaultBtn = createButton(hwnd, L"Default",   ID_DEFAULT_BTN,
                                X_PANEL + W_PANEL - 110, Y_PATH_PANEL + 56, 95, 32);

    // ---- Toolchain rows (text + status + install button) ----
    int rowY = Y_TOOLS_PANEL + 56;
    for (int i = 0; i < kToolCount; ++i) {
        int textX = X_PANEL + 42;
        int textW = 150;

        g_toolUi[i].nameLabel = createLabel(hwnd, kTools[i].displayName,
            textX, rowY + 6, 60, 20, g_fontBody, ID_TOOL_BASE + i * 10);
        g_toolUi[i].descLabel = createLabel(hwnd, kTools[i].description,
            textX + 60, rowY + 6, W_PANEL - 380, 20, g_fontSmall, ID_TOOL_BASE + i * 10 + 5);
        g_toolUi[i].statusLabel = createLabel(hwnd, L"...",
            X_PANEL + W_PANEL - 290, rowY + 6, 170, 20, g_fontBody, ID_TOOL_BASE + i * 10 + 6);
        g_toolUi[i].actionBtn = createButton(hwnd, L"Install",
            ID_TOOL_BASE + i * 10 + 1,
            X_PANEL + W_PANEL - 110, rowY + 1, 95, 28);
        EnableWindow(g_toolUi[i].actionBtn, FALSE);

        rowY += H_TOOL_ROW;
        (void)textW;
    }

    // ---- Log edit ----
    g_logEdit = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL |
        ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
        X_PANEL + 16, Y_LOG_PANEL + 56,
        W_PANEL - 32, H_LOG_PANEL - 72,
        hwnd, reinterpret_cast<HMENU>((INT_PTR)ID_LOG_EDIT), nullptr, nullptr);
    SendMessageW(g_logEdit, WM_SETFONT, reinterpret_cast<WPARAM>(g_fontMono), TRUE);
    SetWindowTheme(g_logEdit, L"DarkMode_Explorer", nullptr);

    // ---- Action buttons ----
    g_installBtn = createButton(hwnd, L"Install HelixSeed", ID_INSTALL_BTN,
        X_PANEL, Y_ACTION_BAR, 200, H_ACTION, true, true, C_BG);
    g_launchBtn  = createButton(hwnd, L"Launch", ID_LAUNCH_BTN,
        X_PANEL + 212, Y_ACTION_BAR, 110, H_ACTION, false, false, C_BG);
    g_openBtn    = createButton(hwnd, L"Open Folder", ID_OPEN_BTN,
        X_PANEL + 330, Y_ACTION_BAR, 130, H_ACTION, false, false, C_BG);

    // GPU backend selector. Sits between Open Folder and Close. Default to
    // Both so existing users get the same behaviour with no extra clicks.
    const int comboY = Y_ACTION_BAR + (H_ACTION - 24) / 2;
    g_backendLabel = CreateWindowExW(0, L"STATIC", L"GPU backend:",
        WS_CHILD | WS_VISIBLE | SS_RIGHT,
        X_PANEL + 470, comboY + 4, 88, 18,
        hwnd, nullptr, nullptr, nullptr);
    SendMessageW(g_backendLabel, WM_SETFONT, reinterpret_cast<WPARAM>(g_fontSmall), TRUE);
    g_backendCombo = CreateWindowExW(0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
        X_PANEL + 562, comboY, 100, 200,
        hwnd, reinterpret_cast<HMENU>((INT_PTR)ID_BACKEND_COMBO), nullptr, nullptr);
    SendMessageW(g_backendCombo, WM_SETFONT, reinterpret_cast<WPARAM>(g_fontBody), TRUE);
    SendMessageW(g_backendCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Both"));
    SendMessageW(g_backendCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"CUDA only"));
    SendMessageW(g_backendCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"OpenCL only"));
    SendMessageW(g_backendCombo, CB_SETCURSEL, BACKEND_BOTH, 0);

    g_closeBtn   = createButton(hwnd, L"Close", ID_CLOSE_BTN,
        X_PANEL + W_PANEL - 110, Y_ACTION_BAR, 110, H_ACTION, false, false, C_BG);

    EnableWindow(g_launchBtn, FALSE);
    EnableWindow(g_openBtn, FALSE);
}

// =========================================================================
// Action handlers
// =========================================================================
void onInstallClicked(HWND hwnd) {
    if (g_buildRunning.load()) return;

    int n = GetWindowTextLengthW(g_pathEdit);
    std::wstring path((size_t)n, L'\0');
    GetWindowTextW(g_pathEdit, path.data(), n + 1);

    if (path.empty()) {
        MessageBoxW(hwnd, L"Choose an install path first.", kAppTitle, MB_ICONWARNING | MB_OK);
        return;
    }

    const LRESULT comboSel = SendMessageW(g_backendCombo, CB_GETCURSEL, 0, 0);
    BackendChoice backend = BACKEND_BOTH;
    if (comboSel == BACKEND_CUDA || comboSel == BACKEND_OPENCL) {
        backend = static_cast<BackendChoice>(comboSel);
    }

    bool requiredOk = true;
    for (int i = 0; i < kToolCount; ++i) {
        if (!kTools[i].required) continue;
        // nvcc is only required when building the CUDA backend.
        if (std::wstring(kTools[i].exeName) == L"nvcc.exe" && backend == BACKEND_OPENCL) {
            continue;
        }
        if (!g_toolUi[i].found) { requiredOk = false; break; }
    }
    if (!requiredOk) {
        MessageBoxW(hwnd,
            L"Required tools are missing.\n\nUse the per-tool Install buttons "
            L"in the Toolchain panel, then click Install HelixSeed again.",
            kAppTitle, MB_ICONWARNING | MB_OK);
        return;
    }

    if (fs::exists(path) && !fs::is_empty(path)) {
        std::wstring q = L"The path \"" + path + L"\" already exists and is not empty.\n\nWipe and continue?";
        if (MessageBoxW(hwnd, q.c_str(), kAppTitle, MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES) return;
    }

    g_buildRunning.store(true);
    EnableWindow(g_installBtn, FALSE);
    EnableWindow(g_browseBtn, FALSE);
    EnableWindow(g_defaultBtn, FALSE);
    EnableWindow(g_pathEdit, FALSE);
    if (g_backendCombo) EnableWindow(g_backendCombo, FALSE);
    SetWindowTextW(g_logEdit, L"");

    bool clOnPath = SearchPathW(nullptr, L"cl.exe", nullptr, 0, nullptr, nullptr) > 0;
    bool hasMvn = g_hasSystemMaven;
    WorkerArgs args{ path, hasMvn, clOnPath ? L"" : g_vcvarsPath, g_jdkHome, backend };
    std::thread([args]() { buildWorker(args); }).detach();
}

void onBrowseClicked(HWND hwnd) {
    std::wstring chosen = pickFolder(hwnd);
    if (chosen.empty()) return;
    if (fs::is_directory(chosen)) chosen += L"\\HelixSeed";
    SetWindowTextW(g_pathEdit, chosen.c_str());
}

void onDone(WPARAM success, LPARAM lparam) {
    g_buildRunning.store(false);
    EnableWindow(g_installBtn, TRUE);
    EnableWindow(g_browseBtn, TRUE);
    EnableWindow(g_defaultBtn, TRUE);
    EnableWindow(g_pathEdit, TRUE);
    if (g_backendCombo) EnableWindow(g_backendCombo, TRUE);

    if (success && lparam) {
        const wchar_t* p = reinterpret_cast<const wchar_t*>(lparam);
        g_installedExePath = p;
        LocalFree(reinterpret_cast<HLOCAL>(lparam));
        appendToLog((L"--- Done. Exe: " + g_installedExePath + L"\r\n").c_str());
        EnableWindow(g_launchBtn, TRUE);
        EnableWindow(g_openBtn, TRUE);
        MessageBoxW(g_hwnd, L"HelixSeed installed successfully.", kAppTitle, MB_ICONINFORMATION | MB_OK);
    } else {
        appendToLog(L"--- Install failed. See log above. ---\r\n");
        MessageBoxW(g_hwnd, L"Install failed. See the log for details.", kAppTitle, MB_ICONERROR | MB_OK);
    }
}

void launchInstalled() {
    if (g_installedExePath.empty()) return;
    fs::path p = g_installedExePath;
    ShellExecuteW(g_hwnd, L"open", p.c_str(), nullptr, p.parent_path().c_str(), SW_SHOWNORMAL);
}

void openFolder() {
    if (g_installedExePath.empty()) return;
    fs::path p = fs::path(g_installedExePath).parent_path();
    ShellExecuteW(g_hwnd, L"explore", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// =========================================================================
// WndProc
// =========================================================================
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            g_hwnd = hwnd;
            enableDarkTitleBar(hwnd);
            buildUi(hwnd);
            rescanTools();
            return 0;

        case WM_PAINT:
            paintWindow(hwnd);
            return 0;

        case WM_ERASEBKGND:
            // We paint everything in WM_PAINT.
            return 1;

        case WM_DRAWITEM: {
            LPDRAWITEMSTRUCT dis = reinterpret_cast<LPDRAWITEMSTRUCT>(lp);
            if (dis->CtlType == ODT_BUTTON) {
                drawCustomButton(dis);
                return TRUE;
            }
            break;
        }

        case WM_CTLCOLORSTATIC: {
            HDC hdc = reinterpret_cast<HDC>(wp);
            HWND ctl = reinterpret_cast<HWND>(lp);
            SetBkMode(hdc, TRANSPARENT);

            // Tool name: bright white
            for (int i = 0; i < kToolCount; ++i) {
                if (ctl == g_toolUi[i].nameLabel) {
                    SetTextColor(hdc, C_TEXT);
                    return reinterpret_cast<LRESULT>(g_brushPanel);
                }
                if (ctl == g_toolUi[i].descLabel) {
                    SetTextColor(hdc, C_MUTED);
                    return reinterpret_cast<LRESULT>(g_brushPanel);
                }
                if (ctl == g_toolUi[i].statusLabel) {
                    if (g_toolUi[i].found)            SetTextColor(hdc, C_GREEN);
                    else if (kTools[i].required)      SetTextColor(hdc, C_RED);
                    else                               SetTextColor(hdc, C_SUBTLE);
                    return reinterpret_cast<LRESULT>(g_brushPanel);
                }
            }
            // Read-only EDIT (log box) sends WM_CTLCOLORSTATIC
            if (ctl == g_logEdit) {
                SetTextColor(hdc, C_TEXT);
                SetBkColor(hdc, C_INPUT_BG);
                return reinterpret_cast<LRESULT>(g_brushInput);
            }
            // Default (panel-coloured)
            SetTextColor(hdc, C_TEXT);
            return reinterpret_cast<LRESULT>(g_brushPanel);
        }

        case WM_CTLCOLOREDIT: {
            HDC hdc = reinterpret_cast<HDC>(wp);
            SetTextColor(hdc, C_TEXT);
            SetBkColor(hdc, C_INPUT_BG);
            SetBkMode(hdc, OPAQUE);
            return reinterpret_cast<LRESULT>(g_brushInput);
        }

        case WM_APP_LOG_CHUNK: {
            const wchar_t* text = reinterpret_cast<const wchar_t*>(wp);
            appendToLog(text);
            LocalFree(reinterpret_cast<HLOCAL>(wp));
            return 0;
        }

        case WM_APP_DONE:
            onDone(wp, lp);
            return 0;

        case WM_APP_RESCAN:
            SetTimer(hwnd, 1, 1500, nullptr);
            return 0;

        case WM_TIMER:
            if (wp == 1) { KillTimer(hwnd, 1); rescanTools(); }
            return 0;

        case WM_COMMAND: {
            int id = LOWORD(wp);
            switch (id) {
                case ID_INSTALL_BTN: onInstallClicked(hwnd); return 0;
                case ID_BROWSE_BTN:  onBrowseClicked(hwnd);  return 0;
                case ID_DEFAULT_BTN: SetWindowTextW(g_pathEdit, defaultInstallPath().c_str()); return 0;
                case ID_LAUNCH_BTN:  launchInstalled();      return 0;
                case ID_OPEN_BTN:    openFolder();           return 0;
                case ID_CLOSE_BTN:   PostMessageW(hwnd, WM_CLOSE, 0, 0); return 0;
            }
            if (id >= ID_TOOL_BASE && id < ID_TOOL_BASE + kToolCount * 10 + 2) {
                int rel = id - ID_TOOL_BASE;
                int toolIdx = rel / 10;
                int kind = rel % 10;
                if (kind == 1 && toolIdx >= 0 && toolIdx < kToolCount) {
                    launchInstallForTool(toolIdx);
                }
            }
            break;
        }

        case WM_CLOSE:
            if (g_buildRunning.load()) {
                int r = MessageBoxW(hwnd, L"Install is still running. Quit anyway?",
                                    kAppTitle, MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2);
                if (r != IDYES) return 0;
            }
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            for (auto& b : g_buttons) RemoveWindowSubclass(b.hwnd, ButtonSubclassProc, 1);
            if (g_fontBrand)  DeleteObject(g_fontBrand);
            if (g_fontBody)   DeleteObject(g_fontBody);
            if (g_fontSmall)  DeleteObject(g_fontSmall);
            if (g_fontStatus) DeleteObject(g_fontStatus);
            if (g_fontMono)   DeleteObject(g_fontMono);
            if (g_brushBg)        DeleteObject(g_brushBg);
            if (g_brushPanel)     DeleteObject(g_brushPanel);
            if (g_brushPanelHigh) DeleteObject(g_brushPanelHigh);
            if (g_brushInput)     DeleteObject(g_brushInput);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    OleInitialize(nullptr);
    g_appIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // we paint everything
    wc.lpszClassName = kClassName;
    wc.hIcon = g_appIcon ? g_appIcon : LoadIcon(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    if (!RegisterClassExW(&wc)) return 1;

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    HWND hwnd = CreateWindowExW(
        0, kClassName, kAppTitle,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
        (sw - W_WINDOW) / 2, (sh - H_WINDOW) / 2, W_WINDOW, H_WINDOW,
        nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) return 1;
    if (g_appIcon) {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_appIcon));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_appIcon));
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    OleUninitialize();
    return static_cast<int>(msg.wParam);
}
