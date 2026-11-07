// wvhost.cpp — minimal WebView2 (Chromium) window hosted by the console app.
// Runs on its own thread with its own message loop; the console thread
// talks to it through posted window messages. Commands issued before the
// window/controller is ready are queued and applied once it is.

#include "wvhost.h"

#include "browser.h"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include "WebView2.h"
#include "wrl/client.h"
#include "wrl/event.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <thread>

namespace wv {
namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

constexpr wchar_t kClassName[] = L"BrowserWebViewWindow";
constexpr UINT WM_WV_NAVIGATE = WM_APP + 1;  // LPARAM = std::wstring* url
constexpr UINT WM_WV_VISIBLE  = WM_APP + 2;  // WPARAM = 0 hide / 1 show
constexpr UINT WM_WV_COMMAND  = WM_APP + 3;  // WPARAM = command id
constexpr UINT WM_WV_FIND     = WM_APP + 4;  // LPARAM = std::wstring* text

enum WvCommand : WPARAM {
    WvCmdBack = 1,
    WvCmdForward,
    WvCmdReload,
    WvCmdZoomIn,
    WvCmdZoomOut,
    WvCmdZoomReset,
    WvCmdExternal,
};

HWND g_hwnd = nullptr;
ComPtr<ICoreWebView2Environment> g_env;
ComPtr<ICoreWebView2Controller> g_ctrl;
ComPtr<ICoreWebView2> g_view;

std::mutex g_mu;
std::wstring g_pendingUrl;
bool g_pendingShow = false;
int g_pendingCmd = 0;
std::wstring g_pendingFind;
std::thread g_thread;
bool g_started = false;

std::wstring ShortUrl(const std::wstring& url) {
    // host + path tail, trimmed for the status line
    std::wstring u = url;
    const size_t scheme = u.find(L"://");
    if (scheme != std::wstring::npos) u = u.substr(scheme + 3);
    const size_t slash = u.find(L'/');
    if (slash != std::wstring::npos && slash + 1 < u.size()) {
        const std::wstring host = u.substr(0, slash);
        const std::wstring path = u.substr(slash);
        if (path.size() > 40)
            return host + path.substr(0, 20) + L"…" +
                   path.substr(path.size() - 15);
        return host + path;
    }
    return u;
}

std::wstring UniqueDownloadPath(const std::wstring& name) {
    const fs::path dir = DownloadsDirPath();
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::path base = dir / name;
    if (base.filename().empty()) base = dir / L"download.bin";
    if (!fs::exists(base, ec)) return base.wstring();
    const fs::path stem = base.stem();
    const fs::path ext = base.extension();
    for (int i = 1; i < 1000; ++i) {
        fs::path candidate =
            dir / (stem.wstring() + L"-" + std::to_wstring(i) +
                   ext.wstring());
        if (!fs::exists(candidate, ec)) return candidate.wstring();
    }
    return base.wstring();
}

std::wstring JsQuote(const std::wstring& in) {
    std::wstring out;
    out.reserve(in.size() + 8);
    for (wchar_t c : in) {
        if (c == L'\\' || c == L'\'') out += L'\\';
        out += c;
    }
    return out;
}

std::wstring FileNameFromUrl(const std::wstring& url) {
    std::wstring u = url;
    const size_t q = u.find_first_of(L"?#");
    if (q != std::wstring::npos) u.resize(q);
    const size_t slash = u.find_last_of(L"/");
    std::wstring name = slash == std::wstring::npos ? u : u.substr(slash + 1);
    for (auto& c : name)
        if (c == L'<' || c == L'>' || c == L'"' || c == L'|' || c == L'?' ||
            c == L'*' || c == L':' || c == L'\\')
            c = L'_';
    if (name.empty() || name == L"." || name == L"..")
        name = L"download";
    return name;
}

// filename from Content-Disposition, falling back to the URL tail.
std::wstring DownloadFileName(const std::wstring& cdf,
                              const std::wstring& url) {
    if (!cdf.empty()) {
        const size_t f = cdf.find(L"filename*=UTF-8''");
        if (f != std::wstring::npos) {
            std::wstring enc = cdf.substr(f + 16);
            const size_t end = enc.find_first_of(L";\" \t");
            if (end != std::wstring::npos) enc.resize(end);
            // percent-decode then UTF-8 → wide
            std::string bytes;
            for (size_t i = 0; i < enc.size(); ++i) {
                if (enc[i] == L'%' && i + 2 < enc.size()) {
                    auto hv = [](wchar_t c) -> int {
                        if (c >= L'0' && c <= L'9') return c - L'0';
                        if (c >= L'a' && c <= L'f') return c - L'a' + 10;
                        if (c >= L'A' && c <= L'F') return c - L'A' + 10;
                        return -1;
                    };
                    const int hi = hv(enc[i + 1]), lo = hv(enc[i + 2]);
                    if (hi >= 0 && lo >= 0) {
                        bytes.push_back(static_cast<char>(hi * 16 + lo));
                        i += 2;
                        continue;
                    }
                }
                bytes.push_back(static_cast<char>(enc[i]));
            }
            const std::wstring decoded = ToWide(bytes);
            if (!decoded.empty()) return decoded;
        }
        const size_t f2 = cdf.find(L"filename=\"");
        if (f2 != std::wstring::npos) {
            const size_t e = cdf.find(L'"', f2 + 10);
            const std::wstring name = cdf.substr(
                f2 + 10, e == std::wstring::npos ? std::wstring::npos
                                                 : e - f2 - 10);
            if (!name.empty()) return name;
        }
    }
    return FileNameFromUrl(url);
}

void ApplyCommand(int cmd) {
    if (!g_ctrl) return;
    switch (cmd) {
    case WvCmdBack: if (g_view) g_view->GoBack(); break;
    case WvCmdForward: if (g_view) g_view->GoForward(); break;
    case WvCmdReload: if (g_view) g_view->Reload(); break;
    case WvCmdZoomIn:
    case WvCmdZoomOut:
    case WvCmdZoomReset: {
        double z = 1.0;
        g_ctrl->get_ZoomFactor(&z);
        if (cmd == WvCmdZoomIn) z *= 1.1;
        else if (cmd == WvCmdZoomOut) z /= 1.1;
        else z = 1.0;
        g_ctrl->put_ZoomFactor(std::clamp(z, 0.25, 5.0));
        break;
    }
    case WvCmdExternal: {
        if (!g_view) break;
        LPWSTR src = nullptr;
        g_view->get_Source(&src);
        const std::wstring url = src ? src : L"";
        if (src) CoTaskMemFree(src);
        if (!url.empty())
            ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr,
                          SW_SHOWNORMAL);
        break;
    }
    }
}

