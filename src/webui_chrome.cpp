// webui_chrome.cpp — paints the browser chrome (tab strip, toolbar,
// status bar), hit-tests mouse input, omnibox suggestions and the
// downloads popup window.

#include "webui.h"

#include <shellapi.h>
#include <windowsx.h>

namespace webui {

namespace {

const COLORREF kBg        = RGB(24, 27, 34);
const COLORREF kBgTab     = RGB(32, 36, 45);
const COLORREF kBgTabAct  = RGB(45, 50, 62);
const COLORREF kBgTool    = RGB(28, 31, 39);
const COLORREF kBgStatus  = RGB(22, 25, 31);
const COLORREF kText      = RGB(201, 209, 217);
const COLORREF kTextDim   = RGB(125, 133, 144);
const COLORREF kAccent    = RGB(94, 234, 212);
const COLORREF kAccentDim = RGB(45, 90, 85);
const COLORREF kBorder    = RGB(48, 54, 61);

struct ChromeLayout {
    int tabH = App::kTabH;
    int toolY = App::kTabH;
    int toolH = App::kToolH;
    int statusY = App::kTabH + App::kToolH;
    int navX = 10;
    int navBtnW = 34;
    int navGap = 6;
    int navCount = 4;         // back fwd home reload
    int boxLeft = 0;
    int boxRight = 0;
    int boxTop = 0;
    int boxBottom = 0;
    int dlLeft = 0;
    int dlRight = 0;
    int dlTop = 0;
    int dlBottom = 0;
};

ChromeLayout Layout(int width) {
    ChromeLayout l;
    int x = l.navX;
    for (int i = 0; i < l.navCount; ++i) x += l.navBtnW + l.navGap;
    l.boxLeft = x + 6;
    l.dlRight = width - 10;
    l.dlLeft = l.dlRight - 58;
    l.boxRight = l.dlLeft - 10;
    if (l.boxRight - l.boxLeft < 120) {
        l.boxRight = l.boxLeft + 120;
        l.dlLeft = l.boxRight + 10;
    }
    l.boxTop = l.toolY + 6;
    l.boxBottom = l.toolY + l.toolH - 6;
    l.dlTop = l.boxTop;
    l.dlBottom = l.boxBottom;
    return l;
}

void FillRect(HDC dc, int x, int y, int w, int h, COLORREF c) {
    RECT rc{x, y, x + w, y + h};
    HBRUSH br = CreateSolidBrush(c);
    FillRect(dc, &rc, br);
    DeleteObject(br);
}

void DrawFrameRect(HDC dc, int x, int y, int w, int h, COLORREF c) {
    RECT rc{x, y, x + w, y + h};
    HBRUSH br = CreateSolidBrush(c);
    FrameRect(dc, &rc, br);
    DeleteObject(br);
}

void DrawText(HDC dc, int x, int y, const std::wstring& s, COLORREF c,
              HFONT font, int maxCells = -1) {
    if (s.empty()) return;
    std::wstring out = s;
    if (maxCells >= 0 && static_cast<int>(out.size()) > maxCells)
        out = out.substr(0, static_cast<size_t>(maxCells));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    HFONT old = static_cast<HFONT>(SelectObject(dc, font));
    TextOutW(dc, x, y, out.c_str(), static_cast<int>(out.size()));
    SelectObject(dc, old);
}

std::wstring ClipText(HDC dc, const std::wstring& s, int maxPx) {
    if (s.empty()) return s;
    SIZE sz{};
    GetTextExtentPoint32W(dc, s.c_str(), static_cast<int>(s.size()), &sz);
    if (sz.cx <= maxPx) return s;
    // Binary-ish shrink: drop chars until it fits (with ellipsis).
    size_t n = s.size();
    while (n > 4) {
        --n;
        std::wstring t = s.substr(0, n) + L"…";
        GetTextExtentPoint32W(dc, t.c_str(), static_cast<int>(t.size()), &sz);
        if (sz.cx <= maxPx) return t;
    }
    return L"…";
}

int TextW(HDC dc, HFONT font, const std::wstring& s) {
    HFONT old = static_cast<HFONT>(SelectObject(dc, font));
    SIZE sz{};
    GetTextExtentPoint32W(dc, s.c_str(), static_cast<int>(s.size()), &sz);
    SelectObject(dc, old);
    return sz.cx;
}

// ---------------------------------------------------------------------------
// Chrome painting
// ---------------------------------------------------------------------------

} // namespace

void PaintChrome(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT client;
    GetClientRect(hwnd, &client);
    const int W = client.right;

    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, W, App::kChromeH);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    HGDIOBJ oldFont = SelectObject(mem, g_app.font);

