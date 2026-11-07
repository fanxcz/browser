// gui_main.cpp — Win32 window, message loop and entry point for BrowserGui.

#include "gui.h"

namespace gui {

static const wchar_t* kClassName = L"BrowserGuiWindow";

static void UpdateMetrics(GuiState& state) {
    HDC hdc = GetDC(state.hwnd);
    if (!hdc) return;
    HFONT old = static_cast<HFONT>(SelectObject(hdc, state.font));
    TEXTMETRICW tm{};
    GetTextMetricsW(hdc, &tm);
    SelectObject(hdc, old);
    ReleaseDC(state.hwnd, hdc);
    state.cellW = std::max(6, static_cast<int>(tm.tmAveCharWidth));
    state.cellH = std::max(12,
        static_cast<int>(tm.tmHeight + tm.tmExternalLeading + 2));
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp,
                                LPARAM lp) {
    GuiState* state =
        reinterpret_cast<GuiState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        GuiState* st =
            reinterpret_cast<GuiState*>(cs->lpCreateParams);
        st->hwnd = hwnd;

        // Monospace terminal font: Consolas, falling back handled by GDI.
        st->font = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               FIXED_PITCH | FF_MODERN, L"Consolas");
        if (!st->font)
            st->font = static_cast<HFONT>(GetStockObject(ANSI_FIXED_FONT));
        UpdateMetrics(*st);

        // Poll async loads + caret blink + meta refresh, 10 times/sec.
        SetTimer(hwnd, 1, 100, nullptr);
        return 0;
    }
    case WM_SIZE:
        if (state) {
            UpdateMetrics(*state);
            // Recompute grid size from the new client rect.
            RECT rc;
            GetClientRect(hwnd, &rc);
            state->cols = std::max(40, static_cast<int>(rc.right) / state->cellW);
            state->rows = std::max(15, static_cast<int>(rc.bottom) / state->cellH);
        }
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    case WM_PAINT: {
        if (state) {
            RenderFrame(hwnd, *state);
        } else {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
        }
        return 0;
    }
    case WM_TIMER:
        if (wp == 1 && state) GuiTick(*state);
        return 0;
    case WM_SYSKEYDOWN: // Alt+<key> arrives here, not WM_KEYDOWN.
    case WM_KEYDOWN: {
        if (!state) break;
        bool running = true;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        GuiHandleKey(*state, wp, shift, ctrl, alt, running);
        if (!running) DestroyWindow(hwnd);
        return msg == WM_SYSKEYDOWN ? DefWindowProcW(hwnd, msg, wp, lp) : 0;
    }
    case WM_CHAR: {
        if (!state) break;
        // Ignore control characters (they arrive as WM_KEYDOWN already).
        if (wp >= 0x20 && wp != 0x7F) {
            wchar_t ch = static_cast<wchar_t>(wp);
            GuiHandleChar(*state, ch);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!state) break;
        int x = static_cast<short>(LOWORD(lp)) / state->cellW;
        int y = static_cast<short>(HIWORD(lp)) / state->cellH;
        GuiHandleMouseMove(*state, x, y);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        if (!state) break;
        int x = static_cast<short>(LOWORD(lp)) / state->cellW;
        int y = static_cast<short>(HIWORD(lp)) / state->cellH;
        GuiHandleClick(*state, x, y);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        if (!state) break;
        GuiHandleWheel(*state, GET_WHEEL_DELTA_WPARAM(wp));
        return 0;
    }
    case WM_ERASEBKGND:
        return 1; // we paint the whole client area ourselves
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int GuiMain(HINSTANCE hInstance, int nCmdShow) {
    GuiState state;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(13, 17, 23));
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) return 1;

    // Start with ~110x32 cells.
    RECT desired{0, 0, 110 * 9, 32 * 18};
    AdjustWindowRect(&desired, WS_OVERLAPPEDWINDOW, FALSE);
    const int winW = desired.right - desired.left;
    const int winH = desired.bottom - desired.top;

    HWND hwnd = CreateWindowExW(
        0, kClassName, L"Browser",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, winW, winH,
        nullptr, nullptr, hInstance, &state);
    if (!hwnd) return 1;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // Network session + history (same init as the console entry point).
    if (!InitNetworkSession()) {
        MessageBoxW(nullptr,
            L"WinHTTP не удалось инициализировать — сеть недоступна.",
            L"Browser", MB_ICONERROR);
        return 1;
    }
    const HRESULT comInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninitializeCom = SUCCEEDED(comInit);
    LoadHistory();

    // Console-UI compatible globals: statuses route to the status line.
    g_consoleUiMode = true;
    g_consolePageOpen = false;
    state.focus = Focus::Search;
    state.historyIndex = 0;

    // Restore tabs saved by the previous session.
    RestoreTabs();
    InvalidateRect(hwnd, nullptr, TRUE);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_consoleUiMode = false;
    ShutdownAsyncLoads();
    g_loadCancel.store(true);
    CancelAllDownloads();
    if (g_downloadThread.joinable())
        g_downloadThread.join();
    ShutdownNetworkSession();
    if (shouldUninitializeCom)
        CoUninitialize();
    if (state.font) DeleteObject(state.font);
    return static_cast<int>(msg.wParam);
}

} // namespace gui

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    return gui::GuiMain(hInstance, nCmdShow);
}