void FlushPending() {
    if (!g_ctrl) return;
    std::wstring url, find;
    int cmd = 0;
    bool show = false;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        url = std::move(g_pendingUrl);
        find = std::move(g_pendingFind);
        cmd = g_pendingCmd;
        g_pendingCmd = 0;
        show = g_pendingShow;
        g_pendingShow = false;
    }
    if (g_view && !url.empty())
        g_view->Navigate(url.c_str());
    if (cmd) ApplyCommand(cmd);
    if (!find.empty() && g_view) {
        const std::wstring js =
            L"window.find('" + JsQuote(find) +
            L"', false, false, true, false, true, false);";
        g_view->ExecuteScript(js.c_str(), nullptr);
    }
    if (show) {
        ShowWindow(g_hwnd, SW_SHOW);
        SetForegroundWindow(g_hwnd);
    }
}

void WireEvents() {
    if (!g_view) return;

    g_view->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [](ICoreWebView2*,
               ICoreWebView2NavigationCompletedEventArgs* args)
                -> HRESULT {
                BOOL success = FALSE;
                args->get_IsSuccess(&success);
                LPWSTR src = nullptr;
                if (g_view) g_view->get_Source(&src);
                const std::wstring url = src ? src : L"";
                if (src) CoTaskMemFree(src);
                if (success && !url.empty() &&
                    url.rfind(L"http", 0) == 0) {
                    AppendHistory(url);
                    g_consoleStatus = L"Готово — " + ShortUrl(url);
                } else if (!success) {
                    g_consoleStatus = L"Страница не загрузилась.";
                }
                return S_OK;
            }).Get(), nullptr);

    g_view->add_NewWindowRequested(
        Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [](ICoreWebView2* sender,
               ICoreWebView2NewWindowRequestedEventArgs* args)
                -> HRESULT {
                LPWSTR uri = nullptr;
                args->get_Uri(&uri);
                if (uri && *uri) {
                    sender->Navigate(uri);
                    args->put_Handled(TRUE);
                }
                if (uri) CoTaskMemFree(uri);
                return S_OK;
            }).Get(), nullptr);

    // Browser hotkeys: F5/Ctrl+R reload, Alt+←/→ history, Ctrl+=/-/0 zoom,
    // Ctrl+F find (JS prompt), Ctrl+Shift+E system browser.
    g_ctrl->add_AcceleratorKeyPressed(
        Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
            [](ICoreWebView2Controller*,
               ICoreWebView2AcceleratorKeyPressedEventArgs* args)
                -> HRESULT {
                COREWEBVIEW2_KEY_EVENT_KIND kind;
                if (FAILED(args->get_KeyEventKind(&kind)) ||
                    (kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN &&
                     kind != COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN))
                    return S_OK;
                UINT vk = 0;
                args->get_VirtualKey(&vk);
                const bool ctrl =
                    (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                const bool alt =
                    (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
                auto zoom = [&](double f) {
                    double z = 1.0;
                    g_ctrl->get_ZoomFactor(&z);
                    g_ctrl->put_ZoomFactor(
                        std::clamp(z * f, 0.25, 5.0));
                };
                auto done = [&]() -> HRESULT {
                    args->put_Handled(TRUE);
                    return S_OK;
                };
                if (vk == VK_F5 || (ctrl && (vk == 'R' || vk == 'r')))
                    return g_view ? (g_view->Reload(), done()) : S_OK;
                if (alt && vk == VK_LEFT)
                    return g_view ? (g_view->GoBack(), done()) : S_OK;
                if (alt && vk == VK_RIGHT)
                    return g_view ? (g_view->GoForward(), done()) : S_OK;
                if (ctrl && (vk == VK_OEM_PLUS || vk == VK_ADD || vk == '='))
                    return (zoom(1.1), done());
                if (ctrl && (vk == VK_OEM_MINUS || vk == VK_SUBTRACT ||
                             vk == '-'))
                    return (zoom(1.0 / 1.1), done());
                if (ctrl && vk == '0') {
                    g_ctrl->put_ZoomFactor(1.0);
                    return done();
                }
                if (ctrl && vk == 'F') {
                    g_view->ExecuteScript(
                        L"(function(){var t=prompt('Найти на странице:');"
                        L"if(t)window.find(t,false,false,true,false,"
                        L"true,false);})()", nullptr);
                    return done();
                }
                if (ctrl && GetAsyncKeyState(VK_SHIFT) & 0x8000 &&
                    (vk == 'E' || vk == 'e')) {
                    ApplyCommand(WvCmdExternal);
                    return done();
                }
                return S_OK;
            }).Get(), nullptr);

    ComPtr<ICoreWebView2_4> wv4;
    g_view.As(&wv4);
    if (!wv4) return;

    wv4->add_DownloadStarting(
        Callback<ICoreWebView2DownloadStartingEventHandler>(
            [](ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs* args)
                -> HRESULT {
            ComPtr<ICoreWebView2DownloadOperation> op;
            if (FAILED(args->get_DownloadOperation(&op)) || !op)
                return S_OK;
            LPWSTR uri = nullptr;
            op->get_Uri(&uri);
            LPWSTR cdf = nullptr;
            op->get_ContentDisposition(&cdf);
            const std::wstring url = uri ? uri : L"";
            const std::wstring name = DownloadFileName(
                cdf ? cdf : L"", url);
            if (uri) CoTaskMemFree(uri);
            if (cdf) CoTaskMemFree(cdf);
            const std::wstring path = UniqueDownloadPath(name);
            args->put_ResultFilePath(path.c_str());
            g_consoleStatus = L"Загрузка: " + name;
            return S_OK;
        }).Get(), nullptr);
}

