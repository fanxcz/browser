#include "ui_int.h"

namespace console_ui {
    BOOL WINAPI ConsoleCtrlHandler(DWORD type) {
        return type == CTRL_C_EVENT ? TRUE : FALSE;
    }

    bool Run() {
        State state;
        if (state.output == INVALID_HANDLE_VALUE || state.inputHandle == INVALID_HANDLE_VALUE)
            return false;
        DWORD outputMode = 0;
        DWORD oldMode = 0;
        if (!GetConsoleMode(state.output, &outputMode) ||
            !GetConsoleMode(state.inputHandle, &oldMode) ||
            !GetConsoleScreenBufferInfo(state.output, &state.info))
            return false;
        // ENABLE_PROCESSED_INPUT stays OFF: with it on, Ctrl+C is eaten
        // as a control event and never reaches the key loop as a key
        // press, so copy could never fire. Now Ctrl+C arrives as a
        // normal KEY_EVENT (and ConsoleCtrlHandler is just a safety net).
        const DWORD newMode = (oldMode | ENABLE_EXTENDED_FLAGS | ENABLE_MOUSE_INPUT |
                               ENABLE_WINDOW_INPUT) &
                              ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT |
                                ENABLE_QUICK_EDIT_MODE | ENABLE_PROCESSED_INPUT);
        if (!SetConsoleMode(state.inputHandle, newMode)) return false;
        SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

        g_consoleUiMode = true;
        // Restore omnibox/terminal command history from the last session.
        if (!g_safeMode) {
            state.inputHistory = LoadInputHistory();
            if (state.inputHistory.size() > 80)
                state.inputHistory.resize(80);
            state.historyIndex = state.inputHistory.size();
        }
        LoadThemePacks();
        LoadThemeSetting();
        // Persisted display/network toggles from settings.txt.
        UiApplyOfflineSetting();
        UiApplyImagesSetting();
        UiApplyStatusBarSetting();
        UiApplyPrefetchSetting();
        UiApplyZoomSetting();
        if (!g_safeMode) {
            // Maintenance: once-a-day data backup + cache size cap.
            std::thread([] {
                try { MaybeDailyBackup(); EvictCacheOverflow(); }
                catch (...) {}
            }).detach();
            UiScheduleUpdateCheck();
        }
        state.focus = Focus::Search;
        state.historyIndex = 0;
        bool running = true;
        // Restore tabs saved in the previous session (data/tabs.txt).
        // --safe-mode starts with a single fresh tab instead.
        if (!g_safeMode) RestoreTabs();
        // browser:// protocol / .html file association handoff.
        if (!g_pendingLaunchUrl.empty()) {
            const std::wstring u = g_pendingLaunchUrl;
            g_pendingLaunchUrl.clear();
            if (IsInternalPageUrl(u))
                UiOpenInternalPage(u.substr(std::wstring(kInternalPrefix).size()));
            else
                UiOpenUrl(u);
        }
        if (g_safeMode)
            g_consoleStatus = L"Безопасный режим — вкладки и история не "
                              L"восстанавливались.";
        std::wstring lastUrl = g_currentUrl;
        Render(state);

