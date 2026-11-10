#include "ui_int.h"

namespace console_ui {
    void HandleMouse(State& state, const MOUSE_EVENT_RECORD& mouse) {
        if (mouse.dwEventFlags == MOUSE_WHEELED) {
            const SHORT delta = static_cast<SHORT>(HIWORD(mouse.dwButtonState));
            // Wheel scrolls the settings dropdown while it is open.
            if (state.settingsOpen) {
                const int maxRows = std::max(4, state.height - 7);
                const int maxScroll = std::max(0,
                    static_cast<int>(SettingsMenuItems().size()) - maxRows);
                state.settingsScroll = std::clamp(state.settingsScroll +
                    (delta > 0 ? -1 : 1), 0, maxScroll);
                return;
            }
            const size_t notches = static_cast<size_t>(std::max(1, std::abs(static_cast<int>(delta)) / WHEEL_DELTA));
            // Density scales the base step; `scroll <n>` overrides it.
            const size_t base = g_scrollLines > 0 ? static_cast<size_t>(g_scrollLines)
                : static_cast<size_t>(g_density == 0 ? 5 : g_density == 2 ? 2 : 3);
            const size_t step = notches * base;
            const size_t maxOffset = state.pageLineCount > static_cast<size_t>(state.pageCapacity)
                ? state.pageLineCount - static_cast<size_t>(state.pageCapacity) : 0;
            if (delta > 0)
                state.scrollOffset = state.scrollOffset > step ? state.scrollOffset - step : 0;
            else if (delta < 0)
                state.scrollOffset = std::min(maxOffset, state.scrollOffset + step);
            state.selectedItem = -1;
            state.hoveredItem = -1;
            state.focus = Focus::Page;
            return;
        }
        if (mouse.dwEventFlags == MOUSE_HWHEELED) return;

        // Side mouse buttons (XBUTTON1/XBUTTON2): the console exposes them
        // as extra button bits — 4th-from-left and 3rd-from-left.
        // X1 → back, X2 → forward (flip here if your mouse reports them
        // swapped).
        if (mouse.dwEventFlags == 0 &&
            (mouse.dwButtonState == FROM_LEFT_4TH_BUTTON_PRESSED ||
             mouse.dwButtonState == FROM_LEFT_3RD_BUTTON_PRESSED)) {
            ActivateAction(state,
                mouse.dwButtonState == FROM_LEFT_4TH_BUTTON_PRESSED
                    ? NavAction::Back : NavAction::Forward);
            return;
        }

        // Any click dismisses the image viewer; wheel is ignored.
        if (state.viewingImage) {
            if (mouse.dwEventFlags == 0 &&
                (mouse.dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0)
                CloseImageViewer(state);
            return;
        }

        const int x = mouse.dwMousePosition.X - state.left;
        const int y = mouse.dwMousePosition.Y - state.top;

        // Middle-click: on a tab closes it; on a link/image opens it in a
        // new tab (like browsers).
        if (mouse.dwEventFlags == 0 &&
            (mouse.dwButtonState & FROM_LEFT_2ND_BUTTON_PRESSED) != 0) {
            if (y == 0) {
                for (const auto& hit : state.tabHits) {
                    if (!hit.isNew && !hit.isClose &&
                        x >= hit.left && x <= hit.right) {
                        ApplyView(state, UiCloseTabIndex(hit.index,
                                                        CurrentView(state)));
                        return;
                    }
                }
                return;
            }
            for (const auto& row : state.itemRows) {
                if (y == row.row) {
                    std::wstring target;
                    if (static_cast<size_t>(row.item) < g_currentLinks.size())
                        target = g_currentLinks[
                            static_cast<size_t>(row.item)].url;
                    else {
                        const int img = ImageIndexForItem(state, row.item);
                        if (img >= 0)
                            target = g_currentImages[
                                static_cast<size_t>(img)].url;
                    }
                    if (!target.empty() &&
                        target.rfind(L"openfile://", 0) != 0)
                        ApplyView(state, UiOpenInNewTab(target,
                                                        CurrentView(state)));
                    return;
                }
            }
            return;
        }

        // DOUBLE_CLICK is accepted too: click-vs-drag is decided on
        // release, and the second press of a double-click must arm the
        // click counter for word/line selection.
        const bool isClick = (mouse.dwEventFlags == 0 ||
            mouse.dwEventFlags == DOUBLE_CLICK) &&
            (mouse.dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0;

        if (mouse.dwEventFlags == MOUSE_MOVED) {
            // Drag in progress: track the end cell; a move without the
            // button means the release happened outside the window.
            if (state.mouseSelDown) {
                if ((mouse.dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0) {
                    state.selEX = x;
                    state.selEY = y;
                    // Auto-scroll while dragging near the content edges —
                    // like a terminal extending the selection off-screen.
                    if (g_consolePageOpen && state.pageCapacity > 0) {
                        const int contentTop = 6;
                        const int cap = state.pageCapacity;
                        const size_t maxOff =
                            state.pageLineCount > static_cast<size_t>(cap)
                                ? state.pageLineCount - static_cast<size_t>(cap)
                                : 0;
                        if (y <= contentTop && state.scrollOffset > 0)
                            --state.scrollOffset;
                        else if (y >= contentTop + cap - 1 &&
                                 state.scrollOffset < maxOff)
                            ++state.scrollOffset;
                    }
                } else {
                    FinishScreenSel(state, x, y);
                }
                return;
            }
            int hoverItem = -1;
            for (const auto& row : state.itemRows)
                if (y == row.row) { hoverItem = row.item; break; }
            int hoverAction = -1;
            for (size_t i = 0; i < state.actionHits.size(); ++i) {
                const auto& hit = state.actionHits[i];
                if (y == hit.row && x >= hit.left && x <= hit.right) {
                    hoverAction = static_cast<int>(i); break;
                }
            }
            int hoverButton = 0;
            if (state.searchButtonLeft >= 0 && y == state.searchRow &&
                x >= state.searchButtonLeft && x <= state.searchButtonRight)
                hoverButton = 1;
            int hoverSuggest = -1;
            for (const auto& hit : state.suggestHits)
                if (y == hit.row && x >= hit.left && x <= hit.right) {
                    hoverSuggest = hit.index; break;
                }
            if (hoverSuggest == -1)
                for (const auto& hit : state.bookmarkHits)
                    if (y == hit.row && x >= hit.left && x <= hit.right) {
                        hoverSuggest = hit.index; break;
                    }
            int hoverMenu = -1;
            for (const auto& hit : state.settingsHits)
                if (y == hit.row && x >= hit.left && x <= hit.right) {
                    hoverMenu = hit.index; break;
                }
            if (hoverItem != state.hoveredItem || hoverAction != state.hoveredAction ||
                hoverButton != state.hoveredSearchButton ||
                hoverSuggest != state.hoverSuggest ||
                hoverMenu != state.settingsHover) {
                state.hoveredItem = hoverItem;
                state.hoveredAction = hoverAction;
                state.hoveredSearchButton = hoverButton;
                state.hoverSuggest = hoverSuggest;
                state.settingsHover = hoverMenu;
                if (hoverSuggest >= 0) state.suggestIndex = hoverSuggest;
            }
            // Hover prefetch: warm the cache for the link under the cursor
            // (browser-style — makes the click feel instant).
            if (hoverItem >= 0 &&
                static_cast<size_t>(hoverItem) < g_currentLinks.size()) {
                const std::wstring u =
                    g_currentLinks[static_cast<size_t>(hoverItem)].url;
                if (!u.empty()) UiPrefetchUrl(u);
            }
            return;
        }
        // Button release: decide click vs drag-selection.
        if (state.mouseSelDown &&
            (mouse.dwButtonState & (FROM_LEFT_1ST_BUTTON_PRESSED |
                                    FROM_LEFT_2ND_BUTTON_PRESSED)) == 0) {
            FinishScreenSel(state, x, y);
            return;
        }
        if (!isClick) return;

        // A press somewhere else (or long after) flushes a pending click
        // immediately so fast click-click sequences don't lose the first.
        if (state.pendingClickValid &&
            (std::abs(x - state.pendingClickX) > 1 ||
             std::abs(y - state.pendingClickY) > 1 ||
             Clock::now() - state.pendingClickTime >
                 std::chrono::milliseconds(500)))
            DispatchPendingClick(state);

        // Multi-click streak: a press within 500 ms of the previous one
        // at ~the same cell counts up. 2nd = word, 3rd+ = line. The
        // press is consumed (no drag) and the deferred single click from
        // the previous release is cancelled.
        {
            const auto now = Clock::now();
            const bool sameSpot = state.lastClickX >= 0 &&
                std::abs(x - state.lastClickX) <= 1 &&
                std::abs(y - state.lastClickY) <= 1 &&
                now - state.lastClickTime < std::chrono::milliseconds(500);
            state.clickCount = sameSpot ? state.clickCount + 1 : 1;
            state.lastClickX = x; state.lastClickY = y;
            state.lastClickTime = now;
            if (state.clickCount >= 2) {
                state.pendingClickValid = false;
                if (state.clickCount == 2) SelectWordAt(state, x, y);
                else SelectLineAt(state, y);
                state.screenSelValid = true;
                g_consoleStatus = L"Выделено — c / Ctrl+C копирует.";
                state.focus = Focus::Page;
                return;
            }
        }

        // Left press arms a potential drag; the click itself is dispatched
        // on release (deferred ~250 ms) so dragging never triggers links
        // or buttons, and double-clicks never fire the first click.
        state.mouseSelDown = true;
        state.selSX = state.selEX = x;
        state.selSY = state.selEY = y;
        state.selStartScroll = state.scrollOffset;
        state.clickCtrl = (mouse.dwControlKeyState &
            (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
        state.screenSelValid = false;
        state.focus = Focus::Page;
        return;
    }

    // Left-click dispatch — runs on button release when the drag turned
    // out to be a plain click (see HandleMouse / FinishScreenSel).
    void HandleLeftClick(State& state, int x, int y) {
        RefreshSize(state);
        // Command palette rows (modal overlay, topmost).
        if (state.paletteOpen) {
            for (const auto& hit : state.paletteHits) {
                if (y == hit.row && x >= hit.left && x <= hit.right) {
                    PaletteActivate(state, hit.index);
                    return;
                }
            }
            // Click outside the palette box closes it, then the click
            // falls through to normal handling.
            PaletteClose(state);
        }
        // Settings dropdown rows (topmost overlay).
        for (const auto& hit : state.settingsHits) {
            if (y == hit.row && x >= hit.left && x <= hit.right) {
                ActivateSettingsItem(state, hit.index);
                return;
            }
        }
        // Autocomplete dropdown hits (they overlay content rows).
        for (const auto& hit : state.suggestHits) {
            if (y == hit.row && x >= hit.left && x <= hit.right &&
                hit.index < static_cast<int>(state.suggestions.size())) {
                state.input = state.suggestions[static_cast<size_t>(hit.index)];
                state.suggestIndex = -1;
                state.caret = state.input.size();
                state.inputViewStart = 0;
                state.inputUndo.clear();
                state.browsingHistory = false;
                state.focus = Focus::Search;
                return;
            }
        }
        // Home-page bookmark hits.
        if (!state.bookmarkHits.empty()) {
            const auto bookmarks = GetBookmarksCached();
            for (const auto& hit : state.bookmarkHits) {
                if (y == hit.row && x >= hit.left && x <= hit.right &&
                    hit.index < static_cast<int>(bookmarks.size())) {
                    UiOpenUrl(bookmarks[static_cast<size_t>(hit.index)]);
                    return;
                }
            }
        }
        // Tab strip hits first (row 0).
        if (y == 0) {
            // Close buttons take priority over the tab body.
            for (const auto& hit : state.tabHits) {
                if (hit.isClose && x >= hit.left && x <= hit.right) {
                    ApplyView(state, UiCloseTabIndex(hit.index, CurrentView(state)));
                    return;
                }
            }
            for (const auto& hit : state.tabHits) {
                if (hit.isNew && x >= hit.left && x <= hit.right) {
                    ApplyView(state, UiNewTab(CurrentView(state)));
                    return;
                }
                if (!hit.isNew && !hit.isClose &&
                    x >= hit.left && x <= hit.right) {
                    ApplyView(state, UiSwitchTabTo(hit.index, CurrentView(state)));
                    return;
                }
            }
        }
        for (const auto& hit : state.actionHits) {
            if (y == hit.row && x >= hit.left && x <= hit.right) {
                ActivateAction(state, hit.action);
                return;
            }
        }
        if (state.searchButtonLeft >= 0 && y == state.searchRow &&
            x >= state.searchButtonLeft && x <= state.searchButtonRight) {
            Submit(state);
            return;
        }
        if (y >= state.searchBoxTop && y <= state.searchBoxBottom &&
            x >= state.searchBoxLeft && x <= state.searchBoxRight) {
            state.focus = Focus::Search;
            state.hoveredItem = -1;
            const int relative = std::clamp(x - state.searchLeft, 0,
                std::max(0, state.searchRight - state.searchLeft + 1));
            state.caret = std::min(state.input.size(),
                state.inputViewStart + static_cast<size_t>(relative));
            state.browsingHistory = false;
            return;
        }
        for (const auto& row : state.itemRows) {
            if (y == row.row) {
                state.selectedItem = row.item;
                state.hoveredItem = row.item;
                state.focus = Focus::Page;
                ActivateItem(state, row.item);
                return;
            }
        }
        // NOTE: no focus change here — the button press already moved
        // focus to the page. Re-asserting it on the deferred dispatch
        // (~250 ms later) could steal focus back from the omnibox if the
        // user hit Ctrl+L in between.
        // Any click that didn't hit the menu closes it.
        if (state.settingsOpen) {
            state.settingsOpen = false;
            state.settingsHover = -1;
        }
    }

    // Ctrl+C must reach our input loop as a key event (copy), not kill
    // the process via the default console control handler. Ctrl+Break
    // still terminates — it stays the emergency exit.
}
