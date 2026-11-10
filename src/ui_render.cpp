#include "ui_int.h"

namespace console_ui {

    struct PageLine {
        std::wstring text;
        int itemIndex = -1;  // >=0: selectable link (0..L-1) or field (L..)
        bool heading = false;
    };

    // Push the frame to the console: compare against the previous frame
    // and WriteConsoleOutput only the rows that actually changed.
    void FlushFrame(State& state) {
        const size_t w = state.width;
        const size_t n = w * static_cast<size_t>(state.height);
        if (state.cells.size() != n) return;
        const bool full = state.prevCells.size() != n;
        // Rows covered by the ANSI image viewer are skipped — its pixels
        // live outside the cell buffer; CloseImageViewer forces a repaint.
        const int skipTop = state.viewingImage ? 4 : -1;
        const int skipBottom = state.viewingImage ? state.height - 3 : -1;
        std::wstring chars(static_cast<size_t>(w), L' ');
        std::vector<WORD> attrs(static_cast<size_t>(w));
        for (int y = 0; y < state.height; ++y) {
            if (y >= skipTop && y <= skipBottom) continue;
            const size_t row = static_cast<size_t>(y) * w;
            bool dirty = full;
            if (!dirty) {
                for (size_t x = 0; x < w; ++x) {
                    const CHAR_INFO& a = state.cells[row + x];
                    const CHAR_INFO& b = state.prevCells[row + x];
                    if (a.Char.UnicodeChar != b.Char.UnicodeChar ||
                        a.Attributes != b.Attributes) { dirty = true; break; }
                }
            }
            if (!dirty) continue;
            COORD pos{static_cast<SHORT>(state.left),
                      static_cast<SHORT>(state.top + y)};
            for (size_t x = 0; x < w; ++x) {
                chars[x] = state.cells[row + x].Char.UnicodeChar;
                attrs[x] = state.cells[row + x].Attributes;
            }
            DWORD written = 0;
            WriteConsoleOutputCharacterW(state.output, chars.data(),
                                         static_cast<DWORD>(w), pos, &written);
            WriteConsoleOutputAttribute(state.output, attrs.data(),
                                        static_cast<DWORD>(w), pos, &written);
        }
        state.prevCells = state.cells;
    }

    // Status-line color: errors red, confirmations green, the rest muted.
    // Derived from raw console attributes so it works in every theme.
    // 0 = plain, 1 = error, 2 = ok, 3 = loading.
    static int StatusKind(const std::wstring& status, bool loading) {
        if (loading) return 3;
        static const wchar_t* kErr[] = {
            L"Не ", L"Нельзя", L"Ошибка", L"недоступ", L"Отмена",
            L"отмен", L"fail", L"Превышен", L"не найд", L"Пусто" };
        static const wchar_t* kOk[] = {
            L"скопирован", L"сохранен", L"Сохранено", L"готов",
            L"очищен", L"Добавлен", L"открыт", L"Загружено",
            L"Обновлено", L"Выделено", L"строка выделена",
            L"Слово выделено", L"Автообновление" };
        for (const wchar_t* w : kErr)
            if (status.find(w) != std::wstring::npos) return 1;
        for (const wchar_t* w : kOk)
            if (status.find(w) != std::wstring::npos) return 2;
        return 0;
    }

    WORD StatusColor(const std::wstring& status, bool loading) {
        switch (StatusKind(status, loading)) {
        case 1: return FOREGROUND_RED | FOREGROUND_INTENSITY;
        case 2: return FOREGROUND_GREEN | FOREGROUND_INTENSITY;
        case 3: return kAccent;
        default: return kMuted;
        }
    }

    // Colored status glyph drawn before the status text.
    static std::pair<wchar_t, WORD> StatusMark(const std::wstring& status,
                                               bool loading) {
        switch (StatusKind(status, loading)) {
        case 1: return {L'✗', FOREGROUND_RED | FOREGROUND_INTENSITY};
        case 2: return {L'✓', FOREGROUND_GREEN | FOREGROUND_INTENSITY};
        case 3: return {L'◌', kAccent};
        default: return {L'·', kMuted};
        }
    }

