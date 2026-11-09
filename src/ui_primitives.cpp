#include "ui_int.h"

namespace console_ui {
    void RebuildVisibleFields(State& state) {
        state.visibleFields.clear();
        for (size_t i = 0; i < g_currentFields.size(); ++i) {
            if (g_currentFields[i].kind != FieldKind::Hidden)
                state.visibleFields.push_back(i);
        }
    }

    size_t ItemCount(const State& state) {
        return g_currentLinks.size() + state.visibleFields.size() +
               g_currentImages.size();
    }

    // Item index ranges: [0, L) links, [L, L+F) fields, [L+F, ...) images.
    int ImageIndexForItem(const State& state, int item) {
        const int base = static_cast<int>(g_currentLinks.size() +
                                          state.visibleFields.size());
        if (item < base ||
            static_cast<size_t>(item - base) >= g_currentImages.size())
            return -1;
        return item - base;
    }

    // Global item index -> row in pageLines space.
    size_t ItemToRow(const State& state, int item) {
        if (item < 0 || static_cast<size_t>(item) >= state.itemPageRows.size())
            return static_cast<size_t>(-1);
        return state.itemPageRows[static_cast<size_t>(item)];
    }

    // Global item index -> index into g_currentFields, or -1 for links.
    int FieldIndexForItem(const State& state, int item) {
        if (item < 0 || static_cast<size_t>(item) >= ItemCount(state) ||
            static_cast<size_t>(item) < g_currentLinks.size())
            return -1;
        const size_t pos = static_cast<size_t>(item) - g_currentLinks.size();
        if (pos >= state.visibleFields.size()) return -1;
        return static_cast<int>(state.visibleFields[pos]);
    }

    // g_currentFields index -> global item index (links first, then fields).
    int ItemIndexForField(const State& state, int fieldIdx) {
        for (size_t pos = 0; pos < state.visibleFields.size(); ++pos)
            if (static_cast<int>(state.visibleFields[pos]) == fieldIdx)
                return static_cast<int>(g_currentLinks.size() + pos);
        return -1;
    }

    std::wstring FormatBytes(uint64_t bytes) {
        if (bytes >= 1024 * 1024)
            return std::to_wstring(bytes / (1024 * 1024)) + L" МБ";
        if (bytes >= 1024)
            return std::to_wstring(bytes / 1024) + L" КБ";
        return std::to_wstring(bytes) + L" Б";
    }

    TabViewState CurrentView(const State& state) {
        return {state.scrollOffset, state.selectedItem, state.hoveredItem,
                state.findMode, state.findQuery, state.findPos,
                state.editingField};
    }

    void ClearSelection(State& state); // defined below, near PageLine

    void ApplyView(State& state, const TabViewState& view) {
        CloseImageViewer(state);
        ClearSelection(state);
        state.scrollOffset = view.scrollOffset;
        state.selectedItem = view.selectedItem;
        state.hoveredItem = -1;
        state.findMode = view.findMode;
        state.findQuery = view.findQuery;
        state.findPos = view.findPos;
        state.editingField = view.editingField;
        state.inputHistory.clear();
        state.historyIndex = 0;
        state.browsingHistory = false;
        if (view.editingField >= 0 &&
            static_cast<size_t>(view.editingField) < g_currentFields.size()) {
            // Restore the field editor with the stored value.
            state.input = ToWide(g_currentFields[static_cast<size_t>(view.editingField)].value);
            state.caret = state.input.size();
            state.inputViewStart = 0;
            state.focus = Focus::Search;
        } else {
            state.editingField = -1;
            state.input.clear();
            state.caret = 0;
            state.inputViewStart = 0;
            state.focus = Focus::Page;
        }
        state.inputUndo.clear(); // wholesale replace — fresh edit session
    }

