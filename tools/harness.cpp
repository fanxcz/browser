// browser_harness — remote-control test harness for Browser.exe.
//
// Attaches to the browser's console (AttachConsole + WriteConsoleInputW),
// injects real key/mouse INPUT_RECORDs, reads the screen buffer back and
// captures the window to PNG. Lets an agent verify every mechanic live.
//
// Usage:
//   browser_harness run <script.txt>     execute a script of commands
//   browser_harness <cmd> [args...]      single command (needs attach first
//                                        in the same process — prefer run)
//
// Script commands (one per line, # comments allowed):
//   launch [path]            start Browser.exe (default Release build)
//   attach <pid>             attach to a running instance
//   attachname <exe>         attach by process image name
//   wait <ms>                sleep
//   key <spec>               key press+release; spec like c, enter, f5,
//                            ctrl+c, ctrl+shift+t, alt+left
//   text <anything>          type literal text (rest of line)
//   down <x> <y> [btn]       mouse press at window-relative coords
//   move <x> <y>             mouse move (drag while button held)
//   up <x> <y> [btn]         mouse release
//   wheel <x> <y> <notches>  mouse wheel at position
//   dblclick <x> <y>         double left click
//   size                     print window size in cells
//   dump [file]              print screen buffer text (or write to file)
//   shot <file.png>          capture the console window to PNG
//   clip                     print clipboard text
//   expect <text>            PASS/FAIL: text visible on screen
//   expectclip <text>        PASS/FAIL: text in clipboard
//   echo <text>              print a marker line
//
// Coordinates are WINDOW-relative cells (what the user sees), exactly like
// a human pointing at the screen. Exit code of `run`: 0 if every expect
// passed, 1 otherwise.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "user32.lib")

// ---------------------------------------------------------------------------
// GDI+ for PNG screenshots (loaded dynamically-free via pragma below)
// ---------------------------------------------------------------------------
#include <objbase.h>
#include <gdiplus.h>
#include <gdiplusimaging.h>
#pragma comment(lib, "gdiplus.lib")

using std::string;
using std::wstring;

static HANDLE g_in = nullptr;
static HANDLE g_out = nullptr;
static HWND g_hwnd = nullptr;
static int g_winLeft = 0, g_winTop = 0;
static int g_winW = 0, g_winH = 0;
static int g_failures = 0;
static int g_checks = 0;

static wstring ToWide(const string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
static string ToUtf8(const wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n,
                        nullptr, nullptr);
    return s;
}
static wstring LowerWide(wstring s) {
    for (auto& c : s) {
        // towlower() is locale-bound and leaves Cyrillic untouched in
        // the C locale — fold ASCII and Cyrillic manually instead.
        if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
        else if (c >= 0x0410 && c <= 0x042F) c = (wchar_t)(c + 0x20);
        else c = (wchar_t)towlower(c);
    }
    return s;
}
static string Trim(const string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// ---------------------------------------------------------------------------
// Console attach / geometry
// ---------------------------------------------------------------------------

static bool RefreshGeometry() {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!GetConsoleScreenBufferInfo(g_out, &info)) return false;
    g_winLeft = info.srWindow.Left;
    g_winTop = info.srWindow.Top;
    g_winW = info.srWindow.Right - info.srWindow.Left + 1;
    g_winH = info.srWindow.Bottom - info.srWindow.Top + 1;
    return true;
}

static bool AttachTo(DWORD pid) {
    FreeConsole();
    for (int i = 0; i < 50 && !AttachConsole(pid); ++i) Sleep(100);
    if (!AttachConsole(pid)) {
        // AttachConsole fails if already attached — retry once after free.
        FreeConsole();
        if (!AttachConsole(pid)) {
            std::cout << "AttachConsole failed: " << GetLastError() << "\n";
            return false;
        }
    }
    // Std handles may not be console handles after AttachConsole — open
    // the console devices explicitly instead.
    g_in = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                       OPEN_EXISTING, 0, nullptr);
    g_out = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                        OPEN_EXISTING, 0, nullptr);
    if (g_in == INVALID_HANDLE_VALUE || g_out == INVALID_HANDLE_VALUE) {
        std::cout << "std handles invalid: in=" << (void*)g_in
                  << " out=" << (void*)g_out << "\n";
        return false;
    }
    g_hwnd = GetConsoleWindow();
    if (!RefreshGeometry()) {
        std::cout << "geometry failed: " << GetLastError() << "\n";
        return false;
    }
    return true;
}

