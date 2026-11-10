// ui_palette.cpp — Ctrl+K command palette: fuzzy search over commands,
// omnibox history, bookmarks and open tabs in one overlay list.

#include "ui_int.h"

namespace console_ui {

    // Static command catalog for the palette. Keep in sync with
    // ui_commands.cpp — descriptions are shown muted on the right.
    struct PaletteCmd { const wchar_t* cmd; const wchar_t* desc; };
    static const PaletteCmd kPaletteCommands[] = {
        {L"back", L"назад"}, {L"forward", L"вперёд"},
        {L"reload", L"обновить"}, {L"home", L"домой"},
        {L"stop", L"остановить загрузку"}, {L"up", L"на уровень выше"},
        {L"hard-reload", L"обновить мимо кэша"},
        {L"reload 30", L"автообновление каждые 30 с"},
        {L"offline on", L"только кэш (офлайн)"}, {L"offline off", L"сеть"},
        {L"offline status", L"статус офлайн-режима"},
        {L"history", L"история"}, {L"bookmarks", L"закладки"},
        {L"downloads", L"загрузки"}, {L"cache", L"кэш"},
        {L"stats", L"статистика"}, {L"settings", L"настройки"},
        {L"feeds", L"RSS-ленты"}, {L"help", L"справка"},
        {L"about", L"о браузере"}, {L"notes", L"заметки"},
        {L"note ", L"добавить заметку…"},
        {L"ip", L"внешний IP"}, {L"define ", L"словарь слова…"},
        {L"tr ", L"перевод текста…"}, {L"w ", L"википедия…"},
        {L"ai ", L"вопрос ИИ…"}, {L"ai sum", L"пересказ страницы"},
        {L"search ", L"поиск…"}, {L"open ", L"открыть по номеру/адресу"},
        {L"edge ", L"через headless Edge…"}, {L"web", L"WebView2-окно"},
        {L"download ", L"скачать URL…"}, {L"dl ", L"быстрая загрузка…"},
        {L"bookmark", L"добавить в закладки"},
        {L"clear-history", L"очистить историю"},
        {L"clear-history-older ", L"историю старше N дней…"},
        {L"bookmarks-clear", L"очистить закладки"},
        {L"cache-clear", L"очистить кэш"},
        {L"export", L"экспорт данных"}, {L"import ", L"импорт HTML…"},
        {L"session", L"экспорт сессии"}, {L"sessions", L"сохранённые сессии"},
        {L"restore ", L"восстановить сессию…"},
        {L"backup", L"резервная копия"}, {L"snapshot", L"снимок экрана"},
        {L"lucky", L"случайная страница"},
        {L"update", L"проверить обновления"},
        {L"update apply", L"скачать и установить обновление"},
        {L"sync", L"синхронизация Gist"},
        {L"pause", L"пауза загрузок"}, {L"stop", L"отменить загрузки"},
        {L"verify", L"проверить sha256"},
        {L"new", L"новая вкладка"}, {L"close", L"закрыть вкладку"},
        {L"dup", L"дубликат вкладки"}, {L"reopen", L"открыть закрытую"},
        {L"tabs", L"список вкладок"}, {L"tab ", L"перейти на вкладку N…"},
        {L"tabname ", L"имя вкладки…"}, {L"rename ", L"переименовать…"},
        {L"pin", L"закрепить/открепить"},
        {L"close-left", L"закрыть вкладки слева"},
        {L"close-right", L"закрыть вкладки справа"},
        {L"close-others", L"закрыть остальные"},
        {L"tabmove ", L"сдвинуть вкладку…"},
        {L"reader", L"reader-режим"}, {L"source", L"исходный код"},
        {L"source ", L"исходник URL…"}, {L"info", L"о странице"},
        {L"title", L"название страницы"}, {L"links", L"ссылки в буфер"},
        {L"linkpage", L"страница всех ссылок"},
        {L"images", L"страница картинок"}, {L"imglinks", L"URL картинок"},
        {L"saveimg", L"сохранить картинку"},
        {L"copy ", L"копировать url/domain/title/…"},
        {L"find", L"найти на странице"}, {L"find ", L"найти текст…"},
        {L"scroll ", L"прокрутить на N строк…"},
        {L"md", L"страница в Markdown"}, {L"save", L"сохранить страницу"},
        {L"home ", L"сменить домашнюю…"},
        {L"theme", L"список тем"}, {L"theme ", L"тема…"},
        {L"theme save ", L"сохранить тему…"},
        {L"zoom", L"масштаб текста"}, {L"zoomin", L"крупнее"},
        {L"zoomout", L"мельче"},
        {L"statusbar", L"строка состояния"},
        {L"prefetch", L"автоподгрузка ссылок"},
        {L"fullscreen", L"полный экран"}, {L"noimg", L"без картинок"},
        {L"engine", L"поисковик"}, {L"engine ", L"поисковик…"},
        {L"feedback", L"issues на GitHub"}, {L"repo", L"репозиторий"},
        {L"site", L"сайт проекта"}, {L"ver", L"версия"},
        {L"palette", L"эта палитра"},
    };

