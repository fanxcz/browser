// gui_webview.cpp — embedded WebView2 (Chromium) page view for the GUI.
// The terminal chrome stays; the content area becomes a real browser
// viewport when webMode is on (web command / F9).

#include "gui.h"

#include "WebView2.h"
#include "wrl/client.h"
#include "wrl/event.h"

namespace gui {
namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

GuiState* g_state = nullptr;
HWND g_parent = nullptr;
ComPtr<ICoreWebView2Environment> g_env;
ComPtr<ICoreWebView2Controller> g_ctrl;
ComPtr<ICoreWebView2> g_view;
bool g_envRequested = false;
bool g_wantVisible = false;
std::wstring g_webUrl;

// Content area in pixels: below the chrome rows, above the status footer.
RECT ContentBounds(const GuiState& state) {
    RECT rc{};
    GetClientRect(state.hwnd, &rc);
    const int top = 6 * state.cellH;
    const int bottom = std::max(top, (state.rows - 2) * state.cellH);
    return RECT{ 0, top, rc.right, bottom };
}

void ApplyBounds() {
    if (g_ctrl && g_state)
        g_ctrl->put_Bounds(ContentBounds(*g_state));
}

void WireEvents() {
    if (!g_view) return;

    g_view->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args)
                -> HRESULT {
                LPWSTR uri = nullptr;
                args->get_Uri(&uri);
                g_webUrl = uri ? uri : L"";
                if (uri) CoTaskMemFree(uri);
                g_consoleStatus = L"Загрузка…";
                InvalidateGui();
                return S_OK;
            }).Get(), nullptr);

    g_view->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [](ICoreWebView2*,
               ICoreWebView2NavigationCompletedEventArgs* args)
                -> HRESULT {
                BOOL success = FALSE;
                args->get_IsSuccess(&success);
                if (success && !g_webUrl.empty()) {
                    AppendHistory(g_webUrl);
                    g_consoleStatus =
                        L"Готово — " + GuiShorten(g_webUrl, 60);
                } else {
                    g_consoleStatus = L"Страница не загрузилась.";
                }
                InvalidateGui();
                return S_OK;
            }).Get(), nullptr);

    g_view->add_DocumentTitleChanged(
        Callback<ICoreWebView2DocumentTitleChangedEventHandler>(
            [](ICoreWebView2*, IUnknown*) -> HRESULT {
                LPWSTR title = nullptr;
                if (g_view) g_view->get_DocumentTitle(&title);
                if (title && *title && g_parent)
                    SetWindowTextW(g_parent,
                        (L"Browser — " + std::wstring(title)).c_str());
                if (title) CoTaskMemFree(title);
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

    // Forward only omnibox-focus shortcut; the page keeps native input.
    g_ctrl->add_AcceleratorKeyPressed(
        Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
            [](ICoreWebView2Controller*,
               ICoreWebView2AcceleratorKeyPressedEventArgs* args)
                -> HRESULT {
                COREWEBVIEW2_KEY_EVENT_KIND kind =
                    COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN;
                args->get_KeyEventKind(&kind);
                UINT vk = 0;
                args->get_VirtualKey(&vk);
                const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                if (kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN ||
                    !(ctrl && vk == 'L')) {
                    args->put_Handled(FALSE);
                    return S_OK;
                }
                if (g_state) {
                    bool running = true;
                    GuiHandleKey(*g_state, vk, false, true, false, running);
                }
                args->put_Handled(TRUE);
                return S_OK;
            }).Get(), nullptr);
}

void Ensure() {
    if (g_envRequested || !g_state || !g_state->hwnd) return;
    g_envRequested = true;
    g_parent = g_state->hwnd;

    CreateCoreWebView2EnvironmentWithOptions(
        nullptr, nullptr, nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(hr) || !env || !g_state) return S_OK;
                g_env = env;
                env->CreateCoreWebView2Controller(
                    g_state->hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [](HRESULT hr2, ICoreWebView2Controller* c) -> HRESULT {
                            if (FAILED(hr2) || !c || !g_state) return S_OK;
                            g_ctrl = c;
                            if (FAILED(g_ctrl->get_CoreWebView2(&g_view)) ||
                                !g_view)
                                return S_OK;
                            WireEvents();
                            ApplyBounds();
                            g_ctrl->put_IsVisible(
                                g_state->webMode ? TRUE : FALSE);
                            if (g_state->webMode && !g_webUrl.empty())
                                g_view->Navigate(g_webUrl.c_str());
                            InvalidateGui();
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());
}

} // namespace

void GuiWebToggle(GuiState& state) {
    g_state = &state;
    state.webMode = !state.webMode;
    Ensure();
    g_wantVisible = state.webMode;
    if (g_ctrl)
        g_ctrl->put_IsVisible(state.webMode ? TRUE : FALSE);
    g_consoleStatus = state.webMode
        ? L"Режим WebView2 — настоящий рендер (F9 — обратно в текст)."
        : L"Режим чтения — текст страницы.";
    InvalidateGui();
}

void GuiWebNavigate(GuiState& state, const std::wstring& url) {
    g_state = &state;
    g_webUrl = url;
    Ensure();
    if (g_view) {
        g_view->Navigate(url.c_str());
    } else {
        g_consoleStatus = L"WebView2 запускается…";
        InvalidateGui();
    }
}

void GuiWebResize(const GuiState& state) {
    if (g_ctrl && state.hwnd == g_parent) ApplyBounds();
}

void GuiWebShutdown() {
    if (g_ctrl) {
        g_ctrl->Close();
        g_ctrl.Reset();
        g_view.Reset();
    }
    g_env.Reset();
    g_envRequested = false;
    g_parent = nullptr;
    g_state = nullptr;
}

} // namespace gui