    void RefreshSize(State& state) {
        if (!GetConsoleScreenBufferInfo(state.output, &state.info)) return;
        state.left = state.info.srWindow.Left;
        state.top = state.info.srWindow.Top;
        state.width = static_cast<SHORT>(std::max<LONG>(40,
            state.info.srWindow.Right - state.info.srWindow.Left + 1));
        state.height = static_cast<SHORT>(std::max<LONG>(15,
            state.info.srWindow.Bottom - state.info.srWindow.Top + 1));
    }

    void Clear(State& state) {
        // (Re)build the frame buffer; a size change invalidates the
        // previous frame so FlushFrame repaints everything.
        const size_t n = static_cast<size_t>(state.width) * state.height;
        if (state.cells.size() != n || state.prevCells.size() != n) {
            state.cells.assign(n, CHAR_INFO{L' ', kText});
            state.prevCells.clear();
        } else {
            std::fill(state.cells.begin(), state.cells.end(),
                      CHAR_INFO{L' ', kText});
        }
    }

    void At(State& state, int x, int y, const std::wstring& value, WORD color  , int maxWidth  ) {
        if (y < 0 || y >= state.height || x < 0 || x >= state.width) return;
        const int room = state.width - x;
        const int limit = maxWidth < 0 ? room : std::min(room, maxWidth);
        if (limit <= 0) return;
        std::wstring text = value;
        for (wchar_t& ch : text)
            if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
        if (static_cast<int>(text.size()) > limit)
            text.resize(static_cast<size_t>(limit));
        // Draw into the frame buffer — FlushFrame() pushes diffs to the
        // real console once per render.
        const size_t need = (static_cast<size_t>(y) + 1) * state.width;
        if (state.cells.size() < need) return;
        CHAR_INFO* row = state.cells.data() +
            static_cast<size_t>(y) * state.width + x;
        const size_t n = text.size();
        for (size_t i = 0; i < n; ++i) {
            row[i].Char.UnicodeChar = text[i];
            row[i].Attributes = color;
        }
    }

    // Draw a text line, highlighting every occurrence of `highlight`
    // (the last Ctrl+F / "/" query) with the selection color.
    void DrawText(State& state, int x, int y, const std::wstring& text, WORD color, const std::wstring& highlight) {
        const std::wstring needle = LowerWide(highlight);
        if (needle.empty() || text.empty()) {
            At(state, x, y, text, color);
            return;
        }
        const std::wstring haystack = LowerWide(text);
        const size_t qlen = needle.size();
        int cx = x;
        size_t seg = 0;
        for (size_t p = haystack.find(needle); p != std::wstring::npos;
             p = haystack.find(needle, p + qlen)) {
            if (p > seg) {
                At(state, cx, y, text.substr(seg, p - seg), color);
                cx += static_cast<int>(p - seg);
            }
            At(state, cx, y, text.substr(p, qlen), kSelected);
            cx += static_cast<int>(qlen);
            seg = p + qlen;
        }
        if (seg < text.size())
            At(state, cx, y, text.substr(seg), color);
    }

    void Center(State& state, int y, const std::wstring& value, WORD color  ) {
        const int x = std::max(0, (static_cast<int>(state.width) -
                                   static_cast<int>(value.size())) / 2);
        At(state, x, y, value, color);
    }

    std::wstring Shorten(std::wstring value, size_t maxChars) {
        if (value.size() <= maxChars) return value;
        if (maxChars < 4) return value.substr(0, maxChars);
        return value.substr(0, maxChars - 3) + L"...";
    }