void ThreadMain();
void Ensure();

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_WV_NAVIGATE: {
        std::unique_ptr<std::wstring> url(
            reinterpret_cast<std::wstring*>(lp));
        if (g_view) {
            g_view->Navigate(url->c_str());
        } else {
            std::lock_guard<std::mutex> lock(g_mu);
            g_pendingUrl = *url;
        }
        return 0;
    }
    case WM_WV_VISIBLE: {
        {
            std::lock_guard<std::mutex> lock(g_mu);
            g_pendingShow = (wp != 0);
        }
        if (wp) {
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
        } else {
            ShowWindow(hwnd, SW_HIDE);
        }
        return 0;
    }
    case WM_WV_COMMAND: {
        if (g_ctrl) {
            ApplyCommand(static_cast<int>(wp));
        } else {
            std::lock_guard<std::mutex> lock(g_mu);
            g_pendingCmd = static_cast<int>(wp);
        }
        return 0;
    }
    case WM_WV_FIND: {
        std::unique_ptr<std::wstring> text(
            reinterpret_cast<std::wstring*>(lp));
        if (g_view) {
            const std::wstring esc = JsQuote(text->substr(0, 200));
            const std::wstring js =
                L"window.find('" + esc +
                L"', false, false, true, false, true, false);";
            g_view->ExecuteScript(js.c_str(), nullptr);
        } else {
            std::lock_guard<std::mutex> lock(g_mu);
            g_pendingFind = *text;
        }
        return 0;
    }
    case WM_SIZE:
        if (g_ctrl) {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            g_ctrl->put_Bounds(rc);
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ThreadMain() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(
        0, kClassName, L"Browser — Web",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        1100, 750, nullptr, nullptr, wc.hInstance, nullptr);

    CreateCoreWebView2EnvironmentWithOptions(
        nullptr, nullptr, nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(hr) || !env) return S_OK;
                g_env = env;
                env->CreateCoreWebView2Controller(
                    g_hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [](HRESULT hr2, ICoreWebView2Controller* c) -> HRESULT {
                            if (FAILED(hr2) || !c) return S_OK;
                            g_ctrl = c;
                            if (FAILED(g_ctrl->get_CoreWebView2(&g_view)) ||
                                !g_view)
                                return S_OK;
                            // Local Wikipedia library at https://wiki.local/.
                            ComPtr<ICoreWebView2_3> wv3;
                            if (SUCCEEDED(g_view.As(&wv3)) && wv3) {
                                wv3->SetVirtualHostNameToFolderMapping(
                                    L"wiki.local",
                                    WikiHtmlRoot().wstring().c_str(),
                                    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                            }
                            WireEvents();
                            RECT rc{};
                            GetClientRect(g_hwnd, &rc);
                            g_ctrl->put_Bounds(rc);
                            FlushPending();
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_view.Reset();
    g_ctrl.Reset();
    g_env.Reset();
    g_hwnd = nullptr;
    CoUninitialize();
}

void Ensure() {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_started) return;
    g_started = true;
    g_thread = std::thread(ThreadMain);
}

void PostUrl(const std::wstring& url) {
    HWND hwnd = g_hwnd;
    if (!hwnd) {
        std::lock_guard<std::mutex> lock(g_mu);
        g_pendingUrl = url;
        return;
    }
    auto* u = new std::wstring(url);
    if (!PostMessageW(hwnd, WM_WV_NAVIGATE, 0,
                      reinterpret_cast<LPARAM>(u))) {
        delete u;
        std::lock_guard<std::mutex> lock(g_mu);
        g_pendingUrl = url;
    }
}

void PostCommand(int cmd) {
    HWND hwnd = g_hwnd;
    if (!hwnd || !PostMessageW(hwnd, WM_WV_COMMAND, cmd, 0)) {
        std::lock_guard<std::mutex> lock(g_mu);
        g_pendingCmd = cmd;
    }
}

} // namespace

void Show(const std::wstring& url) {
    Ensure();
    PostUrl(url);
    HWND hwnd = g_hwnd;
    if (hwnd) {
        PostMessageW(hwnd, WM_WV_VISIBLE, 1, 0);
    } else {
        std::lock_guard<std::mutex> lock(g_mu);
        g_pendingShow = true;
    }
}

void Hide() {
    Ensure();
    if (g_hwnd) PostMessageW(g_hwnd, WM_WV_VISIBLE, 0, 0);
    else {
        std::lock_guard<std::mutex> lock(g_mu);
        g_pendingShow = false;
    }
}

void Back()    { Ensure(); PostCommand(WvCmdBack); }
void Forward() { Ensure(); PostCommand(WvCmdForward); }
void Reload()  { Ensure(); PostCommand(WvCmdReload); }
void ZoomIn()  { Ensure(); PostCommand(WvCmdZoomIn); }
void ZoomOut() { Ensure(); PostCommand(WvCmdZoomOut); }
void ZoomReset(){ Ensure(); PostCommand(WvCmdZoomReset); }
void External(){ Ensure(); PostCommand(WvCmdExternal); }

void Find(const std::wstring& text) {
    Ensure();
    HWND hwnd = g_hwnd;
    if (!hwnd) {
        std::lock_guard<std::mutex> lock(g_mu);
        g_pendingFind = text;
        return;
    }
    auto* t = new std::wstring(text.substr(0, 200));
    if (!PostMessageW(hwnd, WM_WV_FIND, 0, reinterpret_cast<LPARAM>(t)))
        delete t;
}

void Shutdown() {
    if (!g_started) return;
    if (g_hwnd) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    if (g_thread.joinable()) g_thread.join();
    g_started = false;
}

} // namespace wv