    void PaletteOpen(State& state) {
        state.paletteOpen = true;
        state.paletteQuery.clear();
        state.paletteIndex = 0;
        PaletteRebuild(state);
    }

    void PaletteClose(State& state) {
        state.paletteOpen = false;
        state.paletteEntries.clear();
        state.paletteMatch.clear();
        state.paletteHits.clear();
    }

    // Subsequence fuzzy match: returns score (0 = no match).
    static int FuzzyScore(const std::wstring& query,
                          const std::wstring& target) {
        if (query.empty()) return 1;
        size_t qi = 0;
        int score = 0;
        bool prevHit = false;
        for (size_t ti = 0; ti < target.size() && qi < query.size(); ++ti) {
            if (towlower(target[ti]) == towlower(query[qi])) {
                score += prevHit ? 6 : 2;
                if (ti == 0 || target[ti - 1] == L' ' ||
                    target[ti - 1] == L'-' || target[ti - 1] == L'_')
                    score += 5;
                ++qi;
                prevHit = true;
            } else {
                prevHit = false;
            }
        }
        return qi == query.size() ? score : 0;
    }

    void PaletteRebuild(State& state) {
        state.paletteEntries.clear();
        const std::wstring q = LowerWide(state.paletteQuery);

        auto add = [&](int kind, const std::wstring& text,
                       const std::wstring& arg = {}, int tab = -1) {
            state.paletteEntries.push_back({kind, text, arg, tab});
        };

        // Commands first — they're the palette's main purpose.
        for (const auto& c : kPaletteCommands) {
            const int s = FuzzyScore(q, LowerWide(c.cmd));
            if (s > 0)
                add(0, c.cmd, c.desc);
        }
        // Omnibox history (recent inputs: URLs and commands).
        for (auto it = state.inputHistory.rbegin();
             it != state.inputHistory.rend(); ++it) {
            if (it->empty()) continue;
            if (FuzzyScore(q, LowerWide(*it)) > 0)
                add(1, *it);
        }
        // Bookmarks.
        for (const auto& url : GetBookmarksCached()) {
            if (FuzzyScore(q, LowerWide(url)) > 0)
                add(2, url, url);
        }
        // Open tabs.
        const size_t tabs = UiTabCount();
        for (size_t i = 0; i < tabs; ++i) {
            const std::wstring label = UiTabTitle(i);
            const std::wstring url = UiTabUrl(i);
            if (FuzzyScore(q, LowerWide(label + L" " + url)) > 0)
                add(3, label.empty() ? url : label, url,
                    static_cast<int>(i));
        }

        // Rank by fuzzy score: primary = entry text, secondary = the
        // kind-specific extra (command description / bookmark URL /
        // tab URL). stable_sort keeps catalog order on ties.
        std::vector<std::pair<int, int>> scored;
        scored.reserve(state.paletteEntries.size());
        for (size_t i = 0; i < state.paletteEntries.size(); ++i) {
            const auto& e = state.paletteEntries[i];
            std::wstring secondary;
            if (e.kind == 0) secondary = e.arg;       // description
            else if (e.kind == 2) secondary = e.arg;  // bookmark url
            else if (e.kind == 3 && e.tab >= 0)
                secondary = UiTabUrl(static_cast<size_t>(e.tab));
            int s = FuzzyScore(q, LowerWide(e.text)) * 2 +
                    FuzzyScore(q, LowerWide(secondary));
            // Commands outrank history/bookmarks/tabs — the palette is
            // primarily a command launcher.
            if (e.kind == 0) s += 40;
            scored.push_back({-s, static_cast<int>(i)});
        }
        std::stable_sort(scored.begin(), scored.end());
        state.paletteMatch.clear();
        for (const auto& sc : scored) {
            state.paletteMatch.push_back(sc.second);
            if (state.paletteMatch.size() >= 40) break;
        }
        state.paletteIndex = state.paletteMatch.empty()
            ? 0 : std::min(state.paletteIndex,
                           static_cast<int>(state.paletteMatch.size()) - 1);
    }

