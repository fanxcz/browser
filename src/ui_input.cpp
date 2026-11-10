#include "ui_int.h"

namespace console_ui {
    void HandleKey(State& state, const KEY_EVENT_RECORD& key, bool& running) {
        if (!key.bKeyDown) return;
        const DWORD modifiers = key.dwControlKeyState;
        const bool ctrl = (modifiers & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
        const bool alt = (modifiers & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;
        const bool shift = (modifiers & SHIFT_PRESSED) != 0;
        const WORD code = key.wVirtualKeyCode;

        // Command palette is modal: it eats keys until closed (Esc) or
        // until a Ctrl/Alt chord dismisses it and falls through.
        if (state.paletteOpen && PaletteHandleKey(state, key)) return;

        // Image viewer overlay: any key closes it; D downloads the file.
        if (state.viewingImage) {
            if (!ctrl && !alt && (code == 'D' || code == 'S')) {
                StartDownload(state.viewImageUrl, "");
                return;
            }
            CloseImageViewer(state);
            return;
        }

        // Settings dropdown: arrows/J/K move, Enter runs, Esc closes.
        // Any other key closes the menu and falls through to normal input.
        if (state.settingsOpen) {
            const int count =
                static_cast<int>(SettingsMenuItems().size());
            if (code == VK_ESCAPE) {
                state.settingsOpen = false;
                state.settingsHover = -1;
                return;
            }
            if (code == VK_RETURN || code == VK_SPACE) {
                ActivateSettingsItem(state, state.settingsSel);
                return;
            }
            if (code == VK_UP ||
                (!ctrl && !alt && code == 'K')) {
                state.settingsSel =
                    (state.settingsSel + count - 1) % count;
                state.settingsHover = -1;
                return;
            }
            if (code == VK_DOWN ||
                (!ctrl && !alt && code == 'J')) {
                state.settingsSel = (state.settingsSel + 1) % count;
                state.settingsHover = -1;
                return;
            }
            state.settingsOpen = false;
            state.settingsHover = -1;
        }

        // Ctrl+L also cancels field editing. (!shift — Ctrl+Shift+L is the
        // theme toggle below; VK_L is the same for both.)
        if (ctrl && !shift && code == 'L') {
            state.editingField = -1;
            state.input.clear(); state.caret = 0; state.inputViewStart = 0;
            state.inputUndo.clear();
            state.findMode = false;
            state.focus = Focus::Search; state.browsingHistory = false;
            return;
        }
        // Tab management: Ctrl+T new, Ctrl+W close, Ctrl+Tab cycle,
        // Ctrl+1..8 jump, Ctrl+9 last tab.
        if (ctrl && code == 'T') {
            ApplyView(state, shift ? UiReopenClosedTab(CurrentView(state))
                                   : UiNewTab(CurrentView(state)));
            return;
        }
        if (ctrl && code == 'N') {
            ApplyView(state, UiNewTab(CurrentView(state)));
            return;
        }
        if (code == VK_F1) {
            state.input = L"help";
            state.caret = state.input.size();
            state.inputViewStart = 0;
            state.focus = Focus::Search;
            Submit(state);
            return;
        }
        if (ctrl && code == 'W') {
            ApplyView(state, UiCloseTab(CurrentView(state)));
            return;
        }
        if (ctrl && code == VK_TAB) {
            ApplyView(state, UiCycleTab(shift ? -1 : 1, CurrentView(state)));
            return;
        }
        if (ctrl && code >= '1' && code <= '9') {
            const size_t count = UiTabCount();
            if (count > 1) {
                const size_t target = code == '9'
                    ? count - 1 : static_cast<size_t>(code - '1');
                if (target < count)
                    ApplyView(state, UiSwitchTabTo(target, CurrentView(state)));
            }
            return;
        }
        if (ctrl && code == 'F') {
            if (g_consolePageOpen && !g_currentText.empty()) {
                state.findMode = true;
                state.findPos = 0;
                state.findQuery.clear();
                state.input.clear(); state.caret = 0; state.inputViewStart = 0;
                state.focus = Focus::Search;
            } else {
                g_consoleStatus = L"Сначала откройте страницу с текстом.";
            }
            return;
        }
        if (ctrl && code == 'H') {
            UiShowHistoryPage();
            state.selectedItem = -1; state.hoveredItem = -1;
            state.scrollOffset = 0; state.focus = Focus::Page;
            return;
        }
        if (ctrl && code == 'B') {
            UiShowBookmarksPage();
            state.selectedItem = -1; state.hoveredItem = -1;
            state.scrollOffset = 0; state.focus = Focus::Page;
            return;
        }
        if (ctrl && code == 'J') {
            // Real tab navigation: browser://downloads gets a nav entry,
            // so Alt+Left returns to the previous page.
            UiOpenInternalPage(L"downloads");
            state.selectedItem = -1; state.hoveredItem = -1;
            state.scrollOffset = 0; state.focus = Focus::Page;
            return;
        }
        if (ctrl && shift && code == 'O') {
            UiOpenExternalBrowser();
            return;
        }
        if (ctrl && shift && code == 'L') {
            ToggleTheme();
            return;
        }
        if (ctrl && shift && code == 'E' && !g_currentUrl.empty()) {
            // Re-render the current page through headless Edge (JS).
            UiOpenUrlEdge(g_currentUrl);
            return;
        }
        if (ctrl && code == 'C') {
            // An active screen selection always wins — even from the
            // omnibox, like a real terminal.
            if (state.screenSelValid) {
                CopySelectionOrUrl(state);
                return;
            }
            // In the omnibox: copy the selection or the whole input.
            if (state.focus == Focus::Search && !state.input.empty()) {
                if (HasInputSel(state)) {
                    const auto range = InputSelRange(state);
                    UiCopyToClipboard(state.input.substr(
                        range.first, range.second - range.first));
                    g_consoleStatus = L"Текст из адресной строки скопирован.";
                } else if (UiCopyToClipboard(state.input)) {
                    g_consoleStatus = L"Адрес/запрос скопирован.";
                } else {
                    g_consoleStatus = L"Нечего копировать.";
                }
                return;
            }
            // Text selection first; otherwise link/image URL, else page URL.
            CopySelectionOrUrl(state);
            return;
        }
        if (ctrl && shift && code == 'C') {
            // Copy the whole page text (Ctrl+Shift+C, like browsers).
            if (!g_currentText.empty() &&
                UiCopyToClipboard(ToWide(g_currentText)))
                g_consoleStatus = L"Текст страницы скопирован.";
            else
                g_consoleStatus = L"На странице нет текста.";
            return;
        }
        if (ctrl && !shift && code == VK_INSERT) { // Ctrl+Ins = copy
            CopySelectionOrUrl(state);
            return;
        }
        if (shift && !ctrl && code == VK_INSERT &&
            state.focus == Focus::Search) { // Shift+Ins = paste
            const std::wstring clip = UiGetClipboardText();
            if (!clip.empty()) {
                PushInputUndo(state);
                DeleteInputSel(state);
                state.input.insert(state.caret, clip);
                state.caret += clip.size();
                state.browsingHistory = false;
                state.suggestIndex = -1;
            }
            return;
        }
        if (ctrl && code == 'V' && state.focus == Focus::Search) {
            // Paste clipboard text into the omnibox at the caret.
            const std::wstring clip = UiGetClipboardText();
            if (!clip.empty()) {
                PushInputUndo(state);
                DeleteInputSel(state); // paste replaces the selection
                state.input.insert(state.caret, clip);
                state.caret += clip.size();
                state.browsingHistory = false;
                state.suggestIndex = -1;
            }
            return;
        }
        if ((ctrl && code == 'R') || code == VK_F5) {
            if (g_consolePageOpen) ActivateAction(state, NavAction::Reload);
            return;
        }
        if (code == VK_F3) { // find next/prev occurrence on the page
            FindOnPage(state, shift);
            return;
        }
        if (code == VK_F11) { // fullscreen: hide chrome rows
            state.fullscreen = !state.fullscreen;
            g_consoleStatus = state.fullscreen
                ? L"Полный экран — F11 чтобы выйти."
                : L"Полный экран выключен.";
            return;
        }
        if (ctrl && code == VK_HOME && state.focus == Focus::Page &&
            g_consolePageOpen) {
            state.scrollOffset = 0;
            return;
        }
        if (ctrl && code == VK_END && state.focus == Focus::Page &&
            g_consolePageOpen) {
            const size_t cap = state.pageCapacity > 0
                ? static_cast<size_t>(state.pageCapacity) : 20;
            state.scrollOffset = state.pageLineCount > cap
                ? state.pageLineCount - cap : 0;
            return;
        }
        if (ctrl && code == 'U' && state.focus == Focus::Search) {
            // Clear the omnibox (undoable).
            PushInputUndo(state);
            state.input.clear();
            state.caret = 0;
            state.inputViewStart = 0;
            state.suggestIndex = -1;
            return;
        }
        if (ctrl && code == VK_BACK && state.focus == Focus::Search) {
            if (HasInputSel(state)) { PushInputUndo(state); DeleteInputSel(state); return; }
            // Delete the word before the caret.
            size_t p = state.caret;
            while (p > 0 && (state.input[p - 1] == L' ' ||
                             state.input[p - 1] == L'\t'))
                --p;
            while (p > 0 && state.input[p - 1] != L' ' &&
                   state.input[p - 1] != L'\t')
                --p;
            if (p < state.caret) {
                PushInputUndo(state);
                state.input.erase(p, state.caret - p);
                state.caret = p;
                state.suggestIndex = -1;
            }
            return;
        }
        if (alt && code == VK_LEFT) { ActivateAction(state, NavAction::Back); return; }
        if (alt && code == VK_RIGHT) { ActivateAction(state, NavAction::Forward); return; }
        if (alt && code == VK_HOME) { ActivateAction(state, NavAction::Home); return; }
        // Ctrl+PgUp/PgDn — previous/next tab (plain PgUp/PgDn scrolls).
        if (ctrl && (code == VK_PRIOR || code == VK_NEXT)) {
            ApplyView(state, UiCycleTab(code == VK_PRIOR ? -1 : 1,
                                        CurrentView(state)));
            return;
        }
        // Ctrl+E — focus the address bar pre-filled with the selected
        // link's URL (or the page URL), ready to edit.
        if (ctrl && !shift && code == 'E') {
            state.focus = Focus::Search;
            state.findMode = false;
            state.browsingHistory = false;
            if (state.selectedItem >= 0 &&
                static_cast<size_t>(state.selectedItem) < g_currentLinks.size())
                state.input = g_currentLinks[
                    static_cast<size_t>(state.selectedItem)].url;
            else
                state.input = g_currentUrl;
            state.caret = state.input.size();
            state.inputViewStart = 0;
            state.inputUndo.clear();
            return;
        }
        if (code == VK_BROWSER_BACK) { ActivateAction(state, NavAction::Back); return; }
        if (code == VK_BROWSER_FORWARD) { ActivateAction(state, NavAction::Forward); return; }
        if (code == VK_F6) {
            state.focus = Focus::Search;
            state.caret = state.input.size();
            state.browsingHistory = false;
            return;
        }
        if (ctrl && code == 'D') { UiBookmark(); return; }
        if (ctrl && code == 'S') { UiSavePage(); return; }
        // --- Remaining browser-standard Ctrl combos ---
        if (ctrl && code == 'A' && state.focus == Focus::Search) {
            // Select the whole omnibox input.
            state.inputSelAnchor = 0;
            state.caret = state.input.size();
            return;
        }
        if (ctrl && code == 'G') { FindOnPage(state); return; } // find next
        if (ctrl && code == 'K') { PaletteOpen(state); return; } // palette
        if (ctrl && code == 'I') { // Ctrl+I ~ Tab: jump to the omnibox
            state.focus = Focus::Search;
            state.findMode = false;
            state.browsingHistory = false;
            return;
        }
        if (ctrl && code == 'O') { UiOpenDownloadsFolder(); return; }
        if (ctrl && code == 'P') { UiSavePage(); return; } // print -> save
        if (ctrl && code == 'Q') { running = false; return; } // quit
        if (ctrl && code == 'X' && state.focus == Focus::Search) {
            // Cut: selection first, else the char before the caret.
            if (HasInputSel(state) || state.caret > 0) PushInputUndo(state);
            if (HasInputSel(state)) {
                const auto range = InputSelRange(state);
                const std::wstring cut =
                    state.input.substr(range.first, range.second - range.first);
                if (UiCopyToClipboard(cut)) {
                    state.input.erase(range.first, range.second - range.first);
                    state.caret = range.first;
                    ClearInputSel(state);
                    state.browsingHistory = false;
                }
            } else if (state.caret > 0) {
                state.input.erase(state.caret - 1, 1);
                --state.caret;
                state.browsingHistory = false;
            }
            return;
        }
        if (ctrl && code == 'Y') { // Firefox-style: Ctrl+Y = history
            UiShowHistoryPage();
            state.focus = Focus::Page;
            state.selectedItem = -1; state.hoveredItem = -1;
            state.scrollOffset = 0;
            return;
        }
        if (ctrl && code == 'Z') {
            // In the omnibox/field editor Ctrl+Z undoes text edits;
            // elsewhere it reopens the last closed tab.
            if (state.focus == Focus::Search || state.editingField >= 0) {
                if (!state.inputUndo.empty()) UndoInput(state);
                else ApplyView(state, UiReopenClosedTab(CurrentView(state)));
            } else {
                ApplyView(state, UiReopenClosedTab(CurrentView(state)));
            }
            return;
        }
        if (ctrl && code == 'U' && state.focus == Focus::Page) {
            UiViewSourcePage(); // Firefox-style: Ctrl+U = view source
            return;
        }
        if (code == VK_BACK && state.focus == Focus::Page && !state.findMode) {
            ActivateAction(state, NavAction::Back);
            return;
        }
        if (code == VK_TAB) {
            // Tab accepts the highlighted autocomplete suggestion first.
            if (state.focus == Focus::Search && state.suggestIndex >= 0 &&
                state.suggestIndex < static_cast<int>(state.suggestions.size())) {
                state.input = state.suggestions[static_cast<size_t>(state.suggestIndex)];
                state.suggestIndex = -1;
                state.caret = state.input.size();
                state.inputViewStart = 0;
                state.inputUndo.clear();
                state.browsingHistory = false;
                return;
            }
            state.focus = state.focus == Focus::Search ? Focus::Page : Focus::Search;
            const size_t items = ItemCount(state);
            if (state.focus == Focus::Page && state.selectedItem < 0 && items > 0)
                state.selectedItem = shift ? static_cast<int>(items - 1) : 0;
            else if (state.focus == Focus::Search)
                state.selectedItem = -1;
            return;
        }
        // Ctrl+Enter submits the form: while editing a field it first
        // commits the typed value, then sends the whole form (like a
        // browser's implicit submission).
        if (code == VK_RETURN && ctrl) {
            if (state.editingField >= 0) {
                if (static_cast<size_t>(state.editingField) < g_currentFields.size()) {
                    g_currentFields[static_cast<size_t>(state.editingField)].value =
                        ToUtf8(TrimWide(state.input));
                }
                const int fi = state.editingField;
                state.editingField = -1;
                state.input.clear(); state.caret = 0; state.inputViewStart = 0;
                state.focus = Focus::Page;
                UiSubmitFormForField(fi);
            } else {
                const int target = state.selectedItem >= 0
                    ? state.selectedItem : state.hoveredItem;
                const int fi = FieldIndexForItem(state, target);
                if (fi >= 0) {
                    UiSubmitFormForField(fi);
                } else if (state.focus == Focus::Search &&
                           !TrimWide(state.input).empty()) {
                    // Ctrl+Enter in the omnibox: open in a background tab.
                    const std::wstring q = TrimWide(state.input);
                    if (IsUrlLikeInput(q))
                        ApplyView(state, UiOpenInBackgroundTab(
                            q, CurrentView(state)));
                    else
                        ApplyView(state, UiSearchInBackgroundTab(
                            ToUtf8(q), CurrentView(state)));
                    state.input.clear(); state.caret = 0;
                    state.inputViewStart = 0;
                    state.browsingHistory = false;
                    state.suggestIndex = -1;
                    state.focus = Focus::Page;
                } else {
                    Submit(state);
                }
            }
            return;
        }
        if (code == VK_RETURN) {
            Submit(state);
            if (g_consoleStatus == L"__EXIT__") running = false;
            return;
        }
        if (code == VK_ESCAPE) {
            if (state.editingField >= 0) {
                // Cancel field editing first.
                state.editingField = -1;
                state.input.clear(); state.caret = 0; state.inputViewStart = 0;
                state.focus = Focus::Page;
            } else if (state.findMode) {
                state.findMode = false;
                // Keep findQuery so matches stay highlighted until the next
                // search or navigation, like in a regular browser.
                state.focus = Focus::Page;
            } else if (state.selAnchor >= 0 || state.screenSelValid) {
                // Esc clears an active text selection first.
                ClearSelection(state);
                state.screenSelValid = false;
            } else if (!state.input.empty()) {
                PushInputUndo(state);
                state.input.clear(); state.caret = 0; state.inputViewStart = 0;
                state.browsingHistory = false;
            } else if (g_loadRunning.load()) {
                UiCancelLoad();
            } else if (g_consolePageOpen) {
                if (UiNavCanBack()) UiBack();
                else {
                    g_consolePageOpen = false;
                    state.focus = Focus::Search;
                }
                state.selectedItem = -1;
                state.hoveredItem = -1;
                state.scrollOffset = 0;
                state.focus = g_consolePageOpen ? Focus::Page : Focus::Search;
            } else {
                running = false;
            }
            return;
        }

        if (state.focus == Focus::Page && (code == VK_UP || code == VK_DOWN)) {
            const size_t items = ItemCount(state);
            if (items > 0) {
                const int maxIndex = static_cast<int>(items - 1);
                if (state.selectedItem < 0)
                    state.selectedItem = code == VK_DOWN ? 0 : maxIndex;
                else
                    state.selectedItem = std::clamp(
                        state.selectedItem + (code == VK_DOWN ? 1 : -1), 0, maxIndex);
                EnsureItemVisible(state, state.selectedItem);
            } else if (state.pageLineCount > static_cast<size_t>(state.pageCapacity)) {
                const size_t maxOffset = state.pageLineCount - static_cast<size_t>(state.pageCapacity);
                if (code == VK_DOWN) state.scrollOffset = std::min(maxOffset, state.scrollOffset + 1);
                else if (state.scrollOffset > 0) --state.scrollOffset;
            }
            return;
        }
        // In the address field, Up/Down first navigates the autocomplete
        // dropdown; only without suggestions does it browse input history.
        if (state.focus == Focus::Search && (code == VK_UP || code == VK_DOWN) &&
            !state.suggestions.empty()) {
            const int n = static_cast<int>(state.suggestions.size());
            if (state.suggestIndex < 0)
                state.suggestIndex = code == VK_DOWN ? 0 : n - 1;
            else
                state.suggestIndex = (state.suggestIndex +
                    (code == VK_DOWN ? 1 : -1) + n) % n;
            return;
        }
        if (state.focus == Focus::Search && (code == VK_UP || code == VK_DOWN)) {
            if (!state.inputHistory.empty()) {
                if (!state.browsingHistory) {
                    state.historyDraft = state.input;
                    state.historyIndex = state.inputHistory.size();
                    state.browsingHistory = true;
                }
                if (code == VK_UP) {
                    if (state.historyIndex > 0) --state.historyIndex;
                } else if (state.historyIndex < state.inputHistory.size()) {
                    ++state.historyIndex;
                }
                state.input = state.historyIndex < state.inputHistory.size()
                    ? state.inputHistory[state.historyIndex] : state.historyDraft;
                state.caret = state.input.size();
                state.inputUndo.clear();
                return;
            }
        }
        if (!ctrl && (code == VK_NEXT || code == VK_PRIOR)) {
            const size_t step = static_cast<size_t>(std::max(1, state.pageCapacity - 2));
            const size_t maxOffset = state.pageLineCount > static_cast<size_t>(state.pageCapacity)
                ? state.pageLineCount - static_cast<size_t>(state.pageCapacity) : 0;
            if (code == VK_NEXT) state.scrollOffset = std::min(maxOffset, state.scrollOffset + step);
            else state.scrollOffset = state.scrollOffset > step ? state.scrollOffset - step : 0;
            state.selectedItem = -1;
            state.hoveredItem = -1;
            state.focus = Focus::Page;
            return;
        }
        if (state.focus == Focus::Page && code == VK_HOME) {
            state.scrollOffset = 0; state.selectedItem = -1; state.hoveredItem = -1; return;
        }
        if (state.focus == Focus::Page && code == VK_END) {
            state.scrollOffset = state.pageLineCount > static_cast<size_t>(state.pageCapacity)
                ? state.pageLineCount - static_cast<size_t>(state.pageCapacity) : 0;
            state.selectedItem = -1; state.hoveredItem = -1; return;
        }

        // Fast keyboard navigation while the page has focus — no mouse needed.
        // Printable characters are treated as commands here instead of being
        // typed into the omnibox (use o / Tab / Ctrl+L to type a query).
        if (state.focus == Focus::Page && !ctrl && !alt) {
            const wchar_t ch = key.uChar.UnicodeChar;
            const wchar_t lowerCh = static_cast<wchar_t>(std::towlower(ch));
            const int maxItem = static_cast<int>(ItemCount(state)) - 1;
            const auto moveItems = [&](int delta) {
                if (maxItem < 0) return;
                if (state.selectedItem < 0)
                    state.selectedItem = delta > 0 ? 0 : maxItem;
                else
                    state.selectedItem = std::clamp(state.selectedItem + delta, 0, maxItem);
                EnsureItemVisible(state, state.selectedItem);
            };
            const auto pageScroll = [&](int direction) { // +1 down, -1 up
                const size_t step = static_cast<size_t>(std::max(1, state.pageCapacity));
                const size_t maxOffset = state.pageLineCount > static_cast<size_t>(state.pageCapacity)
                    ? state.pageLineCount - static_cast<size_t>(state.pageCapacity) : 0;
                if (direction > 0)
                    state.scrollOffset = std::min(maxOffset, state.scrollOffset + step);
                else
                    state.scrollOffset = state.scrollOffset > step ? state.scrollOffset - step : 0;
                state.selectedItem = -1;
                state.hoveredItem = -1;
            };

            // V toggles text-selection mode; while active j/k/g/G move the
            // selection cursor line-wise and y copies the range.
            if (ch == L'V') {
                if (state.selectMode) {
                    state.selectMode = false;
                    if (!HasSelection(state)) ClearSelection(state);
                } else {
                    state.selectMode = true;
                    state.selAnchor = state.selCursor =
                        static_cast<long>(state.scrollOffset);
                    state.selectedItem = -1;
                    state.hoveredItem = -1;
                    g_consoleStatus =
                        L"Выделение: j/k — курсор, y — копировать, V/Esc — выход.";
                }
                return;
            }
            if (state.selectMode) {
                const auto moveSel = [&](long delta) {
                    const size_t total = state.selLines.empty()
                        ? state.pageLineCount : state.selLines.size();
                    if (total == 0) return;
                    const long maxLine = static_cast<long>(total) - 1;
                    long c = state.selCursor < 0
                        ? static_cast<long>(state.scrollOffset) : state.selCursor;
                    state.selCursor = std::clamp(c + delta, 0L, maxLine);
                    const int cap = state.pageCapacity > 0
                        ? state.pageCapacity : 20;
                    if (state.selCursor < static_cast<long>(state.scrollOffset))
                        state.scrollOffset = static_cast<size_t>(state.selCursor);
                    else if (state.selCursor >=
                             static_cast<long>(state.scrollOffset) + cap)
                        state.scrollOffset = static_cast<size_t>(
                            state.selCursor) - static_cast<size_t>(cap) + 1;
                };
                if (lowerCh == L'j') { moveSel(1); return; }
                if (lowerCh == L'k') { moveSel(-1); return; }
                if (lowerCh == L'g' && !shift) {
                    state.selCursor = 0; state.scrollOffset = 0; return;
                }
                if (lowerCh == L'g' && shift) {
                    const size_t total = state.selLines.empty()
                        ? state.pageLineCount : state.selLines.size();
                    state.selCursor = total ? static_cast<long>(total) - 1 : 0;
                    const size_t cap = static_cast<size_t>(
                        std::max(1, state.pageCapacity));
                    state.scrollOffset = total > cap ? total - cap : 0;
                    return;
                }
                if (lowerCh == L'y') { // copy the selected range
                    const std::wstring sel = SelectedText(state);
                    if (!sel.empty() && UiCopyToClipboard(sel))
                        g_consoleStatus = L"Выделенный текст скопирован (" +
                            std::to_wstring(sel.size()) + L" симв.).";
                    else
                        g_consoleStatus = L"Выделите диапазон (якорь ≠ курсор).";
                    return;
                }
                // Other keys fall through to normal page commands.
            }
            if (lowerCh == L'j') { moveItems(1); return; }
            if (lowerCh == L'k') { moveItems(-1); return; }
            if (code == VK_SPACE) {
                pageScroll(shift ? -1 : 1);
                return;
            }
            if (lowerCh == L'o') { // omnibox / address bar
                state.input.clear(); state.caret = 0; state.inputViewStart = 0;
                state.findMode = false;
                state.focus = Focus::Search; state.browsingHistory = false;
                return;
            }
            if (ch == L'/') { // quick find, like browser find-as-you-type
                if (!g_currentText.empty()) {
                    state.findMode = true;
                    state.findPos = 0;
                    state.findQuery.clear();
                    state.input.clear(); state.caret = 0; state.inputViewStart = 0;
                    state.focus = Focus::Search;
                } else {
                    g_consoleStatus = L"На странице нет текста для поиска.";
                }
                return;
            }
            // B (shift+b) — open the selected link in a background tab.
            if (ch == L'B' && state.selectedItem >= 0) {
                std::wstring target;
                if (static_cast<size_t>(state.selectedItem) <
                    g_currentLinks.size())
                    target = g_currentLinks[
                        static_cast<size_t>(state.selectedItem)].url;
                else {
                    const int img = ImageIndexForItem(state,
                        state.selectedItem);
                    if (img >= 0)
                        target = g_currentImages[
                            static_cast<size_t>(img)].url;
                }
                if (!target.empty() &&
                    target.rfind(L"openfile://", 0) != 0)
                    ApplyView(state, UiOpenInBackgroundTab(target,
                                                          CurrentView(state)));
                return;
            }
            if (lowerCh == L'b') { ActivateAction(state, NavAction::Back); return; }
            if (lowerCh == L'f') { ActivateAction(state, NavAction::Forward); return; }
            if (lowerCh == L'r') { ActivateAction(state, NavAction::Reload); return; }
            if (lowerCh == L'y') { // copy current page URL to clipboard
                if (!g_currentUrl.empty() && UiCopyToClipboard(g_currentUrl))
                    g_consoleStatus = L"URL скопирован в буфер обмена.";
                else
                    g_consoleStatus = L"Не удалось скопировать (нет страницы?).";
                return;
            }
            if (lowerCh == L'p' &&
                (g_currentUrl == L"browser://downloads" ||
                 g_downloadRunning.load() || PendingDownloadCount() > 0)) {
                UiToggleDownloadPause();
                return;
            }
            if (lowerCh == L'e') {
                if (g_currentUrl == L"browser://downloads")
                    UiOpenDownloadsFolder();
                else
                    UiOpenExternalBrowser(); // open page in system browser
                return;
            }
            if (lowerCh == L'x' && g_currentUrl == L"browser://downloads") {
                // Cancel the selected download (active or queued).
                if (state.selectedItem >= 0 &&
                    static_cast<size_t>(state.selectedItem) <
                        g_currentLinks.size()) {
                    const std::wstring u =
                        g_currentLinks[static_cast<size_t>(state.selectedItem)].url;
                    if (u.rfind(L"openfile://", 0) == 0)
                        UiCancelDownloadByName(u.substr(11));
                    else
                        g_consoleStatus = L"Выберите файл в списке загрузок.";
                } else {
                    g_consoleStatus = L"Сначала выберите файл (j/k).";
                }
                return;
            }
            if (lowerCh == L'v') {
                if (g_currentUrl == L"browser://downloads") {
                    // Verify the selected download against its .sha256 sidecar.
                    if (state.selectedItem >= 0 &&
                        static_cast<size_t>(state.selectedItem) <
                            g_currentLinks.size()) {
                        const std::wstring u =
                            g_currentLinks[static_cast<size_t>(state.selectedItem)].url;
                        if (u.rfind(L"openfile://", 0) == 0)
                            UiVerifyDownloadedFile(u.substr(11));
                        else
                            g_consoleStatus = L"Выберите файл в списке загрузок.";
                    } else {
                        g_consoleStatus = L"Сначала выберите файл (j/k).";
                    }
                    return;
                }
                // On a normal page: paste the clipboard into the omnibox.
                const std::wstring clip = UiGetClipboardText();
                if (!clip.empty()) {
                    state.input = clip;
                    state.caret = clip.size();
                    state.inputViewStart = 0;
                    state.inputUndo.clear();
                    state.browsingHistory = false;
                    state.suggestIndex = -1;
                    state.focus = Focus::Search;
                } else {
                    g_consoleStatus = L"Буфер обмена пуст.";
                }
                return;
            }
            if (lowerCh == L't' || lowerCh == L'd') { // open/download selected item
                if (state.selectedItem < 0) {
                    // `d` with nothing selected downloads the current page URL —
                    // handy right after opening a file link that can't render.
                    if (lowerCh == L'd' && !g_currentUrl.empty() &&
                        g_currentUrl.rfind(L"http", 0) == 0) {
                        StartDownload(g_currentUrl, "");
                    } else {
                        g_consoleStatus = lowerCh == L't'
                            ? L"Сначала выберите элемент (j/k)."
                            : L"Сначала выберите ссылку или изображение (j/k).";
                    }
                    return;
                }
                std::wstring target;
                if (static_cast<size_t>(state.selectedItem) < g_currentLinks.size())
                    target = g_currentLinks[static_cast<size_t>(state.selectedItem)].url;
                else {
                    const int img = ImageIndexForItem(state, state.selectedItem);
                    if (img >= 0)
                        target = g_currentImages[static_cast<size_t>(img)].url;
                }
                if (target.empty()) {
                    g_consoleStatus = L"У выбранного элемента нет адреса.";
                    return;
                }
                if (target.rfind(L"openfile://", 0) == 0) {
                    // Already-downloaded file: open it instead of re-fetching.
                    UiOpenDownloadedFile(target.substr(11));
                    return;
                }
                if (lowerCh == L't') {
                    ApplyView(state, UiOpenInNewTab(target, CurrentView(state)));
                } else {
                    StartDownload(target, "");
                }
                return;
            }
            if (lowerCh == L'g') {
                if (shift) { // G — end of page
                    state.scrollOffset = state.pageLineCount > static_cast<size_t>(state.pageCapacity)
                        ? state.pageLineCount - static_cast<size_t>(state.pageCapacity) : 0;
                } else {     // g — top of page
                    state.scrollOffset = 0;
                }
                state.selectedItem = -1; state.hoveredItem = -1;
                return;
            }
            // [ / ] — previous / next tab (like browsers' Ctrl+Tab pair).
            if (code == VK_OEM_4) {
                ApplyView(state, UiCycleTab(-1, CurrentView(state)));
                return;
            }
            if (code == VK_OEM_6) {
                ApplyView(state, UiCycleTab(1, CurrentView(state)));
                return;
            }
            // , / . — back / forward (Vim-style).
            if (lowerCh == L',') { ActivateAction(state, NavAction::Back); return; }
            if (lowerCh == L'.') { ActivateAction(state, NavAction::Forward); return; }
            // l — focus the address bar (same as o).
            if (lowerCh == L'l') {
                state.input.clear(); state.caret = 0; state.inputViewStart = 0;
                state.findMode = false;
                state.focus = Focus::Search; state.browsingHistory = false;
                return;
            }
            if (lowerCh == L'u') { UiViewSourcePage(); return; }
            if (lowerCh == L'i') { UiPageInfoPage(); return; }
            if (lowerCh == L'h') {
                UiShowHistoryPage();
                state.selectedItem = -1; state.hoveredItem = -1;
                state.scrollOffset = 0; state.focus = Focus::Page;
                return;
            }
            if (lowerCh == L'n') {
                ApplyView(state, UiNewTab(CurrentView(state)));
                return;
            }
            if (lowerCh == L'w') {
                ApplyView(state, UiCloseTab(CurrentView(state)));
                return;
            }
            if (lowerCh == L'q') {
                UiOpenInternalPage(L"downloads");
                state.selectedItem = -1; state.hoveredItem = -1;
                state.scrollOffset = 0; state.focus = Focus::Page;
                return;
            }
            if (lowerCh == L'm') { UiReaderMode(); return; }
            if (ch == L'S') { UiSavePageMarkdown(); return; } // Shift+S
            if (lowerCh == L's') { UiSavePage(); return; }
            if (lowerCh == L'a') { UiBookmark(); return; }
            if (lowerCh == L'c') { CopySelectionOrUrl(state); return; }
            if (lowerCh == L'z') { // copy the whole page text
                if (!g_currentText.empty() &&
                    UiCopyToClipboard(ToWide(g_currentText)))
                    g_consoleStatus = L"Текст страницы скопирован.";
                else
                    g_consoleStatus = L"На странице нет текста.";
                return;
            }
            if (ch == L'?') { // quick help
                state.input = L"help";
                state.caret = state.input.size();
                state.inputViewStart = 0;
                state.focus = Focus::Search;
                Submit(state);
                return;
            }
            // Digits: quick-select a numbered link; Enter opens it.
            // (Only links are numbered — fields are reached via j/k.)
            if (ch >= L'1' && ch <= L'9' || ch == L'0') {
                const int index = ch == L'0' ? 9 : ch - L'1';
                if (index < static_cast<int>(g_currentLinks.size())) {
                    state.selectedItem = index;
                    EnsureItemVisible(state, index);
                    g_consoleStatus = L"Enter — открыть ссылку " +
                        std::to_wstring(index + 1) + L".";
                }
                return;
            }
        }

        // When the page has focus, typing immediately returns focus to the search field.
        if (state.focus == Focus::Page && !ctrl && !alt &&
            key.uChar.UnicodeChar >= L' ' && key.uChar.UnicodeChar != 0) {
            state.focus = Focus::Search;
            if (!state.findMode) {
                state.input.clear();
                state.caret = 0;
                state.inputViewStart = 0;
            }
        }
        if (state.focus != Focus::Search) return;

        // Arrow/Home/End collapse an active input selection to the caret.
        if (state.inputSelAnchor >= 0 &&
            (code == VK_LEFT || code == VK_RIGHT || code == VK_HOME ||
             code == VK_END))
            ClearInputSel(state);
        if (code == VK_LEFT) { if (state.caret > 0) --state.caret; state.browsingHistory = false; return; }
        if (code == VK_RIGHT) { if (state.caret < state.input.size()) ++state.caret; state.browsingHistory = false; return; }
        if (code == VK_HOME) { state.caret = 0; return; }
        if (code == VK_END) { state.caret = state.input.size(); return; }
        if (code == VK_BACK) {
            if (HasInputSel(state)) { PushInputUndo(state); DeleteInputSel(state); return; }
            if (state.caret > 0) {
                size_t eraseAt = state.caret - 1;
                if (eraseAt > 0 && state.input[eraseAt] >= 0xDC00 && state.input[eraseAt] <= 0xDFFF &&
                    state.input[eraseAt - 1] >= 0xD800 && state.input[eraseAt - 1] <= 0xDBFF)
                    --eraseAt;
                PushInputUndo(state);
                state.input.erase(eraseAt, state.caret - eraseAt);
                state.caret = eraseAt;
            }
            state.browsingHistory = false;
            return;
        }
        if (code == VK_DELETE) {
            if (HasInputSel(state)) { PushInputUndo(state); DeleteInputSel(state); return; }
            if (state.caret < state.input.size()) {
                PushInputUndo(state);
                state.input.erase(state.caret, 1);
            }
            state.browsingHistory = false;
            return;
        }
        const wchar_t ch = key.uChar.UnicodeChar;
        // ' toggles case-sensitive find while in find mode.
        if (state.findMode && !ctrl && !alt && ch == L'\'') {
            state.findCase = !state.findCase;
            state.findPos = 0;
            FindOnPage(state);
            g_consoleStatus = (state.findCase ? L"Aa· " : L"aa· ") +
                              g_consoleStatus;
            return;
        }
        if (!ctrl && !alt && ch >= L' ' && ch != 0) {
            PushInputUndo(state);
            DeleteInputSel(state); // typing replaces the selection
            state.input.insert(state.caret, 1, ch);
            ++state.caret;
            state.browsingHistory = false;
        }
    }

}