    std::vector<std::wstring> Wrap(const std::wstring& source, size_t lineWidth, size_t maxLines) {
        std::vector<std::wstring> lines;
        if (lineWidth < 10 || maxLines == 0) return lines;

        std::wistringstream paragraphs(source);
        std::wstring paragraph;
        while (std::getline(paragraphs, paragraph) && lines.size() < maxLines) {
            // \x01 marks a verbatim (<pre>) line: keep it unwrapped so
            // indentation and spacing survive.
            if (!paragraph.empty() && paragraph.front() == L'\x01') {
                lines.push_back(paragraph.substr(1));
                continue;
            }
            std::wistringstream words(paragraph);
            std::wstring word, line;
            while (words >> word) {
                if (line.empty()) {
                    line = word;
                } else if (line.size() + 1 + word.size() <= lineWidth) {
                    line += L" " + word;
                } else {
                    lines.push_back(line);
                    line = word;
                    if (lines.size() >= maxLines) break;
                }
            }
            if (!line.empty() && lines.size() < maxLines)
                lines.push_back(line);
            else if (paragraph.empty() && !lines.empty() && lines.size() < maxLines)
                lines.emplace_back();
        }
        return lines;
    }

    void DrawSearchButton(State& state, int row, int left) {
        // Compact "[ Найти ]" button drawn inside the right edge of the box.
        const std::wstring label = L"[ Найти ]";
        state.searchButtonLeft = left;
        state.searchButtonRight = left + static_cast<int>(label.size()) - 1;
        const WORD color = state.hoveredSearchButton ? kHover : kAccent;
        At(state, left, row, label, color);
    }

    void DrawSearchBox(State& state, int row) {
        const int boxWidth = std::min<int>(72, state.width - 8);
        const int x = (state.width - boxWidth) / 2;
        // Reserve the rightmost cells for the search button.
        const int buttonWidth = 9; // "[ Найти ]"
        const int fieldWidth = boxWidth - 6 -
            (boxWidth >= 30 ? buttonWidth : 0);
        state.searchRow = row;
        state.searchLeft = x + 3;
        state.searchRight = x + 3 + fieldWidth - 1;
        state.searchBoxLeft = x;
        state.searchBoxRight = x + boxWidth - 1;
        state.searchBoxTop = row - 1;
        state.searchBoxBottom = row + 1;
        state.dropdownTop = row + 2; // suggestion dropdown starts below box
        if (row > state.height / 2)
            state.dropdownTop = std::max(2, row - 9); // bottom box: list above
        state.searchButtonLeft = -1;
        state.searchButtonRight = -1;
        // hoveredSearchButton is intentionally NOT reset here: it is
        // coordinate-based and re-validated on every mouse move, so keeping
        // it avoids flicker during loading animations.

        state.caret = std::min(state.caret, state.input.size());
        if (state.caret < state.inputViewStart)
            state.inputViewStart = state.caret;
        else if (state.caret >= state.inputViewStart + static_cast<size_t>(fieldWidth))
            state.inputViewStart = state.caret - static_cast<size_t>(fieldWidth) + 1;
        const size_t maxStart = state.input.size() > static_cast<size_t>(fieldWidth)
            ? state.input.size() - static_cast<size_t>(fieldWidth) : 0;
        state.inputViewStart = std::min(state.inputViewStart, maxStart);

        At(state, x, row - 1, L"╭" + std::wstring(boxWidth - 2, L'─') + L"╮", kMuted);
        At(state, x, row, L"│", kMuted);
        std::wstring shown;
        WORD inputColor = kBright;
        if (state.editingField >= 0 &&
            static_cast<size_t>(state.editingField) < g_currentFields.size()) {
            // Editing a form field: show its label as the placeholder hint.
            const FormField& field =
                g_currentFields[static_cast<size_t>(state.editingField)];
            std::wstring hint = field.label.empty()
                ? ToWide(field.name) : ToWide(field.label);
            if (hint.empty()) hint = L"значение поля";
            if (state.input.empty()) {
                shown = L"✎ " + hint;
                inputColor = kMuted;
            } else {
                shown = state.input.substr(state.inputViewStart,
                    static_cast<size_t>(fieldWidth));
            }
        } else if (state.input.empty()) {
            if (state.findMode) {
                shown = L"Найти на странице…";
            } else if (g_consolePageOpen) {
                // Browser-style omnibox: show the current URL until the user types.
                shown = g_currentUrl;
            } else {
                shown = L"Поиск или адрес сайта  ·  Enter";
            }
            inputColor = kMuted;
        } else {
            shown = state.input.substr(state.inputViewStart,
                static_cast<size_t>(fieldWidth));
        }
        // Draw the input text; if a selection exists, highlight its range.
        if (state.inputSelAnchor >= 0 && !shown.empty()) {
            const long selLo = std::min(state.inputSelAnchor,
                                        static_cast<long>(state.caret));
            const long selHi = std::max(state.inputSelAnchor,
                                        static_cast<long>(state.caret));
            const size_t base = state.inputViewStart;
            size_t i = 0;
            while (i < shown.size()) {
                const long pos = static_cast<long>(base + i);
                const bool sel = pos >= selLo && pos < selHi;
                size_t j = i + 1;
                while (j < shown.size()) {
                    const long p2 = static_cast<long>(base + j);
                    if ((p2 >= selLo && p2 < selHi) != sel) break;
                    ++j;
                }
                At(state, x + 3 + static_cast<int>(i), row,
                   shown.substr(i, j - i), sel ? kSelected : inputColor,
                   static_cast<int>(j - i));
                i = j;
            }
        } else {
            At(state, x + 3, row, shown, inputColor, fieldWidth);
        }
        At(state, x + boxWidth - 1, row, L"│", kMuted);
        At(state, x, row + 1, L"╰" + std::wstring(boxWidth - 2, L'─') + L"╯", kMuted);
        if (boxWidth >= 30)
            DrawSearchButton(state, row, x + boxWidth - 10);
    }