    void PaletteActivate(State& state, int matchPos) {
        if (matchPos < 0 ||
            matchPos >= static_cast<int>(state.paletteMatch.size()))
            return;
        const PaletteEntry& e =
            state.paletteEntries[static_cast<size_t>(
                state.paletteMatch[static_cast<size_t>(matchPos)])];
        PaletteClose(state);
        switch (e.kind) {
        case 0: // command
        case 1: // history — submit like a normal omnibox entry
            state.input = e.text;
            state.caret = e.text.size();
            state.inputViewStart = 0;
            Submit(state);
            break;
        case 2: // bookmark
            if (!e.arg.empty()) UiOpenUrl(e.arg);
            break;
        case 3: // tab
            if (e.tab >= 0)
                ApplyView(state, UiSwitchTabTo(
                    static_cast<size_t>(e.tab), CurrentView(state)));
            break;
        }
        state.focus = Focus::Page;
    }

    static const wchar_t* KindGlyph(int kind) {
        switch (kind) {
        case 0: return L"⌘ ";
        case 1: return L"↺ ";
        case 2: return L"★ ";
        default: return L"▭ ";
        }
    }

    void DrawPalette(State& state) {
        if (!state.paletteOpen) return;
        state.paletteHits.clear();
        const int w = std::min<int>(std::max(40, state.width - 10), 96);
        const int left = std::max(1, (state.width - w) / 2);
        const int maxRows = std::min<int>(
            9, static_cast<int>(state.paletteMatch.size()));
        const int top = 3;

        // Top border with the query inside.
        {
            std::wstring topLine = L"╭─ " + state.paletteQuery;
            topLine += L' ';
            while (static_cast<int>(topLine.size()) < w - 1) topLine += L' ';
            topLine += L"╮";
            At(state, left, top, Shorten(topLine, static_cast<size_t>(w)),
               kAccent, w);
        }
        // Match rows.
        const int visible = maxRows;
        int scroll = 0;
        if (state.paletteIndex >= scroll + visible)
            scroll = state.paletteIndex - visible + 1;
        if (state.paletteIndex < scroll) scroll = state.paletteIndex;
        for (int r = 0; r < visible; ++r) {
            const int mi = scroll + r;
            if (mi >= static_cast<int>(state.paletteMatch.size())) break;
            const PaletteEntry& e = state.paletteEntries[static_cast<size_t>(
                state.paletteMatch[static_cast<size_t>(mi)])];
            const bool sel = mi == state.paletteIndex;
            std::wstring label = KindGlyph(e.kind) + e.text;
            if (e.kind == 0 && !e.arg.empty()) {
                label += L"  ";
                label += Shorten(e.arg, 40);
            } else if (e.kind == 3 && e.tab >= 0) {
                label += L"  · вкладка " +
                    std::to_wstring(e.tab + 1);
            }
            label = Shorten(label, static_cast<size_t>(w - 4));
            const int y = top + 1 + r;
            std::wstring cell(static_cast<size_t>(w), L' ');
            cell.replace(0, 1, L"│");
            cell.replace(1, label.size(), label);
            cell.replace(static_cast<size_t>(w - 1), 1, L"│");
            At(state, left, y, cell, sel ? kSelected : kText, w);
            state.paletteHits.push_back(
                {y, left, left + w - 1, mi});
        }
        // Fill remaining rows + bottom border.
        for (int r = visible; r < maxRows; ++r) {
            const int y = top + 1 + r;
            std::wstring cell(static_cast<size_t>(w), L' ');
            cell.replace(0, 1, L"│");
            cell.replace(static_cast<size_t>(w - 1), 1, L"│");
            At(state, left, y, cell, kText, w);
        }
        {
            const int y = top + 1 + maxRows;
            std::wstring bot = L"╰─ ";
            bot += std::to_wstring(state.paletteMatch.size()) +
                   L" совпадений · ↑↓ выбор · Enter открыть · Esc";
            while (static_cast<int>(bot.size()) < w - 1) bot += L' ';
            bot += L"╯";
            At(state, left, y, Shorten(bot, static_cast<size_t>(w)),
               kMuted, w);
        }
    }