    // Background bands.
    FillRect(mem, 0, 0, W, App::kChromeH, kBg);
    FillRect(mem, 0, 0, W, App::kTabH, kBgTab);
    FillRect(mem, 0, App::kTabH, W, App::kToolH, kBgTool);
    FillRect(mem, 0, App::kTabH + App::kToolH, W, App::kStatusH, kBgStatus);

    g_app.tabHits.clear();
    const ChromeLayout lay = Layout(W);

    // ---- tab strip ----
    const size_t tabCount = g_app.tabs.size();
    int perTab = 150;
    if (tabCount > 0) {
        const int avail = W - 60;
        perTab = std::clamp(avail / static_cast<int>(tabCount), 90, 220);
    }
    int tx = 6;
    for (size_t i = 0; i < tabCount; ++i) {
        const int tw = std::min(perTab, W - tx - 44);
        if (tw < 40) break;
        const bool active = static_cast<int>(i) == g_app.active;
        Tab& t = g_app.tabs[i];
        std::wstring title = t.title;
        if (title.empty()) title = t.url.empty() ? L"Новая вкладка"
                                                : FileNameFromUrl(t.url);
        if (t.loading) title = L"… " + title;

        FillRect(mem, tx, 5, tw, App::kTabH - 10,
                 active ? kBgTabAct : kBgTab);
        DrawFrameRect(mem, tx, 5, tw, App::kTabH - 10, kBorder);
        if (active)
            FillRect(mem, tx + 1, 5, tw - 2, 3, kAccent);

        // Close "×" zone on the right of the tab.
        const int closeW = 22;
        const int titleMax = tw - 14 - closeW - 8;
        DrawText(mem, tx + 10, 5 + (App::kTabH - 10 - 15) / 2,
                 ClipText(mem, title, titleMax),
                 active ? kText : kTextDim, g_app.font, -1);
        const int cx = tx + tw - closeW - 2;
        const bool closeHover = false;
        DrawText(mem, cx, 5 + (App::kTabH - 10 - 15) / 2,
                 L"×", closeHover ? kAccent : kTextDim, g_app.font);

        g_app.tabHits.push_back({tx, 5, tx + tw - closeW - 4,
                                 App::kTabH - 5,
                                 static_cast<int>(i), false, false});
        g_app.tabHits.push_back({tx + tw - closeW - 4, 5, tx + tw - 2,
                                 App::kTabH - 5,
                                 static_cast<int>(i), true, false});
        tx += tw + 4;
    }
    // "+" new tab button.
    FillRect(mem, tx, 5, 30, App::kTabH - 10, kBgTab);
    DrawFrameRect(mem, tx, 5, 30, App::kTabH - 10, kBorder);
    DrawText(mem, tx + 8, 5 + (App::kTabH - 10 - 15) / 2, L"+",
             kAccent, g_app.fontBold);
    g_app.tabHits.push_back({tx, 5, tx + 30, App::kTabH - 5, 0, false, true});

    // ---- toolbar: nav buttons ----
    static const wchar_t* kNavGlyph[4] = {L"←", L"→", L"⌂", L"↻"};
    int nx = lay.navX;
    for (int i = 0; i < 4; ++i) {
        const int by = lay.toolY + 5;
        const int bh = lay.toolH - 10;
        FillRect(mem, nx, by, lay.navBtnW, bh, kBgTab);
        DrawFrameRect(mem, nx, by, lay.navBtnW, bh, kBorder);

        bool enabled = true;
        if (g_app.active >= 0 &&
            g_app.active < static_cast<int>(g_app.tabs.size())) {
            Tab& t = g_app.tabs[static_cast<size_t>(g_app.active)];
            if (i == 0) {
                BOOL can = FALSE;
                enabled = t.webview &&
                    SUCCEEDED(t.webview->get_CanGoBack(&can)) && can;
            } else if (i == 1) {
                BOOL can = FALSE;
                enabled = t.webview &&
                    SUCCEEDED(t.webview->get_CanGoForward(&can)) && can;
            }
        }
        DrawText(mem, nx + (lay.navBtnW - 12) / 2,
                 by + (bh - 15) / 2, kNavGlyph[i],
                 enabled ? kAccent : kTextDim, g_app.fontBold);
        g_app.navHits[i] = {nx, by, nx + lay.navBtnW, by + bh, i};
        nx += lay.navBtnW + lay.navGap;
    }

