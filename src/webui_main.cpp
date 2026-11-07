// webui_main.cpp — window, WebView2 environment, tabs, navigation,
// shortcuts, downloads tracking and built-in pages for BrowserGui.

#include "webui.h"

#include <commctrl.h>
#include <cwchar>
#include <shellapi.h>
#include <windowsx.h>

namespace webui {

using Microsoft::WRL::Callback;

App g_app;

LRESULT CALLBACK OmniSubclassProc(HWND hwnd, UINT msg, WPARAM wp,
                                  LPARAM lp, UINT_PTR uId, DWORD_PTR ref);

namespace {

ComPtr<ICoreWebView2Environment> g_env;
const wchar_t* kClassName = L"BrowserWebUi";
const wchar_t* kDlClassName = L"BrowserWebUiDownloads";

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

std::wstring HostOf(const std::wstring& url) {
    const size_t p = url.find(L"://");
    if (p == std::wstring::npos) return url;
    std::wstring rest = url.substr(p + 3);
    const size_t slash = rest.find_first_of(L"/ ?#");
    if (slash != std::wstring::npos) rest.resize(slash);
    return rest;
}

} // namespace

std::wstring FileNameFromUrl(const std::wstring& url) {
    std::wstring name = url;
    const size_t q = name.find_first_of(L"?#");
    if (q != std::wstring::npos) name.resize(q);
    const size_t slash = name.find_last_of(L"/\\");
    if (slash != std::wstring::npos) name = name.substr(slash + 1);
    if (name.empty() || name == L".") name = L"download";
    return name;
}

namespace {

// content-disposition: attachment; filename="x.zip" / filename*=UTF-8''x
std::wstring FileNameFromDisposition(const std::wstring& cd,
                                     const std::wstring& url) {
    if (!cd.empty()) {
        const std::wstring low = cd;
        auto findVal = [&](const wchar_t* key) -> std::wstring {
            const size_t p = low.find(key);
            if (p == std::wstring::npos) return {};
            size_t v = p + wcslen(key);
            while (v < low.size() && (low[v] == L' ' || low[v] == L'=')) ++v;
            if (v < low.size() && low[v] == L'"') {
                const size_t e = low.find(L'"', v + 1);
                return low.substr(v + 1, e == std::wstring::npos
                                          ? e : e - v - 1);
            }
            const size_t e = low.find_first_of(L";", v);
            return low.substr(v, e == std::wstring::npos ? e : e - v);
        };
        std::wstring n = findVal(L"filename*=");
        if (n.empty()) n = findVal(L"filename=");
        // filename*=UTF-8''name — strip prefix and percent-decode lightly.
        const size_t tag = n.find(L"''");
        if (tag != std::wstring::npos) n = n.substr(tag + 2);
        if (!n.empty()) return n;
    }
    return FileNameFromUrl(url);
}

fs::path UniqueDownloadPath(const std::wstring& name) {
    fs::path dir = DownloadsDirPath();
    fs::path candidate = dir / name;
    if (!fs::exists(candidate)) return candidate;
    const fs::path ext = candidate.extension();
    const fs::path stem = candidate.stem();
    for (int i = 1; i < 1000; ++i) {
        candidate = dir / (stem.wstring() + L"-" +
                           std::to_wstring(i) + ext.wstring());
        if (!fs::exists(candidate)) return candidate;
    }
    return dir / (stem.wstring() + L"-" + std::to_wstring(GetTickCount64()) +
                  ext.wstring());
}

std::wstring EscapeHtml(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) {
        switch (c) {
        case L'&': out += L"&amp;"; break;
        case L'<': out += L"&lt;"; break;
        case L'>': out += L"&gt;"; break;
        case L'"': out += L"&quot;"; break;
        default: out += c;
        }
    }
    return out;
}

std::wstring NarrowHost(const std::wstring& url) {
    return HostOf(url);
}

// ---------------------------------------------------------------------------
// Chrome helpers shared with webui_chrome.cpp
// ---------------------------------------------------------------------------

RECT ContentRect() {
    RECT rc{};
    GetClientRect(g_app.hwnd, &rc);
    RECT content{0, App::kChromeH, rc.right, rc.bottom};
    if (content.bottom < content.top + 10)
        content.bottom = content.top + 10;
    return content;
}

void UpdateWebViewBounds() {
    if (g_app.active < 0 ||
        g_app.active >= static_cast<int>(g_app.tabs.size()))
        return;
    Tab& t = g_app.tabs[static_cast<size_t>(g_app.active)];
    if (!t.controller) return;
    RECT rc = ContentRect();
    t.controller->put_Bounds(rc);
}

void RecomputeLoading() {
    bool any = false;
    for (const auto& t : g_app.tabs)
        if (t.loading) any = true;
    g_app.anyLoading = any;
}

// ---------------------------------------------------------------------------
// Tab management
// ---------------------------------------------------------------------------

void WireEvents(int index) {
    Tab& t = g_app.tabs[static_cast<size_t>(index)];
    if (!t.webview) return;

    t.webview->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [index](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs*)
                -> HRESULT {
                if (index >= 0 &&
                    index < static_cast<int>(g_app.tabs.size())) {
                    g_app.tabs[static_cast<size_t>(index)].loading = true;
                    RecomputeLoading();
                    if (index == g_app.active)
                        SetStatus(L"Загрузка…");
                    RefreshChrome();
                }
                return S_OK;
            }).Get(), nullptr);

    t.webview->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [index](ICoreWebView2*,
                    ICoreWebView2NavigationCompletedEventArgs* args)
                -> HRESULT {
                if (index < 0 ||
                    index >= static_cast<int>(g_app.tabs.size()))
                    return S_OK;
                Tab& tab = g_app.tabs[static_cast<size_t>(index)];
                tab.loading = false;
                RecomputeLoading();

                BOOL success = FALSE;
                COREWEBVIEW2_WEB_ERROR_STATUS err =
                    COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                args->get_IsSuccess(&success);
                args->get_WebErrorStatus(&err);

                LPWSTR source = nullptr;
                tab.webview->get_Source(&source);
                const std::wstring url = source ? source : L"";
                if (source) CoTaskMemFree(source);
                tab.url = url;

                if (index == g_app.active) {
                    // Mirror URL into the omnibox (unless user is typing).
                    if (GetFocus() != g_app.omnibox &&
                        GetAncestor(GetFocus(), GA_ROOT) != g_app.omnibox) {
                        SetWindowTextW(g_app.omnibox, url.c_str());
                    }
                    SetStatus(success
                        ? L"Готово — " + NarrowHost(url)
                        : L"Ошибка загрузки (" + std::to_wstring(
                              static_cast<int>(err)) + L")");
                }
                if (success && !url.empty() &&
                    url.rfind(L"http", 0) == 0)
                    AppendHistory(url);
                SaveWebUITabs();
                RefreshChrome();
                return S_OK;
            }).Get(), nullptr);

    t.webview->add_DocumentTitleChanged(
        Callback<ICoreWebView2DocumentTitleChangedEventHandler>(
            [index](ICoreWebView2*, IUnknown*)
                -> HRESULT {
                if (index >= 0 &&
                    index < static_cast<int>(g_app.tabs.size())) {
                    Tab& tab = g_app.tabs[static_cast<size_t>(index)];
                    LPWSTR title = nullptr;
                    tab.webview->get_DocumentTitle(&title);
                    tab.title = title && *title ? title : L"";
                    if (title) CoTaskMemFree(title);
                    UpdateTabTitle(index);
                }
                return S_OK;
            }).Get(), nullptr);

    t.webview->add_NewWindowRequested(
        Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [index](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args)
                -> HRESULT {
                LPWSTR uri = nullptr;
                args->get_Uri(&uri);
                if (uri && *uri) {
                    CreateTab(uri, true);
                    args->put_Handled(TRUE);
                }
                if (uri) CoTaskMemFree(uri);
                return S_OK;
            }).Get(), nullptr);

    ComPtr<ICoreWebView2_4> wv4;
    t.webview.As(&wv4);
    if (wv4) wv4->add_DownloadStarting(
        Callback<ICoreWebView2DownloadStartingEventHandler>(
            [index](ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs* args)
                -> HRESULT {
                ComPtr<ICoreWebView2DownloadOperation> op;
                if (FAILED(args->get_DownloadOperation(&op)) || !op)
                    return S_OK;

                LPWSTR uri = nullptr;
                op->get_Uri(&uri);
                LPWSTR cdf = nullptr;
                op->get_ContentDisposition(&cdf);
                const std::wstring url = uri ? uri : L"";
                const std::wstring name = FileNameFromDisposition(
                    cdf ? cdf : L"", url);
                if (uri) CoTaskMemFree(uri);
                if (cdf) CoTaskMemFree(cdf);

                const fs::path path = UniqueDownloadPath(name);
                const std::wstring pathStr = path.wstring();
                args->put_ResultFilePath(pathStr.c_str());

                Download d;
                d.name = name;
                d.url = url;
                d.path = pathStr;
                g_app.downloads.push_back(d);

                // Track progress/state on the operation.
                const size_t dlIndex = g_app.downloads.size() - 1;
                op->add_StateChanged(
                    Callback<ICoreWebView2StateChangedEventHandler>(
                        [dlIndex, pathStr](ICoreWebView2DownloadOperation* o, IUnknown*)
                            -> HRESULT {
                            COREWEBVIEW2_DOWNLOAD_STATE st =
                                COREWEBVIEW2_DOWNLOAD_STATE_IN_PROGRESS;
                            o->get_State(&st);
                            INT64 bytes = 0, total = 0;
                            o->get_BytesReceived(&bytes);
                            o->get_TotalBytesToReceive(&total);
                            for (auto& d : g_app.downloads) {
                                if (d.path == pathStr) {
                                    d.bytes = static_cast<uint64_t>(bytes);
                                    d.total = static_cast<uint64_t>(total);
                                    d.state = st == COREWEBVIEW2_DOWNLOAD_STATE_COMPLETED
                                        ? 1 : st == COREWEBVIEW2_DOWNLOAD_STATE_INTERRUPTED
                                        ? 2 : 0;
                                    break;
                                }
                            }
                            SetStatus(st == COREWEBVIEW2_DOWNLOAD_STATE_COMPLETED
                                ? L"Загрузка завершена: " +
                                      g_app.downloads[dlIndex].name
                                : L"Загрузка: " +
                                      g_app.downloads[dlIndex].name);
                            RefreshChrome();
                            RefreshDownloadsWindow();
                            return S_OK;
                        }).Get(), nullptr);

                SetStatus(L"Загрузка: " + name);
                RefreshChrome();
                (void)index;
                return S_OK;
            }).Get(), nullptr);
}