    int DrawNavButton(State& state, int x, const std::wstring& label, NavAction action, bool enabled, int row  ) {
        const int width = static_cast<int>(label.size()) + 2;
        const int index = static_cast<int>(state.actionHits.size());
        WORD color = enabled ? kAccent : kMuted;
        if (state.hoveredAction == index) color = kHover;
        At(state, x, row, L"[" + label + L"]", color, width);
        state.actionHits.push_back({row, x, x + width - 1, action});
        return x + width + 1;
    }

    // "⚙ Настройки" pinned to the right edge of the nav row (row 1).
    void DrawSettingsButton(State& state) {
        const std::wstring label = state.width >= 60
            ? L"⚙ Настройки" : L"⚙";
        const int w = static_cast<int>(label.size()) + 2;
        const int x = state.width - w - 2;
        if (x < 2) return;
        const int index = static_cast<int>(state.actionHits.size());
        WORD color = kAccent;
        if (state.settingsOpen || state.hoveredAction == index)
            color = state.hoveredAction == index && !state.settingsOpen
                ? kHover : kSelected;
        At(state, x, 1, L"[" + label + L"]", color, w);
        state.actionHits.push_back({1, x, x + w - 1, NavAction::Settings});
    }

    void EnsureItemVisible(State& state, int item) {
        const size_t row = ItemToRow(state, item);
        if (row == static_cast<size_t>(-1)) return;
        const size_t capacity = static_cast<size_t>(std::max(1, state.pageCapacity));
        if (row < state.scrollOffset) state.scrollOffset = row;
        else if (row >= state.scrollOffset + capacity)
            state.scrollOffset = row - capacity + 1;
    }

    // Address-bar autocomplete dropdown, drawn over the content like a
    // browser's suggestion list. Rows are recorded for mouse clicks.
    void DrawSuggestions(State& state) {
        state.suggestHits.clear();
        if (state.suggestions.empty()) return;
        const int maxRows = std::min<int>(
            static_cast<int>(state.suggestions.size()),
            std::max(1, state.height - 4 - state.dropdownTop));
        for (int i = 0; i < maxRows; ++i) {
            const int y = state.dropdownTop + i;
            const bool active = i == state.suggestIndex || i == state.hoverSuggest;
            std::wstring text = L"  → " + Shorten(state.suggestions[static_cast<size_t>(i)],
                                                   static_cast<size_t>(state.width - 8));
            At(state, 2, y, text, active ? kHover : kMuted, state.width - 4);
            state.suggestHits.push_back({y, 2, state.width - 3, i});
        }
    }