    // Key handling while the palette is open. Returns true when consumed.
    bool PaletteHandleKey(State& state, const KEY_EVENT_RECORD& key) {
        const WORD code = key.wVirtualKeyCode;
        const bool ctrl = (key.dwControlKeyState &
            (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
        const bool alt = (key.dwControlKeyState &
            (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;

        if (key.bKeyDown == FALSE) return true;

        // Any Ctrl/Alt chord closes the palette and falls through to the
        // normal handler (e.g. Ctrl+L refocuses the omnibox).
        if (ctrl || alt) {
            PaletteClose(state);
            return false;
        }
        if (code == VK_ESCAPE) { PaletteClose(state); return true; }
        if (code == VK_UP || code == VK_DOWN) {
            const int n = static_cast<int>(state.paletteMatch.size());
            if (n > 0) {
                state.paletteIndex += code == VK_DOWN ? 1 : -1;
                if (state.paletteIndex < 0) state.paletteIndex = n - 1;
                if (state.paletteIndex >= n) state.paletteIndex = 0;
            }
            return true;
        }
        if (code == VK_PRIOR) { // PageUp
            state.paletteIndex = std::max(0, state.paletteIndex - 9);
            return true;
        }
        if (code == VK_NEXT) { // PageDown
            const int n = static_cast<int>(state.paletteMatch.size());
            state.paletteIndex = std::min(n - 1, state.paletteIndex + 9);
            return true;
        }
        if (code == VK_RETURN) {
            PaletteActivate(state, state.paletteIndex);
            return true;
        }
        if (code == VK_BACK) {
            if (!state.paletteQuery.empty()) {
                // Remove the last UTF-16 code unit (surrogate-safe enough
                // for palette queries — Cyrillic is BMP).
                state.paletteQuery.pop_back();
                PaletteRebuild(state);
            }
            return true;
        }
        if (code == VK_DELETE) {
            state.paletteQuery.clear();
            PaletteRebuild(state);
            return true;
        }
        // Printable characters extend the query.
        if (key.uChar.UnicodeChar >= L' ' &&
            key.uChar.UnicodeChar != 0) {
            state.paletteQuery += key.uChar.UnicodeChar;
            state.paletteIndex = 0;
            PaletteRebuild(state);
            return true;
        }
        return true; // swallow everything else while modal
    }

} // namespace console_ui