void ApplyWebViewSettings(ICoreWebView2* wv) {
    if (!wv) return;
    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(wv->get_Settings(&settings)) && settings) {
        settings->put_AreDefaultScriptDialogsEnabled(TRUE);
        settings->put_IsBuiltInErrorPageEnabled(TRUE);
        ComPtr<ICoreWebView2Settings2> s2;
        if (SUCCEEDED(settings.As(&s2)) && s2) {
            s2->put_IsStatusBarEnabled(FALSE);
            s2->put_AreDefaultContextMenusEnabled(TRUE);
        }
    }
}

// Global accelerator keys while the webview has focus.
void HandleAcceleratorKey(WPARAM vk, bool ctrl, bool shift, bool alt,
                          bool& handled) {
    handled = false;
    if (!ctrl && !alt) {
        if (vk == VK_F5) { ReloadActive(); handled = true; return; }
        return;
    }
    if (alt) {
        if (vk == VK_LEFT) { NavBack(); handled = true; return; }
        if (vk == VK_RIGHT) { NavForward(); handled = true; return; }
        return;
    }
    if (!ctrl) return;
    switch (vk) {
    case 'T': CreateTab(L"", true); handled = true; break;
    case 'W': CloseActiveTabOrWindow(); handled = true; break;
    case VK_TAB: CycleTab(shift ? -1 : 1); handled = true; break;
    case 'L': FocusOmnibox(); handled = true; break;
    case 'D': ToggleBookmark(); handled = true; break;
    case 'J': OpenDownloadsWindow(); handled = true; break;
    case 'H': ShowHistoryPage(); handled = true; break;
    case 'O':
        if (shift) { ShowBookmarksPage(); handled = true; }
        break;
    case 'R': ReloadActive(); handled = true; break;
    case VK_OEM_PLUS: case VK_ADD: ZoomActive(0.1); handled = true; break;
    case VK_OEM_MINUS: case VK_SUBTRACT: ZoomActive(-0.1); handled = true; break;
    case '0': ZoomActive(0.0); handled = true; break;
    default:
        if (vk >= '1' && vk <= '9') {
            const int target = vk == '9'
                ? static_cast<int>(g_app.tabs.size()) - 1
                : vk - '1';
            if (target >= 0 && target < static_cast<int>(g_app.tabs.size())) {
                ActivateTab(target);
                handled = true;
            }
        }
        break;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void SetStatus(const std::wstring& text) {
    g_app.status = text;
    RefreshChrome();
}

void RefreshChrome() {
    if (g_app.hwnd) InvalidateRect(g_app.hwnd, nullptr, FALSE);
}

void UpdateTabTitle(int index) {
    if (index < 0 || index >= static_cast<int>(g_app.tabs.size())) return;
    const Tab& t = g_app.tabs[static_cast<size_t>(index)];
    std::wstring title = t.title;
    if (title.empty()) title = t.url.empty() ? L"Новая вкладка"
                                            : FileNameFromUrl(t.url);
    if (t.loading) title = L"… " + title;
    SetWindowTextW(g_app.hwnd,
        (title + L" — Browser").c_str());
    RefreshChrome();
}

void CreateTab(const std::wstring& url, bool activate) {
    if (!g_env) { SetStatus(L"WebView2 ещё инициализируется…"); return; }

    Tab tab;
    tab.url = url;
    g_app.tabs.push_back(std::move(tab));
    const int index = static_cast<int>(g_app.tabs.size()) - 1;

    auto handler =
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [index, url, activate](HRESULT hr,
                                   ICoreWebView2Controller* controller)
                -> HRESULT {
            if (FAILED(hr) || !controller ||
                index >= static_cast<int>(g_app.tabs.size())) {
                SetStatus(L"Не удалось создать вкладку WebView2.");
                return S_OK;
            }
            Tab& t = g_app.tabs[static_cast<size_t>(index)];
            t.controller = controller;
            t.ready = true;
            controller->get_CoreWebView2(&t.webview);
            ApplyWebViewSettings(t.webview.Get());
            WireEvents(index);

            // Accelerator keys (Ctrl+T/W/L/…, F5) while page has focus.
            ComPtr<ICoreWebView2Controller3> ctrl3;
            if (SUCCEEDED(controller->QueryInterface(
                    IID_PPV_ARGS(&ctrl3))) && ctrl3) {
                ctrl3->add_AcceleratorKeyPressed(
                    Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
                        [](ICoreWebView2Controller*,
                           ICoreWebView2AcceleratorKeyPressedEventArgs* args)
                            -> HRESULT {
                        COREWEBVIEW2_KEY_EVENT_KIND kind;
                        args->get_KeyEventKind(&kind);
                        if (kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN &&
                            kind != COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN)
                            return S_OK;
                        UINT vk = 0;
                        args->get_VirtualKey(&vk);
                        const bool ctrl =
                            (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                        const bool shift =
                            (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                        const bool alt =
                            (GetKeyState(VK_MENU) & 0x8000) != 0;
                        bool handled = false;
                        HandleAcceleratorKey(vk, ctrl, shift, alt, handled);
                        if (handled) args->put_Handled(TRUE);
                        return S_OK;
                    }).Get(), nullptr);
            }

            if (activate || g_app.active < 0)
                ActivateTab(index);
            if (!url.empty()) {
                t.webview->Navigate(url.c_str());
            } else {
                t.webview->NavigateToString(StartPageHtml().c_str());
            }
            SaveWebUITabs();
            return S_OK;
        });
    g_env->CreateCoreWebView2Controller(g_app.hwnd, handler.Get());
}

void ActivateTab(int index) {
    if (index < 0 || index >= static_cast<int>(g_app.tabs.size())) return;
    g_app.active = index;

    for (size_t i = 0; i < g_app.tabs.size(); ++i) {
        Tab& t = g_app.tabs[i];
        if (!t.controller) continue;
        const bool active = static_cast<int>(i) == index;
        t.controller->put_IsVisible(active ? TRUE : FALSE);
        if (active) {
            RECT rc = ContentRect();
            t.controller->put_Bounds(rc);
        }
    }

    Tab& t = g_app.tabs[static_cast<size_t>(index)];
    if (!t.url.empty())
        SetWindowTextW(g_app.omnibox, t.url.c_str());
    else
        SetWindowTextW(g_app.omnibox, L"");
    UpdateTabTitle(index);
    RefreshChrome();
    SaveWebUITabs();
}

void CloseTab(int index) {
    if (index < 0 || index >= static_cast<int>(g_app.tabs.size())) return;
    const bool wasActive = index == g_app.active;
    g_app.tabs.erase(g_app.tabs.begin() + index);

    if (g_app.tabs.empty()) {
        g_app.active = -1;
        PostMessageW(g_app.hwnd, WM_CLOSE, 0, 0);
        return;
    }
    if (wasActive)
        ActivateTab(std::min(index,
                             static_cast<int>(g_app.tabs.size()) - 1));
    else if (g_app.active > index)
        --g_app.active;
    SaveWebUITabs();
    RefreshChrome();
}

void CloseActiveTabOrWindow() {
    if (g_app.tabs.size() <= 1) {
        PostMessageW(g_app.hwnd, WM_CLOSE, 0, 0);
        return;
    }
    CloseTab(g_app.active);
}

void CycleTab(int delta) {
    const int n = static_cast<int>(g_app.tabs.size());
    if (n == 0) return;
    int next = g_app.active + delta;
    if (next < 0) next = n - 1;
    if (next >= n) next = 0;
    ActivateTab(next);
}

void NavigateActive(const std::wstring& url) {
    if (g_app.active < 0 ||
        g_app.active >= static_cast<int>(g_app.tabs.size()))
        return;
    Tab& t = g_app.tabs[static_cast<size_t>(g_app.active)];
    if (!t.webview) return;
    if (url.empty()) return;
    t.webview->Navigate(url.c_str());
}

void OmniNavigate() {
    std::wstring text;
    const int len = GetWindowTextLengthW(g_app.omnibox);
    text.resize(static_cast<size_t>(len));
    GetWindowTextW(g_app.omnibox, text.data(), len + 1);
    text = TrimWide(text);
    g_app.suggestOpen = false;
    ShowWindow(g_app.suggestBox, SW_HIDE);
    if (text.empty()) return;

    if (IsUrlLike(text)) {
        NavigateActive(NormalizeUrl(text));
    } else {
        // Search via DuckDuckGo HTML endpoint.
        const std::string q = ToUtf8(text);
        NavigateActive(L"https://duckduckgo.com/?q=" +
                       PercentEncode(ToWide(q)));
    }
    FocusWebView();
}

void ReloadActive() {
    if (g_app.active < 0 ||
        g_app.active >= static_cast<int>(g_app.tabs.size()))
        return;
    Tab& t = g_app.tabs[static_cast<size_t>(g_app.active)];
    if (t.webview) t.webview->Reload();
}

void NavBack() {
    Tab& t = g_app.tabs[static_cast<size_t>(g_app.active)];
    BOOL can = FALSE;
    if (t.webview && SUCCEEDED(t.webview->get_CanGoBack(&can)) && can)
        t.webview->GoBack();
}

void NavForward() {
    Tab& t = g_app.tabs[static_cast<size_t>(g_app.active)];
    BOOL can = FALSE;
    if (t.webview && SUCCEEDED(t.webview->get_CanGoForward(&can)) && can)
        t.webview->GoForward();
}

void NavHome() { CreateTab(L"", true); }

void ZoomActive(double delta) {
    if (g_app.active < 0 ||
        g_app.active >= static_cast<int>(g_app.tabs.size()))
        return;
    Tab& t = g_app.tabs[static_cast<size_t>(g_app.active)];
    if (!t.controller) return;
    double z = 1.0;
    t.controller->get_ZoomFactor(&z);
    if (delta == 0.0) z = 1.0;
    else z = std::clamp(z + delta, 0.25, 5.0);
    t.controller->put_ZoomFactor(z);
    SetStatus(L"Масштаб: " + std::to_wstring(static_cast<int>(z * 100)) +
              L"%");
}

void FocusWebView() {
    if (g_app.active < 0 ||
        g_app.active >= static_cast<int>(g_app.tabs.size()))
        return;
    Tab& t = g_app.tabs[static_cast<size_t>(g_app.active)];
    if (t.controller)
        t.controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
}

void FocusOmnibox() {
    SetFocus(g_app.omnibox);
    SendMessageW(g_app.omnibox, EM_SETSEL, 0, -1);
}

// ---------------------------------------------------------------------------
// Bookmarks / history pages
// ---------------------------------------------------------------------------

void ToggleBookmark() {
    if (g_app.active < 0 ||
        g_app.active >= static_cast<int>(g_app.tabs.size()))
        return;
    const std::wstring url = g_app.tabs[static_cast<size_t>(g_app.active)].url;
    if (url.empty() || url.rfind(L"http", 0) != 0) {
        SetStatus(L"Открыта не веб-страница — нечего сохранять.");
        return;
    }
    const auto bookmarks = LoadBookmarks();
    const bool exists = std::find(bookmarks.begin(), bookmarks.end(), url) !=
                        bookmarks.end();
    std::wofstream out(BookmarksFilePath(),
                       exists ? std::ios::app : std::ios::app);
    if (!exists) {
        out << url << L"\n";
        SetStatus(L"Добавлено в закладки: " + HostOf(url));
    } else {
        SetStatus(L"Уже в закладках: " + HostOf(url));
    }
}

void ShowHistoryPage() {
    if (g_app.active < 0 ||
        g_app.active >= static_cast<int>(g_app.tabs.size())) {
        CreateTab(L"", true);
        return;
    }
    Tab& t = g_app.tabs[static_cast<size_t>(g_app.active)];
    if (t.webview)
        t.webview->NavigateToString(HistoryPageHtml().c_str());
    SetStatus(L"История посещений");
}

void ShowBookmarksPage() {
    if (g_app.active < 0 ||
        g_app.active >= static_cast<int>(g_app.tabs.size())) {
        CreateTab(L"", true);
        return;
    }
    Tab& t = g_app.tabs[static_cast<size_t>(g_app.active)];
    if (t.webview)
        t.webview->NavigateToString(BookmarksPageHtml().c_str());
    SetStatus(L"Закладки");
}

// ---------------------------------------------------------------------------
// Tabs persistence (data/webui-tabs.txt — separate from console tabs.txt)
// ---------------------------------------------------------------------------

fs::path WebUiTabsPath() { return DataDirPath() / L"webui-tabs.txt"; }

void SaveWebUITabs() {
    std::wofstream out(WebUiTabsPath(), std::ios::trunc);
    for (const auto& t : g_app.tabs) {
        if (!t.url.empty() && t.url.rfind(L"http", 0) == 0)
            out << t.url << L"\n";
    }
}

void RestoreWebUITabs() {
    std::wifstream in(WebUiTabsPath());
    std::wstring line;
    int created = 0;
    while (std::getline(in, line) && created < 12) {
        line = TrimWide(line);
        if (line.empty()) continue;
        CreateTab(line, created == 0);
        ++created;
    }
}

// ---------------------------------------------------------------------------
// Built-in HTML pages
// ---------------------------------------------------------------------------

std::wstring PercentEncode(const std::wstring& value) {
    static const wchar_t* hex = L"0123456789ABCDEF";
    std::wstring out;
    for (unsigned char c : ToUtf8(value)) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out += static_cast<wchar_t>(c);
        } else {
            out += L'%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

namespace {
const wchar_t* kPageStyle = LR"CSS(
<style>
  body { background:#1c1f26; color:#c9d1d9; font-family:'Segoe UI',sans-serif;
         margin:0; padding:48px 24px; }
  .wrap { max-width:720px; margin:0 auto; }
  h1 { color:#5eead4; font-weight:600; letter-spacing:2px; }
  .sub { color:#8b949e; margin-bottom:32px; }
  a { color:#79c0ff; text-decoration:none; display:block; padding:8px 10px;
      border-radius:6px; }
  a:hover { background:#21262d; }
  .u { color:#8b949e; font-size:12px; display:block; padding:0 10px 4px; }
  .empty { color:#8b949e; font-style:italic; }
  .bar { border-top:1px solid #30363d; margin:28px 0; }
</style>)CSS";
}

std::wstring StartPageHtml() {
    std::wstring body;
    body += LR"HTML(<div class="wrap">
<h1>BROWSER</h1>
<div class="sub">Введите запрос или адрес в строке выше. Ctrl+T — новая вкладка,
Ctrl+J — загрузки, Ctrl+H — история, Ctrl+D — в закладки.</div>)HTML";

    const auto bookmarks = LoadBookmarks();
    if (!bookmarks.empty()) {
        body += LR"HTML(<div class="bar"></div><b>ЗАКЛАДКИ</b>)HTML";
        for (const auto& b : bookmarks) {
            if (b.rfind(L"http", 0) != 0) continue;
            body += L"<a href=\"" + EscapeHtml(b) + L"\">" +
                    EscapeHtml(HostOf(b)) + L"</a>";
            body += L"<span class=\"u\">" + EscapeHtml(b) + L"</span>";
        }
    }
    body += LR"HTML(</div>)HTML";
    return std::wstring(L"<!doctype html><html><head><meta charset='utf-8'>") +
           kPageStyle + L"</head><body>" + body + L"</body></html>";
}

std::wstring HistoryPageHtml() {
    auto entries = ReadHistoryEntries(HistoryFilePath());
    std::reverse(entries.begin(), entries.end());
    std::wstring body;
    body += LR"HTML(<div class="wrap">
<h1>ИСТОРИЯ</h1>
<div class="sub">Последние посещённые страницы.</div>)HTML";
    std::wstring seen;
    int shown = 0;
    for (const auto& e : entries) {
        if (e.url.rfind(L"http", 0) != 0) continue;
        if (seen.find(e.url) != std::wstring::npos) continue;
        seen += e.url + L"\n";
        body += L"<a href=\"" + EscapeHtml(e.url) + L"\">" +
                EscapeHtml(HostOf(e.url) + L"/" + FileNameFromUrl(e.url)) +
                L"</a>";
        body += L"<span class=\"u\">" + EscapeHtml(e.url) + L"</span>";
        if (++shown >= 200) break;
    }
    if (shown == 0)
        body += LR"HTML(<div class="empty">История пуста.</div>)HTML";
    body += LR"HTML(</div>)HTML";
    return std::wstring(L"<!doctype html><html><head><meta charset='utf-8'>") +
           kPageStyle + L"</head><body>" + body + L"</body></html>";
}

std::wstring BookmarksPageHtml() {
    const auto bookmarks = LoadBookmarks();
    std::wstring body;
    body += LR"HTML(<div class="wrap">
<h1>ЗАКЛАДКИ</h1>
<div class="sub">Ctrl+D на любой странице — добавить в закладки.</div>)HTML";
    int shown = 0;
    for (const auto& b : bookmarks) {
        if (b.rfind(L"http", 0) != 0) continue;
        body += L"<a href=\"" + EscapeHtml(b) + L"\">" +
                EscapeHtml(HostOf(b)) + L"</a>";
        body += L"<span class=\"u\">" + EscapeHtml(b) + L"</span>";
        ++shown;
    }
    if (shown == 0)
        body += LR"HTML(<div class="empty">Закладок нет.</div>)HTML";
    body += LR"HTML(</div>)HTML";
    return std::wstring(L"<!doctype html><html><head><meta charset='utf-8'>") +
           kPageStyle + L"</head><body>" + body + L"</body></html>";
}

// ---------------------------------------------------------------------------
// Downloads window
// ---------------------------------------------------------------------------

void RefreshDownloadsWindow() {
    if (!g_app.dlWindow) return;
    HWND list = GetDlgItem(g_app.dlWindow, 1001);
    if (!list) return;
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    for (const auto& d : g_app.downloads) {
        std::wstring text = d.name;
        if (d.state == 1) text += L"  ✓";
        else if (d.state == 2) text += L"  ✗";
        else if (d.total > 0)
            text += L"  " + std::to_wstring(d.bytes * 100 / d.total) + L"%";
        SendMessageW(list, LB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(text.c_str()));
    }
}

void OpenDownloadsWindow() {
    if (g_app.dlWindow) {
        ShowWindow(g_app.dlWindow, SW_SHOW);
        SetForegroundWindow(g_app.dlWindow);
        RefreshDownloadsWindow();
        return;
    }

    const int w = 460, h = 320;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    g_app.dlWindow = CreateWindowExW(
        WS_EX_TOOLWINDOW, kDlClassName, L"Загрузки — Browser",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
        x, y, w, h, g_app.hwnd, nullptr,
        reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(
            g_app.hwnd, GWLP_HINSTANCE)), nullptr);
    if (!g_app.dlWindow) return;

    HWND list = CreateWindowExW(0, L"LISTBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY,
        12, 12, w - 24, h - 76, g_app.dlWindow,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(1001)), nullptr, nullptr);
    SendMessageW(list, WM_SETFONT,
                 reinterpret_cast<WPARAM>(g_app.font), TRUE);

    const wchar_t* labels[] = {L"Открыть файл", L"Папка загрузок",
                               L"Закрыть"};
    const int ids[] = {1101, 1102, 1103};
    for (int i = 0; i < 3; ++i) {
        HWND b = CreateWindowExW(0, L"BUTTON", labels[i],
            WS_CHILD | WS_VISIBLE,
            12 + i * 150, h - 52, 140, 28, g_app.dlWindow,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ids[i])),
            reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(
                g_app.dlWindow, GWLP_HINSTANCE)), nullptr);
        SendMessageW(b, WM_SETFONT,
                     reinterpret_cast<WPARAM>(g_app.font), TRUE);
    }

    RefreshDownloadsWindow();
    ShowWindow(g_app.dlWindow, SW_SHOW);
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_app.hwnd = hwnd;
        g_app.font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        g_app.fontBold = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE,
            FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

        g_app.omnibox = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            0, 0, 100, 24, hwnd, nullptr,
            reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE)),
            nullptr);
        SendMessageW(g_app.omnibox, WM_SETFONT,
                     reinterpret_cast<WPARAM>(g_app.font), TRUE);
        SendMessageW(g_app.omnibox, EM_SETCUEBANNER, TRUE,
                     reinterpret_cast<LPARAM>(
                         L"Поиск или адрес сайта"));
        SetWindowSubclass(g_app.omnibox, OmniSubclassProc, 1,
                          reinterpret_cast<DWORD_PTR>(hwnd));

        g_app.suggestBox = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"LISTBOX", nullptr,
            WS_CHILD | WS_BORDER | WS_VSCROLL | LBS_NOTIFY,
            0, 0, 100, 100, hwnd, nullptr,
            reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE)),
            nullptr);
        SendMessageW(g_app.suggestBox, WM_SETFONT,
                     reinterpret_cast<WPARAM>(g_app.font), TRUE);

        SetTimer(hwnd, 1, 250, nullptr); // chrome repaint / status refresh
        CreateEnvironment(hwnd);
        return 0;
    }
    case WM_SIZE:
        PositionOmnibox();
        UpdateWebViewBounds();
        RefreshChrome();
        return 0;
    case WM_PAINT:
        PaintChrome(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        if (wp == 1) {
            // Repaint chrome occasionally (tab titles, loading state).
            InvalidateRect(hwnd, nullptr, FALSE);
            // Refresh omnibox nav-button enabled states cheaply.
            RecomputeLoading();
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (GET_WHEEL_DELTA_WPARAM(wp) != 0) {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            if (pt.y < App::kTabH) {
                CycleTab(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -1 : 1);
                return 0;
            }
        }
        break;
    case WM_LBUTTONDOWN:
    case WM_MOUSEMOVE:
    case WM_LBUTTONUP:
        HandleChromeMouse(hwnd, msg, wp, lp);
        return 0;
    case WM_COMMAND:
        if (HIWORD(wp) == EN_CHANGE &&
            reinterpret_cast<HWND>(lp) == g_app.omnibox) {
            UpdateSuggestions();
            return 0;
        }
        if (HIWORD(wp) == LBN_SELCHANGE &&
            reinterpret_cast<HWND>(lp) == g_app.suggestBox) {
            const LRESULT sel = SendMessageW(g_app.suggestBox, LB_GETCURSEL, 0, 0);
            g_app.suggestSel = sel == LB_ERR ? -1 : static_cast<int>(sel);
            return 0;
        }
        if (HIWORD(wp) == LBN_DBLCLK &&
            reinterpret_cast<HWND>(lp) == g_app.suggestBox) {
            const LRESULT sel = SendMessageW(g_app.suggestBox, LB_GETCURSEL, 0, 0);
            if (sel != LB_ERR) ApplySuggestion(static_cast<int>(sel));
            return 0;
        }
        break;
    case WM_KEYDOWN: {
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        bool handled = false;
        HandleAcceleratorKey(wp, ctrl, shift, alt, handled);
        if (handled) return 0;
        if (wp == VK_ESCAPE && g_app.suggestOpen) {
            g_app.suggestOpen = false;
            ShowWindow(g_app.suggestBox, SW_HIDE);
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        SaveWebUITabs();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Omnibox subclass: Enter navigates, Esc returns focus to the page,
// Up/Down moves through the suggestion list.
LRESULT CALLBACK OmniSubclassProc(HWND hwnd, UINT msg, WPARAM wp,
                                  LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    if (msg == WM_KEYDOWN) {
        if (wp == VK_RETURN) {
            const int sel = SendMessageW(g_app.suggestBox, LB_GETCURSEL, 0, 0);
            if (g_app.suggestOpen && sel != LB_ERR &&
                sel < static_cast<int>(g_app.suggestions.size())) {
                ApplySuggestion(static_cast<int>(sel));
            } else {
                OmniNavigate();
            }
            return 0;
        }
        if (wp == VK_ESCAPE) {
            if (g_app.suggestOpen) {
                g_app.suggestOpen = false;
                ShowWindow(g_app.suggestBox, SW_HIDE);
            } else {
                FocusWebView();
            }
            return 0;
        }
        if ((wp == VK_UP || wp == VK_DOWN) && g_app.suggestOpen) {
            const int count = static_cast<int>(g_app.suggestions.size());
            if (count > 0) {
                int sel = SendMessageW(g_app.suggestBox, LB_GETCURSEL, 0, 0);
                sel += wp == VK_DOWN ? 1 : -1;
                sel = std::clamp(sel, 0, count - 1);
                SendMessageW(g_app.suggestBox, LB_SETCURSEL, sel, 0);
                g_app.suggestSel = sel;
                return 0;
            }
        }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Environment + entry point
// ---------------------------------------------------------------------------

void CreateEnvironment(HWND hwnd) {
    const fs::path ud = DataDirPath() / L"webview2";
    std::error_code ec;
    fs::create_directories(ud, ec);

    auto handler =
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED(hr) || !env) {
                SetStatus(L"WebView2 недоступен — обновите Windows.");
                return S_OK;
            }
            g_env = env;
            RestoreWebUITabs();
            if (g_app.tabs.empty())
                CreateTab(L"", true);
            if (g_app.active >= 0)
                ActivateTab(g_app.active);
            return S_OK;
        });
    CreateCoreWebView2EnvironmentWithOptions(
        nullptr, ud.c_str(), nullptr, handler.Get());
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    const HRESULT comInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(comInit)) return 1;
    InitCommonControls();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(24, 27, 34));
    wc.lpszClassName = kClassName;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    WNDCLASSEXW dlc{};
    dlc.cbSize = sizeof(dlc);
    dlc.style = CS_HREDRAW | CS_VREDRAW;
    dlc.lpfnWndProc = DownloadsProc;
    dlc.hInstance = hInstance;
    dlc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    dlc.hbrBackground = CreateSolidBrush(RGB(24, 27, 34));
    dlc.lpszClassName = kDlClassName;
    dlc.hIcon = wc.hIcon;
    RegisterClassExW(&dlc);

    RECT rc{0, 0, 1280, 800};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, FALSE);
    HWND hwnd = CreateWindowExW(
        0, kClassName, L"Browser",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) { CoUninitialize(); return 1; }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_app.font) DeleteObject(g_app.font);
    if (g_app.fontBold && g_app.fontBold != g_app.font)
        DeleteObject(g_app.fontBold);
    g_app.tabs.clear();
    g_env.Reset();
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}

} // namespace webui

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrev,
                    PWSTR lpCmdLine, int nCmdShow) {
    return webui::wWinMain(hInstance, hPrev, lpCmdLine, nCmdShow);
}