    // ---- downloads button (right side of toolbar) ----
    {
        const int by = lay.dlTop, bh = lay.dlBottom - lay.dlTop;
        const int activeDl = static_cast<int>(
            std::count_if(g_app.downloads.begin(), g_app.downloads.end(),
                          [](const Download& d) { return d.state == 0; }));
        std::wstring label = activeDl > 0
            ? L"⇩ " + std::to_wstring(activeDl) : L"⇩";
        FillRect(mem, lay.dlLeft, by, lay.dlRight - lay.dlLeft, bh, kBgTab);
        DrawFrameRect(mem, lay.dlLeft, by, lay.dlRight - lay.dlLeft, bh, kBorder);
        DrawText(mem, lay.dlLeft + 8, by + (bh - 15) / 2, label,
                 kAccent, g_app.fontBold);
        g_app.navHits[BtnDownloads] =
            {lay.dlLeft, by, lay.dlRight, by + bh, BtnDownloads};
    }

    // ---- status bar ----
    {
        const int sy = lay.statusY + (App::kStatusH - 15) / 2;
        std::wstring status = g_app.status;
        if (g_app.anyLoading && status.find(L"Загрузка") == std::wstring::npos)
            status = L"Загрузка…";
        DrawText(mem, 10, sy, ClipText(mem, status, W - 220),
                 kTextDim, g_app.font);
        // Right side: active downloads summary.
        int activeDl = 0;
        for (const auto& d : g_app.downloads)
            if (d.state == 0) ++activeDl;
        if (activeDl > 0) {
            const std::wstring dl = L"⇩ " + std::to_wstring(activeDl) +
                                    L" загрузок — Ctrl+J";
            DrawText(mem, W - TextW(mem, g_app.font, dl) - 10, sy, dl,
                     kAccent, g_app.font);
        }
        // Hint on the far right.
        DrawText(mem, W - TextW(mem, g_app.font, L"Ctrl+T новая  ·  Ctrl+J загрузки") - 10,
                 sy + 15, L"", kTextDim, g_app.font); // spacer no-op
    }

    BitBlt(dc, 0, 0, W, App::kChromeH, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldFont);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

// ---------------------------------------------------------------------------
// Mouse handling
// ---------------------------------------------------------------------------

void HandleChromeMouse(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg != WM_LBUTTONDOWN) return;
    POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    if (pt.y >= App::kChromeH) return; // page area belongs to WebView2

    // New-tab "+" and tab bodies/close zones.
    for (const auto& h : g_app.tabHits) {
        if (!h.Contains(pt.x, pt.y)) continue;
        if (h.isNewTab) { CreateTab(L"", true); return; }
        if (h.isClose) { CloseTab(h.action); return; }
        ActivateTab(h.action);
        return;
    }
    for (int i = 0; i < NavCount; ++i) {
        if (g_app.navHits[i].Contains(pt.x, pt.y)) {
            switch (i) {
            case BtnBack: NavBack(); break;
            case BtnForward: NavForward(); break;
            case BtnHome: CreateTab(L"", true); break;
            case BtnReload: ReloadActive(); break;
            case BtnDownloads: OpenDownloadsWindow(); break;
            }
            return;
        }
    }
    // Click on empty toolbar area → focus the omnibox (Firefox-like).
    RECT client{};
    GetClientRect(hwnd, &client);
    const ChromeLayout lay = Layout(client.right);
    if (pt.y >= App::kTabH && pt.y < App::kTabH + App::kToolH &&
        pt.x >= lay.boxLeft && pt.x <= lay.boxRight)
        FocusOmnibox();
}