static DWORD PidByName(const wchar_t* exe) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{sizeof(pe)};
    DWORD pid = 0;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, exe) == 0) { pid = pe.th32ProcessID; break; }
    }
    CloseHandle(snap);
    return pid;
}

static bool LaunchBrowser(const string& path) {
    string cmd = "\"" + (path.empty()
        ? string("D:\\Browser-3.4\\build\\Release\\Browser.exe") : path) + "\"";
    STARTUPINFOA si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    std::vector<char> cl(cmd.begin(), cmd.end());
    cl.push_back('\0');
    if (!CreateProcessA(nullptr, cl.data(), nullptr, nullptr, FALSE,
                        0, nullptr, nullptr, &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    const DWORD pid = pi.dwProcessId;
    CloseHandle(pi.hProcess);
    Sleep(800);
    return AttachTo(pid);
}

// ---------------------------------------------------------------------------
// Input injection
// ---------------------------------------------------------------------------

static DWORD ModsToState(const string& mods) {
    DWORD s = 0;
    if (mods.find("ctrl") != string::npos) s |= LEFT_CTRL_PRESSED;
    if (mods.find("shift") != string::npos) s |= SHIFT_PRESSED;
    if (mods.find("alt") != string::npos) s |= LEFT_ALT_PRESSED;
    return s;
}

static WORD VkForName(const string& name, wchar_t& ch) {
    static const struct { const char* n; WORD vk; } map[] = {
        {"enter", VK_RETURN}, {"return", VK_RETURN}, {"esc", VK_ESCAPE},
        {"escape", VK_ESCAPE}, {"tab", VK_TAB}, {"backspace", VK_BACK},
        {"bksp", VK_BACK}, {"delete", VK_DELETE}, {"del", VK_DELETE},
        {"insert", VK_INSERT}, {"ins", VK_INSERT},
        {"up", VK_UP}, {"down", VK_DOWN}, {"left", VK_LEFT}, {"right", VK_RIGHT},
        {"home", VK_HOME}, {"end", VK_END},
        {"pgup", VK_PRIOR}, {"pgdn", VK_NEXT}, {"pageup", VK_PRIOR},
        {"pagedown", VK_NEXT}, {"space", VK_SPACE},
        {"f1", VK_F1}, {"f2", VK_F2}, {"f3", VK_F3}, {"f4", VK_F4},
        {"f5", VK_F5}, {"f6", VK_F6}, {"f7", VK_F7}, {"f8", VK_F8},
        {"f9", VK_F9}, {"f10", VK_F10}, {"f11", VK_F11}, {"f12", VK_F12},
        {"pause", VK_PAUSE}, {"scrolllock", VK_SCROLL},
        {"printscreen", VK_SNAPSHOT}, {"backtab", VK_BACK}, // approx
    };
    string low = name;
    for (auto& c : low) c = (char)tolower((unsigned char)c);
    if (low.size() == 1) {
        ch = (wchar_t)low[0];
        return (WORD)towupper((unsigned char)low[0]);
    }
    for (const auto& m : map)
        if (low == m.n) { ch = 0; return m.vk; }
    if (low.size() >= 2 && low[0] == 'f') {
        int n = atoi(low.c_str() + 1);
        if (n >= 1 && n <= 24) { ch = 0; return (WORD)(VK_F1 + n - 1); }
    }
    return 0;
}

static void SendKeyEvent(bool down, WORD vk, wchar_t ch, DWORD mods) {
    INPUT_RECORD r{};
    r.EventType = KEY_EVENT;
    r.Event.KeyEvent.bKeyDown = down ? TRUE : FALSE;
    r.Event.KeyEvent.wRepeatCount = 1;
    r.Event.KeyEvent.wVirtualKeyCode = vk;
    r.Event.KeyEvent.uChar.UnicodeChar = down ? ch : 0;
    r.Event.KeyEvent.dwControlKeyState = mods;
    DWORD written = 0;
    WriteConsoleInputW(g_in, &r, 1, &written);
}

static void DoKey(const string& spec) {
    // spec: [ctrl+][shift+][alt+]key
    string s = spec;
    string modsStr;
    size_t plus;
    while ((plus = s.find('+')) != string::npos && plus > 0 &&
           (s.substr(0, plus) == "ctrl" || s.substr(0, plus) == "shift" ||
            s.substr(0, plus) == "alt" || modsStr.find(s.substr(0, plus)) != string::npos)) {
        if (!modsStr.empty()) modsStr += "+";
        modsStr += s.substr(0, plus);
        s.erase(0, plus + 1);
    }
    wchar_t ch = 0;
    WORD vk = VkForName(s, ch);
    if (!vk) { std::cout << "harness: unknown key '" << s << "'\n"; ++g_failures; return; }
    DWORD mods = ModsToState(modsStr);
    if (ch == 0 && vk >= 'A' && vk <= 'Z')
        ch = (wchar_t)(vk + (mods & SHIFT_PRESSED ? 0 : 32));
    SendKeyEvent(true, vk, ch, mods);
    SendKeyEvent(false, vk, 0, mods);
}

static void SendChar(wchar_t wc, WORD vk, DWORD mods) {
    SendKeyEvent(true, vk, wc, mods);
    SendKeyEvent(false, vk, 0, mods);
}

static void DoText(const wstring& text) {
    // Explicit US-layout map — VkKeyScanW proved unreliable here (it lost
    // plain letters). The console inserts uChar literally, so the vk just
    // needs to be a sane printable key code.
    static const struct { wchar_t ch; WORD vk; bool shift; } sym[] = {
        {L' ', VK_SPACE, false}, {L'!', L'1', true}, {L'@', L'2', true},
        {L'#', L'3', true}, {L'$', L'4', true}, {L'%', L'5', true},
        {L'^', L'6', true}, {L'&', L'7', true}, {L'*', L'8', true},
        {L'(', L'9', true}, {L')', L'0', true},
        {L'-', VK_OEM_MINUS, false}, {L'_', VK_OEM_MINUS, true},
        {L'=', VK_OEM_PLUS, false}, {L'+', VK_OEM_PLUS, true},
        {L'[', VK_OEM_4, false}, {L'{', VK_OEM_4, true},
        {L']', VK_OEM_6, false}, {L'}', VK_OEM_6, true},
        {L';', VK_OEM_1, false}, {L':', VK_OEM_1, true},
        {L'\'', VK_OEM_7, false}, {L'"', VK_OEM_7, true},
        {L'`', VK_OEM_3, false}, {L'~', VK_OEM_3, true},
        {L',', VK_OEM_COMMA, false}, {L'<', VK_OEM_COMMA, true},
        {L'.', VK_OEM_PERIOD, false}, {L'>', VK_OEM_PERIOD, true},
        {L'/', VK_OEM_2, false}, {L'?', VK_OEM_2, true},
        {L'\\', VK_OEM_5, false}, {L'|', VK_OEM_5, true},
    };
    for (wchar_t wc : text) {
        if (wc == L'\t') { DoKey("tab"); continue; }
        if (wc == L'\n' || wc == L'\r') { DoKey("enter"); continue; }
        if (wc >= L'a' && wc <= L'z') {
            SendChar(wc, (WORD)towupper(wc), 0);
            continue;
        }
        if (wc >= L'A' && wc <= L'Z') {
            SendChar(wc, (WORD)wc, SHIFT_PRESSED);
            continue;
        }
        if (wc >= L'0' && wc <= L'9') {
            SendChar(wc, (WORD)wc, 0);
            continue;
        }
        bool done = false;
        for (const auto& s : sym) {
            if (s.ch == wc) {
                SendChar(wc, s.vk, s.shift ? SHIFT_PRESSED : 0);
                done = true;
                break;
            }
        }
        if (!done && wc >= 0x80) {
            // Non-ASCII (Cyrillic etc.): vk=0, rely on uChar alone.
            SendChar(wc, 0, 0);
        }
    }
}

// ---------------------------------------------------------------------------
// Mouse injection (coords are window-relative cells)
// ---------------------------------------------------------------------------

static COORD WinToBuffer(int x, int y) {
    return {(SHORT)(g_winLeft + x), (SHORT)(g_winTop + y)};
}

static DWORD BtnFlag(const string& btn, bool second = false) {
    string b = btn.empty() ? "left" : btn;
    if (b == "right") return RIGHTMOST_BUTTON_PRESSED;
    if (b == "middle") return FROM_LEFT_4TH_BUTTON_PRESSED;
    return second ? FROM_LEFT_2ND_BUTTON_PRESSED : FROM_LEFT_1ST_BUTTON_PRESSED;
}

static void SendMouse(COORD pos, DWORD buttonState, DWORD eventFlags,
                      bool keepButton = false) {
    INPUT_RECORD r{};
    r.EventType = MOUSE_EVENT;
    r.Event.MouseEvent.dwMousePosition = pos;
    r.Event.MouseEvent.dwButtonState = buttonState;
    r.Event.MouseEvent.dwControlKeyState = 0;
    r.Event.MouseEvent.dwEventFlags = eventFlags;
    DWORD written = 0;
    WriteConsoleInputW(g_in, &r, 1, &written);
    (void)keepButton;
}

static int g_heldBtn = 0; // window-relative button state while dragging

static void DoMouse(const string& action, int x, int y,
                    const string& btn = "left") {
    COORD pos = WinToBuffer(x, y);
    if (action == "down") {
        g_heldBtn = 1;
        SendMouse(pos, BtnFlag(btn), 0);
    } else if (action == "move") {
        SendMouse(pos, g_heldBtn ? BtnFlag("left") : 0, MOUSE_MOVED);
    } else if (action == "up") {
        SendMouse(pos, 0, 0);
        g_heldBtn = 0;
    } else if (action == "wheel") {
        // notches passed via btn field hack: encoded in `btn` arg as int
        int notches = atoi(btn.c_str());
        if (!notches) notches = 1;
        INPUT_RECORD r{};
        r.EventType = MOUSE_EVENT;
        r.Event.MouseEvent.dwMousePosition = pos;
        r.Event.MouseEvent.dwButtonState =
            (DWORD)(short)(notches * WHEEL_DELTA) << 16;
        r.Event.MouseEvent.dwEventFlags = MOUSE_WHEELED;
        DWORD written = 0;
        WriteConsoleInputW(g_in, &r, 1, &written);
    } else if (action == "dblclick") {
        SendMouse(pos, BtnFlag(btn), DOUBLE_CLICK);
        SendMouse(pos, 0, 0);
        SendMouse(pos, BtnFlag(btn), DOUBLE_CLICK);
        SendMouse(pos, 0, 0);
    }
}

// ---------------------------------------------------------------------------
// Screen read-back
// ---------------------------------------------------------------------------

static std::vector<wstring> ReadScreen() {
    std::vector<wstring> rows;
    if (!RefreshGeometry()) return rows;
    rows.resize((size_t)g_winH);
    for (int y = 0; y < g_winH; ++y) {
        wstring line((size_t)g_winW, L' ');
        COORD pos{(SHORT)g_winLeft, (SHORT)(g_winTop + y)};
        DWORD rd = 0;
        ReadConsoleOutputCharacterW(g_out, line.data(), (DWORD)g_winW,
                                    pos, &rd);
        while (!line.empty() && line.back() == L' ') line.pop_back();
        rows[(size_t)y] = std::move(line);
    }
    return rows;
}

static void DoDump(const string& file) {
    auto rows = ReadScreen();
    std::ostringstream ss;
    for (const auto& r : rows) ss << ToUtf8(r) << "\n";
    if (file.empty()) {
        std::cout << ss.str();
    } else {
        std::ofstream f(file, std::ios::binary);
        f << ss.str();
        std::cout << "dumped " << rows.size() << " rows -> " << file << "\n";
    }
}

static void DoExpect(const string& needle, bool clip, bool negate = false) {
    ++g_checks;
    wstring n = LowerWide(ToWide(needle));
    bool found = false;
    string where;
    if (clip) {
        if (OpenClipboard(nullptr)) {
            if (HANDLE hd = GetClipboardData(CF_UNICODETEXT)) {
                if (const wchar_t* p = (const wchar_t*)GlobalLock(hd)) {
                    found = LowerWide(p).find(n) != wstring::npos;
                    GlobalUnlock(hd);
                }
            }
            CloseClipboard();
        }
    } else {
        for (auto& row : ReadScreen()) {
            if (LowerWide(row).find(n) != wstring::npos) {
                found = true;
                where = ToUtf8(row);
                break;
            }
        }
    }
    const bool pass = negate ? !found : found;
    if (pass) {
        std::cout << "PASS expect" << (clip ? "clip" : "")
                  << (negate ? "not " : " ") << "'"
                  << needle << "'";
        if (!where.empty() && !negate) std::cout << "   | " << where;
        std::cout << "\n";
    } else {
        ++g_failures;
        std::cout << "FAIL expect" << (clip ? "clip" : "")
                  << (negate ? "not " : " ") << "'"
                  << needle << "'" << (negate ? " still visible" : " not found")
                  << "\n";
    }
}

static void DoClip() {
    if (!OpenClipboard(nullptr)) { std::cout << "(clipboard busy)\n"; return; }
    if (HANDLE hd = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* p = (const wchar_t*)GlobalLock(hd)) {
            std::cout << ToUtf8(p) << "\n";
            GlobalUnlock(hd);
        }
    } else std::cout << "(empty)\n";
    CloseClipboard();
}

// ---------------------------------------------------------------------------
// PNG screenshot via GDI+
// ---------------------------------------------------------------------------

static bool DoShot(const string& file) {
    if (!g_hwnd || !IsWindow(g_hwnd)) { std::cout << "no console window\n"; return false; }
    RECT rc{};
    GetWindowRect(g_hwnd, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return false;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, screen, rc.left, rc.top, SRCCOPY);
    SelectObject(mem, old);

    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartup(&token, &gsi, nullptr);
    Gdiplus::Bitmap* img = Gdiplus::Bitmap::FromHBITMAP(bmp, nullptr);
    // PNG encoder CLSID (well-known GDI+ codec GUID).
    CLSID pngClsid{0x557CF401, 0x1A04, 0x11D3,
                   {0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E}};
    const wstring wf = ToWide(file);
    HRESULT hr = img ? img->Save(wf.c_str(), &pngClsid, nullptr) : E_FAIL;
    delete img;
    Gdiplus::GdiplusShutdown(token);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    if (SUCCEEDED(hr)) { std::cout << "shot -> " << file << "\n"; return true; }
    std::cout << "shot failed\n";
    return false;
}

// ---------------------------------------------------------------------------
// Script runner
// ---------------------------------------------------------------------------

static std::vector<string> SplitArgs(const string& line) {
    std::vector<string> out;
    std::istringstream ss(line);
    string tok;
    while (ss >> tok) out.push_back(tok);
    return out;
}

static void Usage() {
    std::cout <<
        "browser_harness run <script.txt>\n"
        "commands: launch [path] | attach <pid> | attachname <exe> |\n"
        "  wait <ms> | key <spec> | text <text> | down/move/up <x> <y> [btn] |\n"
        "  wheel <x> <y> <notches> | dblclick <x> <y> | size | dump [file] |\n"
        "  shot <file.png> | clip | expect <text> | expectclip <text> | echo\n";
}

static int RunScript(const string& path) {
    std::ifstream f(path);
    if (!f) { std::cerr << "cannot open " << path << "\n"; return 2; }
    string line;
    int lineno = 0;
    while (std::getline(f, line)) {
        ++lineno;
        // strip comments
        const size_t hash = line.find('#');
        if (hash != string::npos) line.erase(hash);
        line = Trim(line);
        if (line.empty()) continue;
        const size_t sp = line.find(' ');
        const string cmd = sp == string::npos ? line : line.substr(0, sp);
        const string rest = sp == string::npos ? "" : Trim(line.substr(sp + 1));
        auto args = SplitArgs(rest);
        std::cout << "> " << line << "\n";

        if (cmd == "launch") {
            if (!LaunchBrowser(rest)) { std::cout << "launch failed\n"; ++g_failures; }
        } else if (cmd == "attach") {
            if (args.empty() || !AttachTo((DWORD)atoi(args[0].c_str())))
                { std::cout << "attach failed\n"; ++g_failures; }
        } else if (cmd == "attachname") {
            DWORD pid = args.empty() ? 0 : PidByName(ToWide(args[0]).c_str());
            if (!pid || !AttachTo(pid)) { std::cout << "attachname failed\n"; ++g_failures; }
        } else if (cmd == "wait") {
            Sleep(args.empty() ? 500 : (DWORD)atoi(args[0].c_str()));
        } else if (cmd == "key") {
            if (args.empty()) { std::cout << "key needs spec\n"; ++g_failures; }
            else DoKey(args[0]);
        } else if (cmd == "text") {
            DoText(ToWide(rest));
        } else if (cmd == "down" || cmd == "move" || cmd == "up") {
            if (args.size() < 2) { std::cout << cmd << " needs x y\n"; ++g_failures; }
            else DoMouse(cmd, atoi(args[0].c_str()), atoi(args[1].c_str()),
                         args.size() > 2 ? args[2] : "left");
        } else if (cmd == "wheel") {
            if (args.size() < 3) { std::cout << "wheel needs x y notches\n"; ++g_failures; }
            else DoMouse("wheel", atoi(args[0].c_str()), atoi(args[1].c_str()),
                         args[2]);
        } else if (cmd == "dblclick") {
            if (args.size() < 2) { std::cout << "dblclick needs x y\n"; ++g_failures; }
            else DoMouse("dblclick", atoi(args[0].c_str()), atoi(args[1].c_str()));
        } else if (cmd == "size") {
            RefreshGeometry();
            std::cout << "window: " << g_winW << "x" << g_winH
                      << " cells, buffer origin " << g_winLeft << "," << g_winTop << "\n";
        } else if (cmd == "dump") {
            DoDump(args.empty() ? "" : args[0]);
        } else if (cmd == "shot") {
            DoShot(args.empty() ? "shot.png" : args[0]);
        } else if (cmd == "clip") {
            DoClip();
        } else if (cmd == "expect") {
            if (rest.empty()) { std::cout << "expect needs text\n"; ++g_failures; }
            else DoExpect(rest, false);
        } else if (cmd == "expectnot") {
            if (rest.empty()) { std::cout << "expectnot needs text\n"; ++g_failures; }
            else DoExpect(rest, false, true);
        } else if (cmd == "expectclip") {
            if (rest.empty()) { std::cout << "expectclip needs text\n"; ++g_failures; }
            else DoExpect(rest, true);
        } else if (cmd == "expectclipnot") {
            if (rest.empty()) { std::cout << "expectclipnot needs text\n"; ++g_failures; }
            else DoExpect(rest, true, true);
        } else if (cmd == "echo") {
            std::cout << rest << "\n";
        } else {
            std::cout << "unknown command: " << cmd << "\n";
            ++g_failures;
        }
        std::cout.flush();
    }
    std::cout << "== " << g_checks << " checks, " << g_failures
              << " failures ==\n";
    return g_failures ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc < 2) { Usage(); return 2; }
    FreeConsole(); // detach from any inherited console first
    const string cmd = argv[1];
    if (cmd == "run" && argc >= 3) return RunScript(argv[2]);
    Usage();
    return 2;
}
