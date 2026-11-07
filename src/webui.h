#pragma once

// webui.h — Firefox-style GUI for Browser: WebView2 rendering engine,
// custom Win32 chrome (tabs, toolbar, omnibox, status bar, downloads).

#include "browser.h"
#include "WebView2.h"
#include "wrl/client.h"
#include "wrl/event.h"

namespace webui {

using Microsoft::WRL::ComPtr;

// One browser tab: its own WebView2 controller + page state.
struct Tab {
    std::wstring title = L"Новая вкладка";
    std::wstring url;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    bool loading = false;
    bool ready = false;   // controller created, events wired
};

// A download tracked from WebView2 DownloadStarting events.
struct Download {
    std::wstring name;
    std::wstring url;
    std::wstring path;
    uint64_t bytes = 0;
    uint64_t total = 0;
    int state = 0;        // 0 active, 1 completed, 2 cancelled/failed
};

// Screen rect of a clickable chrome element (filled during paint).
struct Hit {
    int left = 0, top = 0, right = 0, bottom = 0;
    int action = 0;       // tab index, or NavAction for toolbar buttons
    bool isClose = false;
    bool isNewTab = false;
    bool Contains(int x, int y) const {
        return x >= left && x <= right && y >= top && y <= bottom;
    }
};

enum NavBtn { BtnBack, BtnForward, BtnHome, BtnReload, BtnDownloads, NavCount };

struct App {
    HWND hwnd = nullptr;
    HWND omnibox = nullptr;      // EDIT control
    HWND suggestBox = nullptr;   // LISTBOX dropdown under omnibox
    HWND dlWindow = nullptr;     // downloads popup (created on demand)
    HFONT font = nullptr;
    HFONT fontBold = nullptr;

    std::vector<Tab> tabs;
    int active = -1;

    // Chrome metrics (client pixels).
    static constexpr int kTabH = 32;
    static constexpr int kToolH = 36;
    static constexpr int kStatusH = 22;
    static constexpr int kChromeH = kTabH + kToolH + kStatusH;

    // Painted hit areas.
    std::vector<Hit> tabHits;
    Hit navHits[NavCount];

    std::wstring status;
    bool anyLoading = false;

    // Omnibox autocomplete.
    std::vector<std::wstring> suggestions;
    std::wstring suggestSource;
    bool suggestOpen = false;
    int suggestSel = -1;

    std::vector<Download> downloads;
};

extern App g_app;

// webui_main.cpp
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow);
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
void CreateEnvironment(HWND hwnd);
void CreateTab(const std::wstring& url, bool activate);
void CloseTab(int index);
void ActivateTab(int index);
void NavigateActive(const std::wstring& url);
void OmniNavigate();
void RefreshChrome();
void UpdateTabTitle(int index);
void SetStatus(const std::wstring& text);
void UpdateTabTitle(int index);
void OpenDownloadsWindow();
void RefreshDownloadsWindow();
void ToggleBookmark();
void ShowHistoryPage();
void ShowBookmarksPage();
void SaveWebUITabs();
void RestoreWebUITabs();
std::wstring FileNameFromUrl(const std::wstring& url);
std::wstring StartPageHtml();
std::wstring HistoryPageHtml();
std::wstring BookmarksPageHtml();
std::wstring PercentEncode(const std::wstring& value);
void FocusWebView();
void FocusOmnibox();
void ReloadActive();
void NavBack();
void NavForward();
void NavHome();
void CycleTab(int delta);
void CloseActiveTabOrWindow();
void ZoomActive(double delta);

// webui_chrome.cpp
void PaintChrome(HWND hwnd);
void HandleChromeMouse(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
void UpdateSuggestions();
void ApplySuggestion(int index);
void PositionOmnibox();
LRESULT CALLBACK DownloadsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

} // namespace webui