// ---------------------------------------------------------------------------
// Omnibox suggestions
// ---------------------------------------------------------------------------

void UpdateSuggestions() {
    if (!g_app.omnibox || !g_app.suggestBox) return;
    int len = GetWindowTextLengthW(g_app.omnibox);
    std::wstring text(static_cast<size_t>(len), L'\0');
    GetWindowTextW(g_app.omnibox, text.data(), len + 1);
    text = TrimWide(text);

    if (text.size() < 2 || GetFocus() != g_app.omnibox) {
        g_app.suggestOpen = false;
        ShowWindow(g_app.suggestBox, SW_HIDE);
        return;
    }
    if (text != g_app.suggestSource) {
        g_app.suggestSource = text;
        g_app.suggestions = GetUrlSuggestions(text, 8);
        g_app.suggestSel = -1;
    }
    if (g_app.suggestions.empty()) {
        g_app.suggestOpen = false;
        ShowWindow(g_app.suggestBox, SW_HIDE);
        return;
    }

    SendMessageW(g_app.suggestBox, LB_RESETCONTENT, 0, 0);
    for (const auto& s : g_app.suggestions)
        SendMessageW(g_app.suggestBox, LB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(s.c_str()));

    // Position under the omnibox.
    RECT wr{};
    GetWindowRect(g_app.omnibox, &wr);
    POINT tl{wr.left, wr.bottom};
    MapWindowPoints(nullptr, g_app.hwnd, &tl, 1);
    const int rowH = 20;
    const int count = static_cast<int>(g_app.suggestions.size());
    const int h = std::min(count, 10) * rowH + 6;
    const int w = wr.right - wr.left;
    SetWindowPos(g_app.suggestBox, HWND_TOP, tl.x, tl.y, w, h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    g_app.suggestOpen = true;
}

void ApplySuggestion(int index) {
    if (index < 0 || index >= static_cast<int>(g_app.suggestions.size()))
        return;
    SetWindowTextW(g_app.omnibox,
                   g_app.suggestions[static_cast<size_t>(index)].c_str());
    g_app.suggestOpen = false;
    ShowWindow(g_app.suggestBox, SW_HIDE);
    OmniNavigate();
}

void PositionOmnibox() {
    if (!g_app.omnibox || !g_app.hwnd) return;
    RECT rc;
    GetClientRect(g_app.hwnd, &rc);
    const ChromeLayout lay = Layout(rc.right);
    SetWindowPos(g_app.omnibox, nullptr, lay.boxLeft, lay.boxTop,
                 lay.boxRight - lay.boxLeft, lay.boxBottom - lay.boxTop,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

// ---------------------------------------------------------------------------
// Downloads popup
// ---------------------------------------------------------------------------

LRESULT CALLBACK DownloadsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        const int note = HIWORD(wp);
        HWND list = GetDlgItem(hwnd, 1001);
        const int sel = list ? static_cast<int>(SendMessageW(
            list, LB_GETCURSEL, 0, 0)) : -1;

        if (note == BN_CLICKED || note == LBN_DBLCLK) {
            if (id == 1101 && sel >= 0 &&
                sel < static_cast<int>(g_app.downloads.size())) {
                const auto& d = g_app.downloads[static_cast<size_t>(sel)];
                if (fs::exists(d.path))
                    ShellExecuteW(nullptr, L"open", d.path.c_str(),
                                  nullptr, nullptr, SW_SHOW);
            } else if (id == 1102) {
                ShellExecuteW(nullptr, L"open",
                              DownloadsDirPath().c_str(),
                              nullptr, nullptr, SW_SHOW);
            } else if (id == 1103) {
                DestroyWindow(hwnd);
            } else if (note == LBN_DBLCLK && sel >= 0 &&
                       sel < static_cast<int>(g_app.downloads.size())) {
                const auto& d = g_app.downloads[static_cast<size_t>(sel)];
                if (fs::exists(d.path))
                    ShellExecuteW(nullptr, L"open", d.path.c_str(),
                                  nullptr, nullptr, SW_SHOW);
            }
        }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_app.dlWindow = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace webui