    // Settings dropdown anchored to the top-right corner. Painted last so
    // it overlays page content, like the suggestion list.
    void DrawSettingsMenu(State& state) {
        state.settingsHits.clear();
        if (!state.settingsOpen) return;
        const auto& items = SettingsMenuItems();
        const int w = std::min<int>(46, state.width - 4);
        if (w < 22) { state.settingsOpen = false; return; }
        const int left = state.width - 2 - w;
        const int top = 2;
        // The menu scrolls when it would not fit above the footer.
        const int maxRows = std::max(4, state.height - top - 5);
        const int total = static_cast<int>(items.size());
        state.settingsSel = std::clamp(state.settingsSel, 0, total - 1);
        if (state.settingsSel < state.settingsScroll)
            state.settingsScroll = state.settingsSel;
        if (state.settingsSel >= state.settingsScroll + maxRows)
            state.settingsScroll = state.settingsSel - maxRows + 1;
        state.settingsScroll = std::clamp(state.settingsScroll, 0,
            std::max(0, total - maxRows));
        const int first = state.settingsScroll;
        const int last = std::min(total, first + maxRows);
        const int drawn = last - first;
        const int rows = drawn + 3; // +hint +borders
        const int bottom = top + rows - 1;
        const bool scrollable = total > maxRows;

        At(state, left, top,
           L"╭" + std::wstring(static_cast<size_t>(w - 2), L'─') +
               (scrollable ? L" ↑↓" : L"") + L"╮",
           kAccent);
        At(state, left, bottom,
           L"╰" + std::wstring(static_cast<size_t>(w - 2), L'─') + L"╯",
           kAccent);
        for (int i = first; i < last; ++i) {
            const int y = top + 1 + (i - first);
            At(state, left, y, L"│", kMuted);
            At(state, left + w - 1, y, L"│", kMuted);
            const bool sel = i == state.settingsSel ||
                             i == state.settingsHover;
            const WORD color = sel ? kHover : kText;
            std::wstring label = Shorten(L"  " + items[i].label,
                static_cast<size_t>(w - 4));
            // Fill the whole row for the highlight, then draw the label.
            At(state, left + 1, y,
               std::wstring(static_cast<size_t>(w - 2), L' '), color);
            At(state, left + 1, y, label, color);
            state.settingsHits.push_back(
                {y, left + 1, left + w - 2, i});
        }
        const int hintY = top + 1 + drawn;
        At(state, left, hintY, L"│", kMuted);
        At(state, left + w - 1, hintY, L"│", kMuted);
        std::wstring hint = scrollable
            ? L"  ↑↓ выбор · колёсико — прокрутка"
            : L"  ↑↓ выбор   Enter — выполнить   Esc — закрыть";
        if (scrollable)
            hint += L"  (" + std::to_wstring(first + 1) + L"-" +
                    std::to_wstring(last) + L"/" +
                    std::to_wstring(total) + L")";
        At(state, left + 1, hintY,
           Shorten(hint, static_cast<size_t>(w - 4)), kMuted);
    }

