#include "ui_int.h"

namespace console_ui {
    void Submit(State& state) {
        std::wstring value = TrimWide(state.input);
        // ':' command prefix — strip so ":back" behaves like "back".
        if (!value.empty() && value.front() == L':')
            value = TrimWide(value.substr(1));
        state.input.clear();
        state.caret = 0;
        state.inputViewStart = 0;
        state.browsingHistory = false;
        state.inputUndo.clear();

        // On the notes page the omnibox appends a line to data/notes.txt.
        if (g_currentUrl == std::wstring(kInternalPrefix) + L"notes") {
            const std::wstring t = TrimWide(value);
            if (t.empty() || LowerWide(t) == L"exit" || LowerWide(t) == L"back") {
                ActivateAction(state, NavAction::Back);
                return;
            }
            UiAddNote(t);
            UiOpenInternalPage(L"notes"); // refresh the page
            g_consoleStatus = L"Заметка добавлена.";
            return;
        }

        // Field editing: Enter commits the value into the form field.
        if (state.editingField >= 0) {
            if (static_cast<size_t>(state.editingField) < g_currentFields.size()) {
                g_currentFields[static_cast<size_t>(state.editingField)].value =
                    ToUtf8(value);
                g_consoleStatus = L"Значение сохранено.";
            }
            state.editingField = -1;
            state.focus = Focus::Page;
            return;
        }

        // In-page find mode: Enter searches (or finds the next occurrence).
        if (state.findMode) {
            FindOnPage(state);
            return;
        }

        // Enter with a highlighted autocomplete suggestion fills the input
        // instead of submitting — press Enter again to go.
        if (!state.suggestions.empty() && state.suggestIndex >= 0) {
            state.input = state.suggestions[static_cast<size_t>(state.suggestIndex)];
            state.suggestIndex = -1;
            state.caret = state.input.size();
            state.inputViewStart = 0;
            state.inputUndo.clear();
            state.browsingHistory = false;
            return;
        }

        if (value.empty()) {
            const int target = state.selectedItem >= 0
                ? state.selectedItem : state.hoveredItem;
            if (g_consolePageOpen && target >= 0)
                ActivateItem(state, target);
            else if (g_consolePageOpen) state.focus = Focus::Page;
            return;
        }
        RememberInput(state, value);

        std::wstring lower = value;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
        if (lower == L"exit" || lower == L"quit") {
            g_consoleStatus = L"__EXIT__";
            return;
        }
        if (lower == L"home" || lower == L"главная") {
            // Custom homepage ("home <url>") wins over the start page.
            const std::wstring hp = UiHomepage();
            if (!hp.empty()) {
                UiOpenUrl(hp);
                state.focus = Focus::Page;
                return;
            }
            UiCancelLoad();
            g_consolePageOpen = false;
            g_consoleStatus.clear();
            state.selectedItem = -1;
            state.hoveredItem = -1;
            state.editingField = -1;
            state.scrollOffset = 0;
            state.focus = Focus::Search;
            return;
        }
        if (lower == L"back") { ActivateAction(state, NavAction::Back); return; }
        if (lower == L"forward") { ActivateAction(state, NavAction::Forward); return; }
        if (lower == L"reload") { ActivateAction(state, NavAction::Reload); return; }
        if (lower.rfind(L"reload ", 0) == 0) {
            // reload <seconds> — auto-refresh interval (repeats).
            try {
                const int secs = std::stoi(
                    ToUtf8(TrimWide(value.substr(7))));
                if (secs > 0 && secs <= 86400) {
                    state.refreshSeconds = secs;
                    state.refreshInterval = secs;
                    state.refreshUrl.clear();
                    state.refreshAt = Clock::now() +
                        std::chrono::seconds(secs);
                    g_consoleStatus = L"Автообновление каждые " +
                        std::to_wstring(secs) + L" с.";
                }
            } catch (...) {
                g_consoleStatus = L"Формат: reload <секунд>";
            }
            return;
        }
        if (lower == L"history") { UiShowHistoryPage(); state.focus = Focus::Page; return; }
        if (lower == L"bookmarks") { UiShowBookmarksPage(); state.focus = Focus::Page; return; }
        if (lower == L"downloads") { UiOpenInternalPage(L"downloads"); state.focus = Focus::Page; return; }
        if (lower.rfind(L"edge ", 0) == 0) {
            // edge <url> — render via headless Edge (JS-heavy pages).
            UiOpenUrlEdge(TrimWide(value.substr(5)));
            state.focus = Focus::Page;
            return;
        }
        if (lower == L"web" || lower.rfind(L"web ", 0) == 0) {
            // web [url|back|forward|reload|zoom-*|find <текст>|off]
            const std::wstring arg = lower.rfind(L"web ", 0) == 0
                ? TrimWide(value.substr(4)) : L"";
            const std::wstring a = LowerWide(arg);
            if (a == L"off" || a == L"close" || a == L"hide") {
                wv::Hide();
                g_consoleStatus = L"WebView2-окно скрыто.";
            } else if (a == L"back") {
                wv::Back();
                g_consoleStatus = L"WebView2: назад.";
            } else if (a == L"forward") {
                wv::Forward();
                g_consoleStatus = L"WebView2: вперёд.";
            } else if (a == L"reload") {
                wv::Reload();
                g_consoleStatus = L"WebView2: обновить.";
            } else if (a == L"zoom-in" || a == L"zoom+") {
                wv::ZoomIn();
            } else if (a == L"zoom-out" || a == L"zoom-") {
                wv::ZoomOut();
            } else if (a == L"zoom-reset" || a == L"zoom0") {
                wv::ZoomReset();
            } else if (a == L"external" || a == L"system") {
                wv::External();
                g_consoleStatus = L"Текущая страница открыта в системном браузере.";
            } else if (a == L"find" || a.rfind(L"find ", 0) == 0) {
                const std::wstring needle = a == L"find"
                    ? TrimWide(arg.substr(4)) : TrimWide(arg.substr(5));
                if (needle.empty()) {
                    g_consoleStatus = L"Формат: web find <текст>";
                } else {
                    wv::Find(needle);
                    g_consoleStatus = L"Поиск на странице: " + needle;
                }
            } else {
                std::wstring target = arg.empty() ? g_currentUrl : L"";
                if (target.empty()) {
                    if (IsUrlLike(arg)) target = NormalizeUrl(arg);
                    else target = L"https://html.duckduckgo.com/html/?q=" +
                        ToWide(UrlEncode(ToUtf8(arg)));
                }
                if (target.empty()) {
                    g_consoleStatus = L"Нет адреса — сначала откройте страницу.";
                } else {
                    wv::Show(target);
                    g_consoleStatus =
                        L"Страница открыта в WebView2-окне (web off — скрыть).";
                }
            }
            return;
        }
        if (lower == L"stop") { StopDownload(); return; }
        if (lower == L"pause") { UiToggleDownloadPause(); return; }
        if (lower == L"sync" || lower.rfind(L"sync ", 0) == 0) {
            UiSync(TrimWide(value.substr(4)));
            return;
        }
        if (lower.rfind(L"download ", 0) == 0) {
            // download <url> [filename] — first token is the URL.
            const std::wstring rest = TrimWide(value.substr(9));
            const size_t sp = rest.find_first_of(L" \t");
            const std::wstring dlUrl = sp == std::wstring::npos
                ? rest : rest.substr(0, sp);
            const std::string dlName = sp == std::wstring::npos
                ? std::string() : ToUtf8(TrimWide(rest.substr(sp + 1)));
            StartDownload(dlUrl, dlName);
            state.focus = Focus::Page;
            return;
        }
        if (lower == L"bookmark" || lower == L"bookmark add") { UiBookmark(); return; }
        if (lower == L"clear-history") { UiClearHistory(); return; }
        if (lower.rfind(L"clear-history-older ", 0) == 0) {
            try {
                ClearHistoryOlderThan(std::stoull(value.substr(20)));
            } catch (...) {
                g_consoleStatus = L"Формат: clear-history-older <дней>";
            }
            return;
        }
        if (lower == L"export") { ExportBookmarksHtml(); return; }
        if (lower.rfind(L"import ", 0) == 0) {
            // Try the path as given, then inside data/ and saved/.
            const std::wstring p = TrimWide(value.substr(7));
            std::error_code ec;
            fs::path fp = fs::path(p);
            if (!fs::exists(fp, ec)) fp = DataDirPath() / p;
            if (!fs::exists(fp, ec)) fp = SavedDirPath() / p;
            if (!fs::exists(fp, ec)) {
                g_consoleStatus = L"Файл не найден: " + p;
            } else {
                const size_t n = ImportBookmarksHtml(fp.wstring());
                g_consoleStatus = L"Импортировано закладок: " +
                    std::to_wstring(n) + L" (" + fp.filename().wstring() + L").";
            }
            return;
        }
        if (lower == L"bookmarks-clear") { UiClearBookmarks(); return; }
        if (lower == L"cache-clear") { UiClearCache(); return; }
        if (lower == L"help") {
            UiOpenInternalPage(L"help");
            state.focus = Focus::Page;
            return;
        }
        if (lower == L"stats") {
            UiOpenInternalPage(L"stats");
            state.focus = Focus::Page;
            return;
        }
        if (lower == L"ai" || lower.rfind(L"ai ", 0) == 0 ||
            lower.rfind(L"ai@", 0) == 0) {
            UiAi(lower == L"ai" ? L"" : TrimWide(value.substr(3)));
            state.focus = Focus::Page;
            return;
        }
        if (lower == L"dup" || lower == L"duplicate") {
            // Duplicate the active tab: reopen its URL in a new tab.
            if (!g_currentUrl.empty() && !IsInternalPageUrl(g_currentUrl))
                ApplyView(state, UiOpenInNewTab(g_currentUrl,
                                                CurrentView(state)));
            return;
        }
        if (lower == L"reopen" || lower == L"reopen-tab") {
            ApplyView(state, UiReopenClosedTab(CurrentView(state)));
            return;
        }
        if (lower == L"tabs") { UiShowTabs(); return; }
        if (lower == L"pin") { UiToggleTabPin(); return; }
        if (lower == L"unpin") {
            if (!UiTabPinned(UiActiveTabIndex()))
                g_consoleStatus = L"Вкладка не закреплена.";
            else
                UiToggleTabPin();
            return;
        }
        // Bare `tabname`/`rename` clears the custom label — the tab goes
        // back to showing the automatic page title.
        if (lower == L"tabname" || lower == L"rename") {
            UiSetTabLabel(L"");
            return;
        }
        if (lower.rfind(L"tabname ", 0) == 0 ||
            lower.rfind(L"rename ", 0) == 0) {
            const size_t off = lower.rfind(L"tabname ", 0) == 0 ? 8 : 7;
            UiSetTabLabel(TrimWide(value.substr(off)));
            return;
        }
        if (lower.rfind(L"tab ", 0) == 0 || lower.rfind(L"tab-", 0) == 0) {
            // tab <n> — jump to tab by its strip number; tab -1 = last.
            const size_t off = lower[3] == L' ' ? 4 : 4;
            const std::wstring arg = TrimWide(value.substr(off));
            try {
                long n = std::stol(ToUtf8(arg));
                const size_t count = UiTabCount();
                if (n < 0) n = static_cast<long>(count) + n; // -1 = last
                if (n >= 1 && static_cast<size_t>(n) <= count) {
                    ApplyView(state, UiSwitchTabTo(
                        static_cast<size_t>(n - 1), CurrentView(state)));
                } else {
                    g_consoleStatus = L"Нет вкладки " + arg + L" (всего " +
                        std::to_wstring(count) + L").";
                }
            } catch (...) { g_consoleStatus = L"Формат: tab <номер>"; }
            return;
        }
        if (lower == L"close-left" || lower == L"close-right") {
            ApplyView(state, UiCloseTabsOnSide(
                lower == L"close-left", CurrentView(state)));
            return;
        }
        if (lower.rfind(L"tabmove ", 0) == 0) {
            const std::wstring arg = LowerWide(TrimWide(value.substr(8)));
            const int delta = arg == L"left" ? -1 : arg == L"right" ? 1 : 0;
            if (delta)
                ApplyView(state, UiMoveTab(delta, CurrentView(state)));
            else g_consoleStatus = L"Формат: tabmove left|right";
            return;
        }
        if (lower == L"copy") {
            if (!g_currentUrl.empty() && UiCopyToClipboard(g_currentUrl))
                g_consoleStatus = L"URL страницы скопирован.";
            else
                g_consoleStatus = L"Нечего копировать.";
            return;
        }
        if (lower.rfind(L"copy ", 0) == 0) {
            // copy <what>: url|domain|title|text|links|images|selection
            const std::wstring what = LowerWide(TrimWide(value.substr(5)));
            std::wstring out;
            if (what == L"url") out = g_currentUrl;
            else if (what == L"domain") {
                std::wstring u = g_currentUrl;
                const size_t p = u.find(L"://");
                if (p != std::wstring::npos) u = u.substr(p + 3);
                const size_t s = u.find_first_of(L"/:");
                out = s == std::wstring::npos ? u : u.substr(0, s);
            }
            else if (what == L"title") out = ToWide(g_currentTitle);
            else if (what == L"text") out = ToWide(g_currentText);
            else if (what == L"links" || what == L"image" ||
                     what == L"images") {
                std::wstring all;
                if (what == L"links") {
                    for (const auto& l : g_currentLinks) {
                        if (!all.empty()) all += L"\r\n";
                        all += l.url;
                    }
                } else {
                    for (const auto& im : g_currentImages) {
                        if (!all.empty()) all += L"\r\n";
                        all += im.url;
                    }
                }
                out = all;
            }
            else if (what == L"selection" || what == L"sel")
                out = ScreenSelectionText(state);
            if (!out.empty() && UiCopyToClipboard(out)) {
                g_consoleStatus = L"Скопировано: " + what + L".";
            } else {
                g_consoleStatus = L"Нечего копировать для '" + what +
                    L"' (варианты: url domain title text links images selection).";
            }
            return;
        }
        if (lower == L"source") { UiViewSourcePage(); return; }
        if (lower.rfind(L"source ", 0) == 0) {
            UiViewSourceUrl(TrimWide(value.substr(7)));
            return;
        }
        if (lower == L"images" || lower.rfind(L"images ", 0) == 0) {
            const std::wstring arg = lower.size() > 6
                ? LowerWide(TrimWide(value.substr(7))) : L"";
            if (arg == L"off" || arg == L"on" || arg == L"status") {
                if (arg == L"status") {
                    g_consoleStatus = std::wstring(L"Изображения: ") +
                        (UiImagesEnabled() ? L"вкл" : L"выкл");
                } else {
                    UiSetImagesEnabled(arg == L"on");
                    g_consoleStatus = arg == L"on"
                        ? L"Изображения включены."
                        : L"Изображения отключены (только текст).";
                }
            } else {
                UiImagesPage();
            }
            return;
        }
        if (lower == L"linkpage" || lower == L"links-page") {
            UiLinksPage();
            return;
        }
        if (lower == L"saveimg" || lower == L"saveimage") {
            const int item = state.hoveredItem >= 0
                ? state.hoveredItem : state.selectedItem;
            const int img = item >= 0 ? ImageIndexForItem(state, item) : -1;
            if (img < 0 ||
                static_cast<size_t>(img) >= g_currentImages.size()) {
                g_consoleStatus = L"Наведите мышь на картинку или выберите её (j/k).";
                return;
            }
            const std::wstring u =
                g_currentImages[static_cast<size_t>(img)].url;
            std::wstring fn = u;
            const size_t slash = fn.find_last_of(L"/");
            if (slash != std::wstring::npos) fn = fn.substr(slash + 1);
            const size_t q = fn.find_first_of(L"?#");
            if (q != std::wstring::npos) fn.resize(q);
            StartDownload(u, ToUtf8(fn));
            return;
        }
        if (lower == L"imglinks") {
            if (g_currentImages.empty()) {
                g_consoleStatus = L"На странице нет изображений.";
                return;
            }
            std::wstring all;
            for (const auto& im : g_currentImages) {
                if (!all.empty()) all += L"\r\n";
                all += im.url;
            }
            if (UiCopyToClipboard(all))
                g_consoleStatus = L"URL изображений скопированы: " +
                    std::to_wstring(g_currentImages.size()) + L" шт.";
            return;
        }
        if (lower.rfind(L"dl ", 0) == 0) {
            const std::wstring u = TrimWide(value.substr(3));
            if (u.empty()) {
                g_consoleStatus = L"Формат: dl <url>";
            } else {
                StartDownload(u, "");
            }
            return;
        }
        if (lower == L"up") {
            // Parent directory of the current URL.
            std::wstring u = g_currentUrl;
            const size_t q = u.find_first_of(L"?#");
            if (q != std::wstring::npos) u.resize(q);
            const size_t scheme = u.find(L"://");
            if (scheme == std::wstring::npos) {
                g_consoleStatus = L"Не http(s)-страница.";
                return;
            }
            const size_t hostEnd = u.find(L'/', scheme + 3);
            if (hostEnd == std::wstring::npos ||
                hostEnd == u.size() - 1) {
                g_consoleStatus = L"Уже в корне сайта.";
                return;
            }
            const size_t lastSlash = u.find_last_of(L'/');
            const std::wstring parent =
                lastSlash <= hostEnd ? u.substr(0, hostEnd) :
                u.substr(0, lastSlash);
            if (parent == g_currentUrl || parent.empty()) {
                g_consoleStatus = L"Уже в корне сайта.";
                return;
            }
            UiOpenUrl(parent);
            return;
        }
        if (lower == L"hard-reload" || lower == L"reload!") {
            UiHardReload();
            return;
        }
        if (lower == L"offline" || lower.rfind(L"offline ", 0) == 0) {
            const std::wstring arg = lower.size() > 7
                ? LowerWide(TrimWide(value.substr(8))) : L"toggle";
            if (arg == L"on") { UiSetOffline(true); }
            else if (arg == L"off") { UiSetOffline(false); }
            else if (arg == L"status") { /* fallthrough to report */ }
            else { UiSetOffline(!UiOfflineEnabled()); }
            g_consoleStatus = std::wstring(L"Офлайн-режим: ") +
                (UiOfflineEnabled() ? L"вкл (только кэш)" : L"выкл");
            return;
        }
        if (lower == L"engine" || lower.rfind(L"engine ", 0) == 0) {
            const std::wstring arg = lower.size() > 6
                ? LowerWide(TrimWide(value.substr(7))) : L"";
            if (arg.empty()) {
                const std::wstring cur = UiGetSetting(L"engine");
                g_consoleStatus = L"Поисковик: " +
                    (cur.empty() ? std::wstring(L"авто (DDG→Bing→Google)") : cur) +
                    L" · engine <ddg|bing|google>";
            } else {
                std::wstring canon;
                if (arg.find(L"ddg") != std::wstring::npos ||
                    arg.find(L"duck") != std::wstring::npos) canon = L"DuckDuckGo";
                else if (arg.find(L"bing") != std::wstring::npos) canon = L"Bing";
                else if (arg.find(L"google") != std::wstring::npos ||
                         arg.find(L"go") == 0) canon = L"Google";
                if (canon.empty()) {
                    g_consoleStatus = L"Неизвестный поисковик. Варианты: ddg, bing, google.";
                } else {
                    UiSetSetting(L"engine", canon);
                    g_consoleStatus = L"Поисковик: " + canon;
                }
            }
            return;
        }
        if (lower == L"theme" || lower.rfind(L"theme ", 0) == 0) {
            const std::wstring arg = lower.size() > 5
                ? LowerWide(TrimWide(value.substr(6))) : L"";
            if (arg.rfind(L"save ", 0) == 0 || arg == L"save") {
                const std::wstring tname = TrimWide(arg.substr(4));
                if (tname.empty()) {
                    g_consoleStatus = L"Формат: theme save <имя>";
                } else if (SaveThemeAs(tname)) {
                    g_consoleStatus = L"Тема сохранена: " + tname;
                } else {
                    g_consoleStatus = L"Не удалось сохранить тему.";
                }
                return;
            }
            if (arg.empty() || arg == L"list") {
                std::wstring list = L"Темы: ";
                for (size_t i = 0; i < g_themes.size(); ++i) {
                    if (i) list += L", ";
                    list += g_themes[i].name;
                    if (i == g_themeIndex) list += L"*";
                }
                g_consoleStatus = list;
            } else {
                bool found = false;
                for (size_t i = 0; i < g_themes.size(); ++i) {
                    if (LowerWide(g_themes[i].name).find(arg) !=
                        std::wstring::npos) {
                        ApplyTheme(i);
                        SaveThemeSetting();
                        g_consoleStatus = L"Тема: " + g_themes[i].name;
                        found = true;
                        break;
                    }
                }
                if (!found)
                    g_consoleStatus = L"Тема не найдена — theme без аргумента покажет список.";
            }
            return;
        }
        if (lower == L"zoom" || lower.rfind(L"zoom ", 0) == 0 ||
            lower == L"zoomin" || lower == L"zoomout") {
            const std::wstring arg = lower == L"zoomin" ? L"in"
                : lower == L"zoomout" ? L"out"
                : lower.size() > 4 ? LowerWide(TrimWide(value.substr(5)))
                : L"status";
            int p = UiZoomPercent();
            if (arg == L"in" || arg == L"+") p += 10;
            else if (arg == L"out" || arg == L"-") p -= 10;
            else if (arg == L"reset" || arg == L"100") p = 100;
            else if (arg != L"status") {
                try { p = std::stoi(ToUtf8(arg)); } catch (...) {}
            }
            UiSetZoomPercent(p);
            g_consoleStatus = L"Масштаб текста: " +
                std::to_wstring(UiZoomPercent()) + L"%";
            return;
        }
        if (lower == L"statusbar" || lower.rfind(L"statusbar ", 0) == 0) {
            const std::wstring arg = lower.size() > 10
                ? LowerWide(TrimWide(value.substr(10))) : L"toggle";
            if (arg == L"on") UiSetStatusBar(true);
            else if (arg == L"off") UiSetStatusBar(false);
            else if (arg == L"status") { /* report below */ }
            else UiSetStatusBar(!UiStatusBarOn());
            g_consoleStatus = std::wstring(L"Строка состояния: ") +
                (UiStatusBarOn() ? L"вкл" : L"выкл");
            return;
        }
        if (lower == L"prefetch" || lower.rfind(L"prefetch ", 0) == 0) {
            const std::wstring arg = lower.size() > 8
                ? LowerWide(TrimWide(value.substr(9))) : L"toggle";
            if (arg == L"on") UiSetPrefetch(true);
            else if (arg == L"off") UiSetPrefetch(false);
            else if (arg == L"status") { /* report below */ }
            else UiSetPrefetch(!UiPrefetchEnabled());
            g_consoleStatus = std::wstring(L"Автоподгрузка ссылок: ") +
                (UiPrefetchEnabled() ? L"вкл" : L"выкл");
            return;
        }
        if (lower == L"fullscreen" || lower.rfind(L"fullscreen ", 0) == 0) {
            const std::wstring arg = lower.size() > 11
                ? LowerWide(TrimWide(value.substr(11))) : L"toggle";
            if (arg == L"on") state.fullscreen = true;
            else if (arg == L"off") state.fullscreen = false;
            else state.fullscreen = !state.fullscreen;
            g_consoleStatus = state.fullscreen
                ? L"Полный экран — F11 чтобы выйти."
                : L"Полный экран выключен.";
            return;
        }
        if (lower.rfind(L"note del ", 0) == 0) {
            try {
                UiNoteDelete(std::stoull(
                    ToUtf8(TrimWide(value.substr(9)))));
            } catch (...) { g_consoleStatus = L"Формат: note del <номер>"; }
            return;
        }
        if (lower == L"notes-clear" || lower == L"notes clear") {
            UiNotesClear();
            return;
        }
        if (lower == L"lucky" || lower == L"random") {
            UiLucky();
            return;
        }
        if (lower == L"snapshot" || lower == L"save-screen") {
            // Dump the current frame buffer as text into saved/shots/.
            const fs::path dir = SavedDirPath() / L"shots";
            std::error_code ec;
            fs::create_directories(dir, ec);
            std::time_t now = std::time(nullptr);
            std::tm tmv{};
            localtime_s(&tmv, &now);
            wchar_t stamp[32];
            swprintf_s(stamp, L"%04d%02d%02d-%02d%02d%02d",
                       tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                       tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
            const fs::path path = dir / (L"shot-" + std::wstring(stamp) + L".txt");
            std::wofstream out(path, std::ios::trunc);
            if (!out) {
                g_consoleStatus = L"Не удалось создать файл снимка.";
            } else {
                const int w = state.width, h = state.height;
                for (int y = 0; y < h && y * w < static_cast<int>(state.cells.size()); ++y) {
                    std::wstring row;
                    for (int x = 0; x < w; ++x) {
                        const CHAR_INFO& ci = state.cells[static_cast<size_t>(y) * w + x];
                        row += ci.Char.UnicodeChar ? ci.Char.UnicodeChar : L' ';
                    }
                    while (!row.empty() && row.back() == L' ') row.pop_back();
                    out << row << L'\n';
                }
                g_consoleStatus = L"Снимок сохранён: " + path.filename().wstring();
            }
            return;
        }
        if (lower == L"sessions") {
            UiSessionsPage();
            return;
        }
        if (lower.rfind(L"restore ", 0) == 0) {
            UiRestoreSession(TrimWide(value.substr(8)));
            return;
        }
        if (lower == L"find" || lower.rfind(L"find ", 0) == 0) {
            if (lower == L"find") {
                state.findMode = true;
                state.focus = Focus::Search;
                state.findQuery.clear();
                state.input.clear();
                state.caret = 0;
                g_consoleStatus = L"Введите текст и нажмите Enter.";
            } else {
                const std::wstring q = TrimWide(value.substr(5));
                state.findMode = true;
                state.findQuery = q;
                state.input = q;
                state.caret = q.size();
                state.findPos = 0;
                state.focus = Focus::Page;
                FindOnPage(state);
            }
            return;
        }
        if (lower == L"new" || lower == L"newtab") {
            ApplyView(state, UiNewTab(CurrentView(state)));
            return;
        }
        if (lower == L"close" || lower == L"closetab") {
            ApplyView(state, UiCloseTab(CurrentView(state)));
            return;
        }
        if (lower == L"info") { UiPageInfoPage(); return; }
        if (lower == L"reader") { UiReaderMode(); return; }
        if (lower == L"backup") { MaybeDailyBackup(); g_consoleStatus = L"Резервная копия данных создана."; return; }
        if (lower == L"session") { UiExportSession(); return; }
        if (lower == L"md" || lower == L"markdown") { UiSavePageMarkdown(); return; }
        if (lower.rfind(L"search ", 0) == 0) {
            value = TrimWide(value.substr(7));
        } else if (lower.rfind(L"open ", 0) == 0) {
            value = TrimWide(value.substr(5));
            const bool digits = !value.empty() && std::all_of(value.begin(), value.end(),
                [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; });
            if (digits) {
                try {
                    const size_t n = std::stoul(value);
                    if (n > 0) OpenSelected(state, n - 1);
                } catch (...) { g_consoleStatus = L"Неверный номер ссылки."; }
                return;
            }
        }

        // --- commands batch 2 ---
        if (lower.rfind(L"tab ", 0) == 0) {
            try {
                const int n = std::stoi(ToUtf8(TrimWide(value.substr(4))));
                const size_t count = UiTabCount();
                if (n >= 1 && static_cast<size_t>(n) <= count)
                    ApplyView(state, UiSwitchTabTo(
                        static_cast<size_t>(n) - 1, CurrentView(state)));
                else g_consoleStatus = L"Нет такой вкладки.";
            } catch (...) { g_consoleStatus = L"Формат: tab <номер>"; }
            return;
        }
        if (lower == L"close-others" || lower == L"closeothers") {
            UiCloseOtherTabs(CurrentView(state));
            return;
        }
        if (lower == L"update" || lower.rfind(L"update ", 0) == 0) {
            const std::wstring arg = lower.size() > 6
                ? LowerWide(TrimWide(value.substr(7))) : L"";
            if (arg == L"apply" || arg == L"install" || arg == L"now") {
                g_consoleStatus = UiApplyUpdate();
                return;
            }
            // Force an update check now: drop the daily stamp and reschedule.
            std::error_code ec;
            fs::remove(DataDirPath() / L"update_check.txt", ec);
            UiScheduleUpdateCheck();
            g_consoleStatus = L"Проверяю обновления… (update apply — установить)";
            return;
        }
        if (lower == L"settings") {
            UiOpenInternalPage(L"settings");
            state.focus = Focus::Page;
            return;
        }
        if (lower == L"feeds" || lower == L"feed") {
            UiOpenInternalPage(L"feeds");
            state.focus = Focus::Page;
            return;
        }
        if (lower == L"feed-list" || lower == L"feed list") {
            UiOpenInternalPage(L"feeds");
            state.focus = Focus::Page;
            return;
        }
        if (lower.rfind(L"feed del ", 0) == 0 ||
            lower.rfind(L"feed remove ", 0) == 0 ||
            lower.rfind(L"feed rm ", 0) == 0) {
            const size_t off = lower.rfind(L"feed remove ", 0) == 0 ? 12
                : lower.rfind(L"feed del ", 0) == 0 ? 9 : 8;
            try {
                const size_t n = std::stoul(ToUtf8(
                    TrimWide(value.substr(off))));
                g_consoleStatus = FeedUnsubscribe(n)
                    ? L"Лента удалена."
                    : L"Нет ленты с таким номером (см. feeds).";
            } catch (...) { g_consoleStatus = L"Формат: feed del <номер>"; }
            return;
        }
        if (lower.rfind(L"feed add ", 0) == 0 ||
            lower.rfind(L"feed ", 0) == 0) {
            // "feed <url>" and "feed add <url>" both subscribe.
            const size_t off = lower.rfind(L"feed add ", 0) == 0 ? 9 : 5;
            const std::wstring u = TrimWide(value.substr(off));
            if (u.empty()) {
                g_consoleStatus = L"Формат: feed add <url-ленты>";
            } else if (FeedSubscribe(u)) {
                g_consoleStatus = L"Лента добавлена: " + u;
            } else {
                g_consoleStatus = L"Не удалось добавить (уже есть или плохой URL).";
            }
            return;
        }
        if (lower == L"palette") { PaletteOpen(state); return; }
        if (lower == L"about") { UiOpenInternalPage(L"about"); state.focus = Focus::Page; return; }
        if (lower == L"notes") { UiOpenInternalPage(L"notes"); state.focus = Focus::Page; return; }
        if (lower == L"cache") { UiOpenInternalPage(L"cache"); state.focus = Focus::Page; return; }
        if (lower == L"ip") { UiOpenInternalPage(L"ip"); state.focus = Focus::Page; return; }
        if (lower == L"feedback") {
            UiShellOpen(L"https://github.com/fanxcz/browser/issues");
            g_consoleStatus = L"Открыто в браузере.";
            return;
        }
        if (lower == L"repo") {
            UiShellOpen(L"https://github.com/fanxcz/browser");
            g_consoleStatus = L"Открыто в браузере.";
            return;
        }
        if (lower == L"site") {
            UiShellOpen(L"https://fanxcz.github.io/browser/");
            g_consoleStatus = L"Открыто в браузере.";
            return;
        }
        if (lower.rfind(L"note ", 0) == 0) {
            UiAddNote(TrimWide(value.substr(5)));
            g_consoleStatus = L"Заметка сохранена (data/notes.txt).";
            return;
        }
        if (lower.rfind(L"define ", 0) == 0 || lower.rfind(L"dict ", 0) == 0) {
            const size_t off = lower.rfind(L"define ", 0) == 0 ? 7 : 5;
            const std::string word = UrlEncode(
                ToUtf8(TrimWide(value.substr(off))));
            UiOpenInternalPage(L"define/" + ToWide(word));
            state.focus = Focus::Page;
            return;
        }
        if (lower.rfind(L"translate ", 0) == 0 ||
            lower.rfind(L"tr ", 0) == 0) {
            const size_t off = lower.rfind(L"translate ", 0) == 0 ? 10 : 3;
            const std::string text = UrlEncode(
                ToUtf8(TrimWide(value.substr(off))));
            UiOpenInternalPage(L"tr/" + ToWide(text));
            state.focus = Focus::Page;
            return;
        }
        if (lower.rfind(L"w ", 0) == 0 || lower.rfind(L"wiki ", 0) == 0) {
            const size_t off = lower.rfind(L"w ", 0) == 0 ? 2 : 5;
            UiOpenUrl(L"https://ru.wikipedia.org/wiki/Special:Search?search=" +
                      ToWide(UrlEncode(ToUtf8(TrimWide(value.substr(off))))));
            return;
        }
        if (lower.rfind(L"home ", 0) == 0) {
            // home <url> — set a custom homepage (persisted).
            const std::wstring hp = TrimWide(value.substr(5));
            if (hp.empty()) {
                UiSetSetting(L"home", L"");
                g_consoleStatus = L"Главная страница сброшена.";
            } else {
                UiSetSetting(L"home", hp);
                g_consoleStatus = L"Главная страница: " + hp;
            }
            return;
        }
        if (lower.rfind(L"scroll ", 0) == 0) {
            try {
                const int n = std::stoi(ToUtf8(TrimWide(value.substr(7))));
                if (n >= 1 && n <= 20) {
                    g_scrollLines = n;
                    g_consoleStatus = L"Скорость прокрутки: " +
                        std::to_wstring(n) + L" строк/щелчок.";
                } else g_consoleStatus = L"Диапазон: 1..20";
            } catch (...) { g_consoleStatus = L"Формат: scroll <1-20>"; }
            return;
        }
        if (lower == L"title") {
            if (g_currentTitle.empty()) {
                g_consoleStatus = L"У страницы нет заголовка.";
            } else {
                UiCopyToClipboard(ToWide(g_currentTitle));
                g_consoleStatus = L"Заголовок скопирован: " +
                    ToWide(g_currentTitle);
            }
            return;
        }
        if (lower == L"links") {
            if (g_currentLinks.empty()) {
                g_consoleStatus = L"На странице нет ссылок.";
            } else {
                std::wstring all;
                for (const auto& l : g_currentLinks) {
                    if (!all.empty()) all += L"\r\n";
                    all += l.url;
                }
                UiCopyToClipboard(all);
                g_consoleStatus = L"Ссылки скопированы: " +
                    std::to_wstring(g_currentLinks.size()) + L" шт.";
            }
            return;
        }
        if (lower == L"mdlink") {
            // Copy hovered/selected link as Markdown [label](url).
            int idx = state.hoveredItem >= 0 ? state.hoveredItem
                                            : state.selectedItem;
            if (idx >= 0 && static_cast<size_t>(idx) < g_currentLinks.size()) {
                const auto& l = g_currentLinks[static_cast<size_t>(idx)];
                UiCopyToClipboard(L"[" + ToWide(l.label) + L"](" +
                                  l.url + L")");
                g_consoleStatus = L"Markdown-ссылка скопирована.";
            } else g_consoleStatus = L"Наведите мышь на ссылку.";
            return;
        }
        state.scrollOffset = 0;
        state.hoveredItem = -1;
        state.selectedItem = -1;
        state.editingField = -1;
        state.findMode = false;
        CloseImageViewer(state);
        if (IsUrlLikeInput(value)) {
            UiOpenUrl(value);
        } else {
            UiSearch(ToUtf8(value));
        }
        state.focus = Focus::Page;
    }

}