        auto lastAnim = Clock::now();
        size_t dlPendingPrev = SIZE_MAX; // download-finish beep tracker
        while (running) {
            // Commit any finished background load and pick up queued jobs.
            const bool loadChanged = PollAsyncLoad();
            // Beep once when the last queued/running download finishes.
            const size_t dlPend = PendingDownloadCount();
            if (dlPendingPrev != SIZE_MAX && dlPendingPrev > 0 &&
                dlPend == 0)
                Beep(1400, 90);
            dlPendingPrev = dlPend;
            // One-shot notice from the background update check.
            const std::wstring upd = UiTakeUpdateNotice();
            if (!upd.empty()) g_consoleStatus = upd;
            // Periodic re-render animates the "loading" indicator.
            bool animTick = false;
            if (g_loadRunning.load() &&
                Clock::now() - lastAnim > std::chrono::milliseconds(400)) {
                lastAnim = Clock::now();
                animTick = true;
            }
            // Deferred single click: fire ~250 ms after release unless a
            // second press promoted it to word/line selection.
            if (state.pendingClickValid &&
                Clock::now() - state.pendingClickTime >
                    std::chrono::milliseconds(250)) {
                DispatchPendingClick(state);
                animTick = true;
            }
            // If a new page arrived, reset scroll/selection state.
            if (g_currentUrl != lastUrl) {
                lastUrl = g_currentUrl;
                ClearSelection(state);
                state.selectedItem = -1;
                state.hoveredItem = -1;
                state.scrollOffset = 0;
                state.findMode = false;
                state.findPos = 0;
                state.findQuery.clear();
                state.editingField = -1;
                // Arm the meta-refresh timer for the new page.
                state.refreshScanUrl = g_currentUrl;
                state.refreshSeconds = g_refreshSeconds;
                state.refreshInterval = 0;   // manual interval dies on nav
                state.refreshUrl = g_refreshUrl;
                state.refreshAt = Clock::now() +
                    std::chrono::seconds(std::max(1, g_refreshSeconds));
                // Per-site theme override (data/sites/<host>.theme, rules).
                ApplySiteTheme(g_currentUrl);
                // Browser-like: the console window title shows the page title.
                const std::wstring title = g_currentTitle.empty()
                    ? g_currentUrl
                    : L"Browser — " + ToWide(g_currentTitle);
                SetConsoleTitleW(title.c_str());
            }

            // Meta refresh: reload/navigate when the timer expires.
            if (state.refreshSeconds > 0 && Clock::now() >= state.refreshAt &&
                g_consolePageOpen && !g_loadRunning.load()) {
                if (state.refreshInterval > 0) {
                    // Manual interval: re-arm and reload the same page.
                    state.refreshAt = Clock::now() +
                        std::chrono::seconds(state.refreshInterval);
                    UiReload();
                } else {
                    state.refreshSeconds = 0;
                    if (state.refreshUrl.empty()) {
                        UiReload();
                    } else {
                        const std::wstring target =
                            ResolveRedirectUrl(g_currentUrl,
                                               ToWide(state.refreshUrl));
                        if (!target.empty()) UiOpenUrl(target);
                    }
                }
                continue;
            }

            DWORD waitResult = WaitForSingleObject(state.inputHandle, 120);
            bool redraw = loadChanged || animTick;
            if (waitResult == WAIT_OBJECT_0) {
                INPUT_RECORD record{};
                DWORD read = 0;
                if (ReadConsoleInputW(state.inputHandle, &record, 1, &read) && read != 0) {
                    switch (record.EventType) {
                    case KEY_EVENT:
                        if (!record.Event.KeyEvent.bKeyDown) redraw = redraw;
                        else HandleKey(state, record.Event.KeyEvent, running);
                        redraw = true;
                        break;
                    case MOUSE_EVENT: {
                        const MOUSE_EVENT_RECORD& mouse = record.Event.MouseEvent;
                        if (mouse.dwEventFlags == MOUSE_MOVED) {
                            const bool wasDragging = state.mouseSelDown;
                            const int previousHover = state.hoveredItem;
                            const int previousAction = state.hoveredAction;
                            const int previousButton = state.hoveredSearchButton;
                            const int previousSuggest = state.hoverSuggest;
                            const int previousMenu = state.settingsHover;
                            HandleMouse(state, mouse);
                            redraw = redraw || wasDragging ||
                                     previousHover != state.hoveredItem ||
                                     previousAction != state.hoveredAction ||
                                     previousButton != state.hoveredSearchButton ||
                                     previousSuggest != state.hoverSuggest ||
                                     previousMenu != state.settingsHover;
                        } else if (mouse.dwButtonState == 0 &&
                                   mouse.dwEventFlags != DOUBLE_CLICK) {
                            // Button release — only interesting mid-drag.
                            if (state.mouseSelDown) {
                                HandleMouse(state, mouse);
                                redraw = true;
                            }
                        } else {
                            HandleMouse(state, mouse);
                            redraw = true;
                        }
                        break;
                    }
                    case WINDOW_BUFFER_SIZE_EVENT:
                        RefreshSize(state);
                        redraw = true;
                        break;
                    default:
                        break;
                    }
                }
            } else if (waitResult == WAIT_FAILED) {
                break;
            }

            if (running && redraw) Render(state);
        }
        g_consoleUiMode = false;
        SetConsoleCtrlHandler(NULL, TRUE);
        SetConsoleMode(state.inputHandle, oldMode);
        SetConsoleTextAttribute(state.output, kText);
        CONSOLE_CURSOR_INFO cursorInfo{20, TRUE};
        SetConsoleCursorInfo(state.output, &cursorInfo);
        return true;
    }
}