    void Render(State& state) {
        RefreshSize(state);
        // Scrolling or resizing moves the text under the selection
        // rectangle — drop it like a real terminal does on scroll.
        if (state.scrollOffset != state.lastScrollOffset ||
            state.width != state.lastSelW || state.height != state.lastSelH) {
            state.screenSelValid = false;
            state.lastScrollOffset = state.scrollOffset;
            state.lastSelW = state.width;
            state.lastSelH = state.height;
        }
        Clear(state);
        state.itemRows.clear();
        state.itemPageRows.clear();
        state.actionHits.clear();
        state.suggestHits.clear();
        state.bookmarkHits.clear();
        RebuildVisibleFields(state);

        // Address-bar autocomplete: history + bookmarks matching the input.
        state.suggestions.clear();
        if (state.input != state.suggestSource) {
            state.suggestSource = state.input;
            state.suggestIndex = -1; // input changed: drop stale highlight
        }
        if (state.focus == Focus::Search && !state.findMode &&
            state.editingField < 0 && !state.input.empty() &&
            state.input.size() >= 2)
            state.suggestions = GetUrlSuggestions(state.input, 8);
        // Command autocomplete: a single bare word that prefixes a known
        // command gets command suggestions ahead of URL matches.
        {
            const std::wstring in = TrimWide(state.input);
            if (state.focus == Focus::Search && !state.findMode &&
                !in.empty() && in.size() < 24 &&
                in.find(L' ') == std::wstring::npos &&
                in.find(L':') == std::wstring::npos &&
                ((in[0] >= L'a' && in[0] <= L'z') ||
                 (in[0] >= L'A' && in[0] <= L'Z'))) {
                static const wchar_t* kCmds[] = {
                    L"ai", L"back", L"forward", L"reload", L"home",
                    L"history", L"bookmarks", L"downloads", L"download",
                    L"stats", L"tabs", L"copy", L"source", L"info",
                    L"reader", L"backup",
                    L"session", L"md", L"markdown", L"bookmark", L"dup",
                    L"reopen", L"clear-history", L"export", L"import",
                    L"bookmarks-clear", L"cache-clear", L"edge", L"web",
                    L"sync", L"stop", L"pause", L"exit",
                    L"help", L"open", L"search", L"download",
                    L"about", L"notes", L"note", L"cache", L"ip",
                    L"define", L"dict", L"tr", L"translate",
                    L"feedback",
                    L"repo", L"site", L"w", L"wiki", L"scroll",
                    L"title", L"links", L"mdlink",
                    L"pin", L"unpin", L"tabname", L"tab", L"close-left",
                    L"close-right", L"tabmove", L"new", L"close",
                    L"dl", L"up", L"hard-reload", L"offline", L"engine",
                    L"theme", L"zoom", L"statusbar", L"prefetch",
                    L"fullscreen", L"linkpage", L"saveimg", L"imglinks",
                    L"lucky", L"snapshot", L"sessions", L"restore",
                    L"find", L"images", L"copy",
                    L"settings", L"feeds", L"feed", L"palette",
                    L"update", L"noimg", L"verify" };
                const std::wstring low = LowerWide(in);
                std::vector<std::wstring> cmds;
                for (const wchar_t* c : kCmds) {
                    const std::wstring cmd = c;
                    if (cmd.size() >= low.size() &&
                        LowerWide(cmd).compare(0, low.size(), low) == 0)
                        cmds.push_back(cmd);
                    if (cmds.size() >= 6) break;
                }
                if (!cmds.empty()) {
                    cmds.insert(cmds.end(), state.suggestions.begin(),
                                state.suggestions.end());
                    if (cmds.size() > 8) cmds.resize(8);
                    state.suggestions = std::move(cmds);
                }
            }
        }
        if (state.suggestIndex >=
            static_cast<int>(state.suggestions.size()))
            state.suggestIndex = -1;
        const int width = state.width;
        const int height = state.height;
        const bool loading = g_loadRunning.load();

        if (!state.fullscreen) {
            DrawTabStrip(state); // row 0: browser tabs
            DrawSettingsButton(state); // row 1, right edge: ⚙ Настройки
        }

        if (!g_consolePageOpen) {
            state.focus = Focus::Search;
            state.hoveredAction = -1;
            const int center = height / 2;
            if (width >= 44) {
                // Branded ASCII logo, centered.
                Center(state, center - 5,
                       L"▛▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▜", kAccent);
                Center(state, center - 4,
                       L"▌   B R O W S E R   C O N S O L E   ▐", kBright);
                Center(state, center - 3,
                       L"▙▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▟", kAccent);
                Center(state, center - 1, L"v5.0", kMuted);
            } else {
                Center(state, center - 4, L"Browser", kBright);
            }
            DrawSearchBox(state, center - 2);
            Center(state, center + 2,
                   L"Введите запрос или адрес  ·  Enter ", kMuted);
            Center(state, center + 4,
                   L"Ctrl+H — история   Ctrl+F — найти   j/k — ссылки   Enter — открыть   o — адрес", kMuted);
            Center(state, center + 5,
                   L"ai — ИИ без ключа   V — выделение текста", kAccent);
            DrawHomeBookmarks(state, center + 7);
            Center(state, height - 2,
                   L"Tab — поле/ссылки   Ctrl+L — адрес   F6 — поле   b/f — назад/вперёд   Esc — выход", kMuted);
            std::wstring statusLine = g_consoleStatus;
            if (loading) {
                statusLine = statusLine.empty() ? L"Загрузка…" : statusLine;
            }
            if (!statusLine.empty()) {
                const auto mark = StatusMark(statusLine, loading);
                Center(state, height - 1,
                       std::wstring(1, mark.first) + L" " +
                       Shorten(statusLine, static_cast<size_t>(width - 6)),
                       StatusColor(statusLine, loading));
            }
        } else {
            // Browser chrome: row 0 = tabs, row 1 = nav buttons,
            // rows 2-4 = omnibox (address bar), content below.
            int navX = 2;
            const bool canBack = UiNavCanBack();
            const bool canForward = UiNavCanForward();
            const bool canReload = !g_currentUrl.empty();
            if (!state.fullscreen) {
            if (width < 58) {
                navX = DrawNavButton(state, navX, L"←", NavAction::Back, canBack, 1);
                navX = DrawNavButton(state, navX, L"→", NavAction::Forward, canForward, 1);
                navX = DrawNavButton(state, navX, L"⌂", NavAction::Home, true, 1);
                DrawNavButton(state, navX, L"↻", NavAction::Reload, canReload, 1);
            } else {
                navX = DrawNavButton(state, navX, L"← Назад", NavAction::Back, canBack, 1);
                navX = DrawNavButton(state, navX, L"Вперёд →", NavAction::Forward, canForward, 1);
                navX = DrawNavButton(state, navX, L"⌂ Домой", NavAction::Home, true, 1);
                navX = DrawNavButton(state, navX, L"↻ Обновить", NavAction::Reload, canReload, 1);
            }
            }
            DrawSearchBox(state, state.fullscreen ? 1 : 3);

            const size_t textLimit = std::min<size_t>(g_currentText.size(), MAX_DISPLAY_CHARS);
            // Zoom >100% narrows the wrap column — text effectively grows.
            const int effWidth = std::max(20,
                (width - 8) * 100 / std::max(50, UiZoomPercent()));
            const size_t lineWidth = static_cast<size_t>(effWidth);
            // Wrap() is the most expensive part of a frame — cache its result
            // keyed on the displayed text prefix and line width.
            static std::string wrapCacheKey;
            static size_t wrapCacheWidth = 0;
            static std::vector<std::wstring> wrapCache;
            const bool wrapHit = wrapCacheWidth == lineWidth &&
                wrapCacheKey.size() == textLimit &&
                (textLimit == 0 ||
                 std::memcmp(wrapCacheKey.data(), g_currentText.data(),
                             textLimit) == 0);
            if (!wrapHit) {
                wrapCacheKey.assign(g_currentText.data(), textLimit);
                wrapCacheWidth = lineWidth;
                wrapCache = Wrap(ToWide(wrapCacheKey), lineWidth, 3000);
            }
            const auto& wrapped = wrapCache;
            std::vector<PageLine> pageLines;
            pageLines.reserve(wrapped.size() + g_currentLinks.size() +
                              state.visibleFields.size() + 6);
            for (const auto& line : wrapped) {
                // \x02 marks an inline <img>: show a placeholder with the
                // alt text (or URL) instead of the raw marker line.
                if (!line.empty() && line.front() == 0x02) {
                    const std::wstring imgUrl = line.substr(1);
                    std::wstring label = imgUrl;
                    for (const auto& img : g_currentImages) {
                        if (img.url == imgUrl && !img.alt.empty()) {
                            label = ToWide(img.alt);
                            break;
                        }
                    }
                    pageLines.push_back({L"▣ [IMG] " + Shorten(label,
                        lineWidth > 12 ? lineWidth - 12 : lineWidth),
                        -1, false});
                    continue;
                }
                pageLines.push_back({line, -1, false});
            }

            if (pageLines.empty())
                pageLines.push_back({L"На странице нет доступного текстового содержимого.", -1, false});

            // Links occupy item indices 0..L-1.
            if (!g_currentLinks.empty()) {
                pageLines.push_back({L"", -1, false});
                pageLines.push_back({L"── ССЫЛКИ ──────────────────────────────", -1, true});
                for (size_t i = 0; i < g_currentLinks.size(); ++i) {
                    std::wstring label = ToWide(g_currentLinks[i].label);
                    if (label.empty()) label = L"Открыть ссылку";
                    const std::wstring num = L"[" + std::to_wstring(i + 1) + L"]  ";
                    pageLines.push_back({num + Shorten(label,
                                         lineWidth > static_cast<size_t>(num.size() + 4)
                                             ? lineWidth - num.size() - 4 : lineWidth),
                                         static_cast<int>(i), false});
                    state.itemPageRows.push_back(pageLines.size() - 1);
                }
            }

            // Form fields occupy item indices L..L+F-1.
            if (!state.visibleFields.empty()) {
                pageLines.push_back({L"", -1, false});
                pageLines.push_back({L"── ПОЛЯ ФОРМЫ ──  j/k — выбор   Enter — изменить", -1, true});
                const size_t linkCount = g_currentLinks.size();
                for (size_t pos = 0; pos < state.visibleFields.size(); ++pos) {
                    const FormField& field =
                        g_currentFields[state.visibleFields[pos]];
                    std::wstring marker;
                    std::wstring value;
                    switch (field.kind) {
                    case FieldKind::Checkbox:
                        marker = field.checked ? L"[x] " : L"[ ] ";
                        value = ToWide(field.label.empty() ? field.name : field.label);
                        break;
                    case FieldKind::Radio:
                        marker = field.checked ? L"(•) " : L"( ) ";
                        value = ToWide(field.label.empty() ? field.name : field.label);
                        break;
                    case FieldKind::Submit:
                        marker = L"» ";
                        value = ToWide(field.value.empty()
                            ? (field.label.empty() ? field.name : field.label)
                            : field.value);
                        break;
                    case FieldKind::Select:
                        marker = L"≡ ";
                        value = ToWide(field.label.empty() ? field.name : field.label);
                        if (!field.options.empty() &&
                            field.selected < field.options.size())
                            value += L" — " + ToWide(field.options[field.selected]);
                        break;
                    case FieldKind::Password:
                        marker = L"> ";
                        value = ToWide(field.label.empty() ? field.name : field.label) +
                                L" = " + L"••••••";
                        break;
                    default: // Text, Textarea
                        marker = L"> ";
                        value = ToWide(field.label.empty() ? field.name : field.label);
                        {
                            std::wstring v = ToWide(field.value);
                            if (v.size() > 40) v = v.substr(0, 37) + L"...";
                            if (!v.empty()) value += L" = " + v;
                        }
                        break;
                    }
                    pageLines.push_back({marker + Shorten(value,
                        lineWidth > marker.size() + 8
                            ? lineWidth - marker.size() - 8 : lineWidth),
                        static_cast<int>(linkCount + pos), false});
                    state.itemPageRows.push_back(pageLines.size() - 1);
                }
            }

            // Images occupy item indices L+F.. — Enter/click opens the file.
            if (UiImagesEnabled() && !g_currentImages.empty()) {
                pageLines.push_back({L"", -1, false});
                pageLines.push_back({L"── ИЗОБРАЖЕНИЯ ──  Enter — открыть", -1, true});
                const int imgBase = static_cast<int>(
                    g_currentLinks.size() + state.visibleFields.size());
                const size_t shown = std::min<size_t>(g_currentImages.size(), 40);
                for (size_t i = 0; i < shown; ++i) {
                    const PageImage& img = g_currentImages[i];
                    std::wstring label = ToWide(img.alt);
                    if (label.empty()) {
                        // Derive a name from the URL tail.
                        std::wstring u = img.url;
                        const size_t slash = u.find_last_of(L"/");
                        label = slash == std::wstring::npos ? u
                            : u.substr(slash + 1);
                        if (label.empty()) label = L"image";
                    }
                    pageLines.push_back({L"▣ " + Shorten(label,
                        lineWidth > 12 ? lineWidth - 12 : lineWidth),
                        imgBase + static_cast<int>(i), false});
                    state.itemPageRows.push_back(pageLines.size() - 1);
                }
                if (g_currentImages.size() > shown)
                    pageLines.push_back({L"… и ещё " +
                        std::to_wstring(g_currentImages.size() - shown),
                        -1, false});
            }

            const int contentTop = state.fullscreen ? 4 : 6;
            const int footerRow = UiStatusBarOn() ? height - 2 : height - 1;
            // Leave one free row above the status line so content never
            // collides with it.
            const int capacity = std::max(1, footerRow - 1 - contentTop);
            state.pageLineCount = pageLines.size();
            // Snapshot line texts while a selection exists so SelectedText()
            // can join them after this frame's local vector is gone.
            if (state.selectMode || state.selAnchor >= 0) {
                state.selLines.clear();
                state.selLines.reserve(pageLines.size());
                for (const auto& l : pageLines)
                    state.selLines.push_back(l.text);
            }
            state.pageCapacity = capacity;
            const size_t maxOffset = pageLines.size() > static_cast<size_t>(capacity)
                ? pageLines.size() - static_cast<size_t>(capacity) : 0;
            state.scrollOffset = std::min(state.scrollOffset, maxOffset);
            EnsureItemVisible(state, state.selectedItem);
            if (state.hoveredItem >= 0) EnsureItemVisible(state, state.hoveredItem);
            // Keep the field being edited on screen.
            if (state.editingField >= 0) {
                const int fi = ItemIndexForField(state, state.editingField);
                if (fi >= 0) EnsureItemVisible(state, fi);
            }

            const long selLo = std::min(state.selAnchor, state.selCursor);
            const long selHi = std::max(state.selAnchor, state.selCursor);
            const bool selActive = state.selAnchor >= 0;
            for (int visible = 0; visible < capacity; ++visible) {
                const size_t lineIndex = state.scrollOffset + static_cast<size_t>(visible);
                if (lineIndex >= pageLines.size()) break;
                const PageLine& line = pageLines[lineIndex];
                const int y = contentTop + visible;
                WORD color = line.heading ? kAccent : kText;
                // Reader-view markers produced by the HTML parser:
                // ■ heading, • list item — highlight them like a browser.
                if (!line.text.empty() && line.itemIndex < 0) {
                    if (line.text.front() == L'■') color = kAccent;
                    else if (line.text.front() == L'•' || line.text.front() == L'◦' ||
                             line.text.front() == L'▪') color = kBright;
                    else if (line.text.front() == L'▎') color = kMuted;
                }
                if (line.itemIndex >= 0) {
                    const bool highlighted = line.itemIndex == state.selectedItem ||
                                             line.itemIndex == state.hoveredItem;
                    color = highlighted ? kHover : kBright;
                    At(state, 2, y, std::wstring(std::max(0, width - 4), L' '),
                       highlighted ? kHover : kText, width - 4);
                    state.itemRows.push_back({y, line.itemIndex});
                }
                const bool inSel = selActive &&
                    static_cast<long>(lineIndex) >= selLo &&
                    static_cast<long>(lineIndex) <= selHi;
                if (inSel) {
                    // Selected range: full-width highlight like a browser.
                    At(state, 2, y, std::wstring(std::max(0, width - 4), L' '),
                       kSelected, width - 4);
                    At(state, 4, y, line.text, kSelected, width - 8);
                } else if (line.itemIndex >= 0) {
                    // Accent-tinted markers: [n] link numbers and the ▣
                    // image glyph pop against the bright body text.
                    size_t markLen = 0;
                    if (!line.text.empty() && line.text.front() == 0x25A3)
                        markLen = 1;
                    else if (line.itemIndex <
                             static_cast<int>(g_currentLinks.size())) {
                        const size_t close = line.text.find(L']');
                        if (!line.text.empty() && line.text.front() == L'[' &&
                            close != std::wstring::npos && close < 8)
                            markLen = close + 1;
                    }
                    if (markLen > 0 && markLen < line.text.size()) {
                        At(state, 4, y, line.text.substr(0, markLen),
                           kAccent, static_cast<int>(markLen));
                        At(state, 4 + static_cast<int>(markLen), y,
                           line.text.substr(markLen), color,
                           width - 8 - static_cast<int>(markLen));
                    } else {
                        At(state, 4, y, line.text, color, width - 8);
                    }
                } else {
                    DrawText(state, 4, y, line.text, color, state.findQuery);
                }
            }

            // Scrollbar on the right edge of the content area.
            if (pageLines.size() > static_cast<size_t>(capacity) &&
                width >= 12) {
                const int sx = width - 2;
                const size_t total = pageLines.size();
                const size_t cap = static_cast<size_t>(capacity);
                const size_t thumbH = std::max<size_t>(
                    1, cap * cap / total);
                const size_t travel = cap > thumbH ? cap - thumbH : 0;
                const size_t range = total > cap ? total - cap : 1;
                const size_t thumbTop = travel == 0 ? 0 :
                    (state.scrollOffset * travel) / range;
                for (int i = 0; i < capacity; ++i) {
                    const bool inThumb = static_cast<size_t>(i) >= thumbTop &&
                        static_cast<size_t>(i) < thumbTop + thumbH;
                    At(state, sx, contentTop + i, L"█",
                       inThumb ? kAccent : kMuted);
                }
            }

            const std::wstring position = pageLines.empty() ? L"" :
                std::to_wstring(state.scrollOffset + 1) + L"-" +
                std::to_wstring(std::min(pageLines.size(),
                    state.scrollOffset + static_cast<size_t>(capacity))) + L" / " +
                std::to_wstring(pageLines.size());
            At(state, std::max(2, width - static_cast<int>(position.size()) - 3),
               5, position, kMuted);

            std::wstring status = g_consoleStatus;
            if (loading) {
                const uint64_t got = g_loadDownloaded.load();
                const uint64_t total = g_loadTotal.load();
                const std::wstring prev = status;
                // ASCII progress bar — visible even before Content-Length
                // arrives (indeterminate spinner while total is unknown).
                if (total > 0) {
                    const int pct = static_cast<int>(std::min<uint64_t>(100, got * 100 / total));
                    const int filled = pct * 20 / 100;
                    status = L"[" + std::wstring(static_cast<size_t>(filled), L'█') +
                        std::wstring(static_cast<size_t>(20 - filled), L'░') + L"] " +
                        std::to_wstring(pct) + L"%  ";
                } else {
                    status = L"[" + std::wstring(1, L"-\\|/"[
                        static_cast<int>(Clock::now().time_since_epoch().count() / 200000000) % 4]) +
                        L"] ";
                }
                status += L"Загрузка… " + FormatBytes(got);
                if (total > 0)
                    status += L" / " + FormatBytes(total);
                // Speed + ETA once we have at least a second of data.
                const auto elapsed = std::chrono::duration_cast<
                    std::chrono::milliseconds>(Clock::now() - g_loadStart);
                if (elapsed.count() >= 800 && got > 0) {
                    const double sec = elapsed.count() / 1000.0;
                    const double speed = got / sec;
                    status += L" · " + FormatBytes(
                        static_cast<uint64_t>(speed)) + L"/с";
                    if (total > got) {
                        const double eta = (total - got) / speed;
                        if (eta < 3600)
                            status += L" · ~" +
                                std::to_wstring(static_cast<int>(eta) + 1) +
                                L"с";
                    }
                }
                status += L"   Esc — отмена   " + prev;
            }
            else if (state.editingField >= 0) {
                status = L"Enter — сохранить   Ctrl+Enter — отправить форму   Esc — отменить";
            } else if (state.hoveredItem >= 0 &&
                       static_cast<size_t>(state.hoveredItem) < ItemCount(state)) {
                const int hi = state.hoveredItem;
                if (static_cast<size_t>(hi) < g_currentLinks.size())
                    status = g_currentLinks[static_cast<size_t>(hi)].url;
                else if (ImageIndexForItem(state, hi) >= 0)
                    status = g_currentImages[static_cast<size_t>(
                        ImageIndexForItem(state, hi))].url;
                else {
                    const size_t fp = state.visibleFields[
                        static_cast<size_t>(hi) - g_currentLinks.size()];
                    const FormField& f = g_currentFields[fp];
                    status = L"Поле: " + ToWide(f.label.empty() ? f.name : f.label);
                }
            } else if (state.selectedItem >= 0 &&
                       static_cast<size_t>(state.selectedItem) < ItemCount(state)) {
                const int si = state.selectedItem;
                if (static_cast<size_t>(si) < g_currentLinks.size()) {
                    status = L"[" + std::to_wstring(si + 1) + L"] " +
                             g_currentLinks[static_cast<size_t>(si)].url;
                } else if (ImageIndexForItem(state, si) >= 0) {
                    status = g_currentImages[static_cast<size_t>(
                        ImageIndexForItem(state, si))].url;
                } else {
                    const size_t fp = state.visibleFields[
                        static_cast<size_t>(si) - g_currentLinks.size()];
                    const FormField& f = g_currentFields[fp];
                    status = L"Поле: " + ToWide(f.label.empty() ? f.name : f.label) +
                             L"   Enter — изменить";
                }
            }
            if (g_downloadRunning.load()) {
                const uint64_t dlNow = TotalDownloadProgress();
                const auto dlNowT = Clock::now();
                std::wstring dl = L"⇩ " + FormatBytes(dlNow);
                // Speed: bytes delta over the sampling window.
                if (state.dlPrevTime.time_since_epoch().count() != 0) {
                    const double dt = std::chrono::duration_cast<
                        std::chrono::milliseconds>(dlNowT - state.dlPrevTime)
                        .count() / 1000.0;
                    if (dt > 0.3) {
                        const double spd =
                            static_cast<double>(dlNow - state.dlPrevBytes) / dt;
                        if (spd > 0)
                            dl += L" · " + FormatBytes(
                                static_cast<uint64_t>(spd)) + L"/с";
                    }
                }
                state.dlPrevBytes = dlNow;
                state.dlPrevTime = dlNowT;
                const size_t pending = PendingDownloadCount();
                if (pending > 0)
                    dl += L" (+" + std::to_wstring(pending) + L" в очереди)";
                status = dl + L"   " + status;
            } else {
                state.dlPrevTime = Clock::time_point{};
            }
            if (UiStatusBarOn()) {
            // Wall clock on the footer row.
            {
                const std::time_t now = std::time(nullptr);
                std::tm tmv{};
                localtime_s(&tmv, &now);
                wchar_t cb[8];
                std::swprintf(cb, 8, L"%02d:%02d", tmv.tm_hour, tmv.tm_min);
                At(state, std::max(2, width - 7), footerRow, cb, kMuted);
            }
            {
                const auto mark = StatusMark(status, loading);
                At(state, 2, height - 3, std::wstring(1, mark.first),
                   mark.second);
                At(state, 4, height - 3,
                   Shorten(status, static_cast<size_t>(width - 6)),
                   StatusColor(status, loading), width - 6);
            }
            // Selection size for the indicator line (screen rect or
            // line-range selection).
            std::wstring selInfo;
            if (state.screenSelValid) {
                const int rw = std::abs(state.ssX2 - state.ssX1) + 1;
                const int rh = std::abs(state.ssY2 - state.ssY1) + 1;
                selInfo = L" · выделено " +
                    std::to_wstring(static_cast<long long>(rw) * rh) +
                    L" симв.";
            } else if (state.selectMode && state.selAnchor >= 0 &&
                       state.selCursor >= state.selAnchor) {
                selInfo = L" · выделено " +
                    std::to_wstring(state.selCursor - state.selAnchor + 1) +
                    L" строк.";
            }
            // Scroll position indicator, right-aligned on the status row.
            const bool scrollable =
                state.pageLineCount > static_cast<size_t>(state.pageCapacity);
            if (scrollable || !selInfo.empty()) {
                std::wstring ind;
                if (g_offline) ind += L"офлайн · ";
                if (UiOfflineEnabled()) ind += L"офлайн-режим · ";
                ind += selInfo;
                if (scrollable) {
                    const size_t cap = static_cast<size_t>(state.pageCapacity);
                    const size_t pages =
                        (state.pageLineCount + cap - 1) / cap;
                    const size_t cur = std::min(pages,
                        state.scrollOffset / cap + 1);
                    const size_t maxOff = state.pageLineCount - cap;
                    const int pct = maxOff
                        ? static_cast<int>(state.scrollOffset * 100 / maxOff) : 0;
                    // Extras: link count always; feed item count when the
                    // page is an RSS/Atom document.
                    ind += L"стр " + std::to_wstring(cur) + L"/" +
                        std::to_wstring(pages) + L" · " +
                        std::to_wstring(pct) + L"% · " +
                        std::to_wstring(g_currentLinks.size()) + L" сс.";
                    if (g_isFeed) {
                        // Count feed items lazily — only on feed pages.
                        const std::wstring feedText =
                            ToWide(g_currentText.substr(0, textLimit));
                        size_t items = 0;
                        for (size_t i = 0; i + 2 < feedText.size(); ++i)
                            if (feedText[i] == L'\n' && feedText[i + 1] == L'•')
                                ++items;
                        if (feedText.rfind(L"•", 0) == 0) ++items;
                        ind += L" · лента: " + std::to_wstring(items);
                    }
                }
                At(state, width - static_cast<int>(ind.size()) - 2,
                   height - 3, ind, g_offline ? kAccent : kMuted);
            }
            const std::wstring footer = width >= 100
                ? L"j/k — ссылки/поля   Enter — открыть   Ctrl+Enter — форма   o — адрес   / — найти   b/f — история   g/G — края   Пробел — листать   Ctrl+T/W — вкладки"
                : width >= 78
                ? L"j/k — элементы   Enter — открыть   o — адрес   / — найти   g/G — края   Пробел — листать   Ctrl+T/W — вкладки"
                : L"j/k — элементы   Enter — открыть   o — адрес   / — найти   Пробел — листать";
            At(state, 2, footerRow, footer, kMuted, width - 4);
            } // UiStatusBarOn()
        }

        // Autocomplete dropdown paints last so it overlays page content.
        DrawSuggestions(state);
        DrawPalette(state);        // Ctrl+K palette overlays everything
        DrawSettingsMenu(state);
        DrawScreenSelection(state); // terminal-style highlight on top
        FlushFrame(state);          // diff-push the finished frame

        const int cursorX = std::clamp(
            state.searchLeft + static_cast<int>(state.caret -
                std::min(state.caret, state.inputViewStart)),
            state.searchLeft, std::max(state.searchLeft, state.searchRight));
        COORD cursor{static_cast<SHORT>(state.left + cursorX),
                     static_cast<SHORT>(state.top + state.searchRow)};
        SetConsoleCursorPosition(state.output, cursor);
        CONSOLE_CURSOR_INFO cursorInfo{20, state.focus == Focus::Search ? TRUE : FALSE};
        SetConsoleCursorInfo(state.output, &cursorInfo);
        SetConsoleTextAttribute(state.output, kText);

        if (state.viewingImage)
            DrawImageViewer(state);
    }

}