    // Bookmark shortcuts shown on the home (new-tab) page.
    // Top-3 most visited sites from history.txt for the home page.
    std::vector<std::wstring> RecentTopSites() {
        std::unordered_map<std::wstring, int> freq;
        for (const auto& e : ReadHistoryEntries(HistoryFilePath()))
            ++freq[e.url];
        std::vector<std::pair<std::wstring, int>> v(freq.begin(), freq.end());
        std::sort(v.begin(), v.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        std::vector<std::wstring> out;
        for (size_t i = 0; i < v.size() && out.size() < 3; ++i) {
            if (IsInternalPageUrl(v[i].first)) continue;
            out.push_back(v[i].first);
        }
        return out;
    }

    void DrawHomeBookmarks(State& state, int topRow) {
        state.bookmarkHits.clear();
        state.homeBookmarksShown = false;
        const auto bookmarks = GetBookmarksCached();
        const auto recent = bookmarks.empty() && !state.input.empty()
            ? std::vector<std::wstring>{} : RecentTopSites();
        if ((bookmarks.empty() && recent.empty()) || !state.input.empty())
            return;
        // Fit between the hints and the status/footer rows; leave two
        // rows for the "recently visited" line when it exists.
        int room = state.height - 3 - topRow - 1 -
                   (recent.empty() ? 0 : 2);
        const size_t shown = std::min<size_t>(
            std::min<size_t>(bookmarks.size(), 6),
            static_cast<size_t>(std::max(0, room)));
        int y = topRow;
        if (shown > 0) {
            state.homeBookmarksShown = true;
            Center(state, y, L"ЗАКЛАДКИ", kMuted);
            ++y;
            for (size_t i = 0; i < shown; ++i, ++y) {
                // Compact display: strip scheme for readability.
                std::wstring label = bookmarks[i];
                const size_t schemeEnd = label.find(L"://");
                if (schemeEnd != std::wstring::npos)
                    label = label.substr(schemeEnd + 3);
                label = Shorten(label, 48);
                At(state, std::max(2, (state.width - 50) / 2), y,
                   L"  ★ " + label, kAccent, 50);
                state.bookmarkHits.push_back(
                    {y, std::max(2, (state.width - 50) / 2),
                     std::max(2, (state.width - 50) / 2) + 52,
                     static_cast<int>(i)});
            }
        }
        if (!recent.empty()) {
            ++y;
            Center(state, y, L"НЕДАВНО", kMuted);
            ++y;
            std::wstring line = L"  ";
            for (size_t i = 0; i < recent.size(); ++i) {
                std::wstring u = recent[i];
                const size_t se = u.find(L"://");
                if (se != std::wstring::npos) u = u.substr(se + 3);
                u = Shorten(u, 40);
                if (i) line += L"   ·   ";
                line += u;
            }
            Center(state, y, line, kMuted);
        }
    }

    // Draw the browser tab strip on row 0 (always visible, like Chrome).
    void DrawTabStrip(State& state) {
        state.tabHits.clear();
        const size_t count = UiTabCount();
        const int active = static_cast<int>(UiActiveTabIndex());
        int x = 1;
        const int avail = state.width - 8;
        const int perTab = count
            ? std::clamp(avail / static_cast<int>(count), 9, 24) : 0;

        for (size_t i = 0; i < count; ++i) {
            const std::wstring num = L" " + std::to_wstring(i + 1) + L" ";
            std::wstring title = UiTabTitle(i);
            if (UiTabPinned(i)) title = L"▪" + title;
            const int room = std::max(4, perTab - static_cast<int>(num.size()) - 1);
            title = Shorten(title, static_cast<size_t>(room));
            std::wstring label = num + title;
            // Active tab gets a close "×" inside its block.
            const bool isActive = static_cast<int>(i) == active;
            // Download badge on the active tab while transfers run.
            if (isActive && g_downloadRunning.load())
                label = L"⇩" + label;
            if (isActive) label += L"×";
            const int w = static_cast<int>(label.size()) + 1;
            if (x + w > state.width - 4) break; // no room: stop drawing tabs

            if (isActive) {
                // Full-cell highlight like an active browser tab.
                std::wstring block(static_cast<size_t>(w), L' ');
                block.replace(0, label.size(), label);
                At(state, x, 0, block, kSelected);
            } else {
                At(state, x, 0, label, kAccent);
            }
            state.tabHits.push_back(
                {x, x + w - 1, i, false, false});
            if (isActive)
                state.tabHits.push_back(
                    {x + w - 2, x + w - 1, i, false, true});
            x += w + 1;
        }

        // "+" new-tab button.
        if (x + 4 <= state.width - 2) {
            At(state, x, 0, L"[+]", kAccent);
            state.tabHits.push_back({x, x + 3, count, true, false});
        }
    }

}
