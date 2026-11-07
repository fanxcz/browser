// gui_input.cpp — keyboard/mouse/timer handling for the terminal GUI.
// Mirrors the console UI keymap, adapted to Win32 messages.

#include "gui.h"

#include <shellapi.h>

namespace gui {

static void RememberInput(GuiState& state, const std::wstring& value) {
    if (value.empty()) return;
    if (state.inputHistory.empty() || state.inputHistory.back() != value)
        state.inputHistory.push_back(value);
    if (state.inputHistory.size() > 80)
        state.inputHistory.erase(state.inputHistory.begin());
    state.historyIndex = state.inputHistory.size();
    state.browsingHistory = false;
    state.historyDraft.clear();
}

static void EnsureItemVisible(GuiState& state, int item) {
    if (item < 0 || static_cast<size_t>(item) >= state.itemPageRows.size())
        return;
    const size_t row = static_cast<size_t>(
        state.itemPageRows[static_cast<size_t>(item)]);
    const size_t capacity = static_cast<size_t>(std::max(1, state.pageCapacity));
    if (row < state.scrollOffset) state.scrollOffset = row;
    else if (row >= state.scrollOffset + capacity)
        state.scrollOffset = row - capacity + 1;
}

static void ActivateAction(GuiState& state, NavAction action) {
    state.selectedItem = -1;
    state.hoveredItem = -1;
    state.scrollOffset = 0;
    state.findMode = false;
    state.editingField = -1;
    switch (action) {
    case NavAction::Back: UiBack(); break;
    case NavAction::Forward: UiForward(); break;
    case NavAction::Home:
        UiCancelLoad();
        g_consolePageOpen = false;
        g_consoleStatus.clear();
        state.input.clear();
        state.caret = 0;
        state.inputViewStart = 0;
        state.focus = Focus::Search;
        break;
    case NavAction::Reload: UiReload(); break;
    }
    if (action != NavAction::Home) state.focus = Focus::Page;
    InvalidateGui();
}

static void FindNextOnPage(GuiState& state) {
    std::wstring query = TrimWide(state.input);
    if (query.empty()) query = state.findQuery;
    if (query.empty()) {
        g_consoleStatus = L"Введите текст для поиска на странице.";
        return;
    }
    state.findQuery = query;
    if (g_currentText.empty()) {
        g_consoleStatus = L"На странице нет текста для поиска.";
        return;
    }
    const std::wstring haystack = LowerWide(ToWide(g_currentText));
    const std::wstring needle = LowerWide(query);
    if (needle.empty()) {
        g_consoleStatus = L"Текст не найден.";
        return;
    }
    size_t pos = haystack.find(needle, state.findPos);
    if (pos == std::wstring::npos)
        pos = haystack.find(needle);
    if (pos == std::wstring::npos) {
        g_consoleStatus = L"Текст не найден на странице.";
        state.findPos = 0;
        return;
    }
    state.findPos = pos + needle.size();
    if (state.findPos >= haystack.size()) state.findPos = 0;
    const std::wstring wideText = ToWide(g_currentText);
    const size_t start = pos > 40 ? pos - 40 : 0;
    const size_t end = std::min(wideText.size(), pos + needle.size() + 60);
    std::wstring snippet = wideText.substr(start, end - start);
    if (start > 0) snippet = L"…" + snippet;
    if (end < wideText.size()) snippet += L"…";
    g_consoleStatus = L"Найдено · " + snippet;
    state.focus = Focus::Page;
}

static void OpenSelected(GuiState& state, size_t index) {
    if (index >= g_currentLinks.size()) {
        g_consoleStatus = L"Ссылка не найдена.";
        return;
    }
    const std::wstring target = g_currentLinks[index].url;
    if (target.rfind(L"openfile://", 0) == 0) {
        UiOpenDownloadedFile(target.substr(11));
        return;
    }
    // Binary/file links go straight to the downloader (like a browser).
    static const wchar_t* kFileExts[] = {
        L".zip", L".rar", L".7z", L".tar", L".gz", L".bz2", L".xz",
        L".exe", L".msi", L".dll", L".dmg", L".pkg", L".deb", L".rpm",
        L".apk", L".ipa", L".pdf", L".doc", L".docx", L".xls", L".xlsx",
        L".ppt", L".pptx", L".odt", L".ods", L".mp3", L".mp4", L".avi",
        L".mkv", L".mov", L".wmv", L".flv", L".wav", L".flac", L".ogg",
        L".iso", L".img", L".bin", L".torrent", L".psd", L".ai",
        L".vsix", L".appimage", L".jar", L".war", L".ear", L".xpi",
        L".crx", L".aab", L".run", L".app", L".msu", L".sys",
        L".cab", L".chm", L".epub", L".cbz", L".sketch", L".fig", L".xd",
    };
    std::wstring lowerTarget = target;
    std::transform(lowerTarget.begin(), lowerTarget.end(),
                   lowerTarget.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    const size_t q = lowerTarget.find_first_of(L"?#");
    if (q != std::wstring::npos) lowerTarget.resize(q);
    bool isFile = false;
    for (const wchar_t* ext : kFileExts)
        if (lowerTarget.size() > wcslen(ext) &&
            lowerTarget.compare(lowerTarget.size() - wcslen(ext),
                                std::wstring::npos, ext) == 0) {
            isFile = true;
            break;
        }
    if (isFile) {
        state.selectedItem = -1;
        state.hoveredItem = -1;
        StartDownload(target, "");
        return;
    }
    state.input.clear();
    state.caret = 0;
    state.inputViewStart = 0;
    state.focus = Focus::Page;
    state.selectedItem = -1;
    state.hoveredItem = -1;
    state.scrollOffset = 0;
    UiOpenUrl(target);
}

static void ActivateField(GuiState& state, int itemIndex) {
    const size_t linkCount = g_currentLinks.size();
    if (static_cast<size_t>(itemIndex) < linkCount ||
        state.visibleFields.empty()) {
        g_consoleStatus = L"Элемент не найден.";
        return;
    }
    const size_t fieldPos = static_cast<size_t>(itemIndex) - linkCount;
    if (fieldPos >= state.visibleFields.size()) {
        g_consoleStatus = L"Элемент не найден.";
        return;
    }
    const size_t fieldIdx = state.visibleFields[fieldPos];
    const FormField& field = g_currentFields[fieldIdx];
    switch (field.kind) {
    case FieldKind::Text:
    case FieldKind::Password:
    case FieldKind::Textarea:
        state.editingField = static_cast<int>(fieldIdx);
        state.input = ToWide(field.value);
        state.caret = state.input.size();
        state.inputViewStart = 0;
        state.findMode = false;
        state.focus = Focus::Search;
        g_consoleStatus = L"Enter — сохранить   Esc — отменить";
        return;
    default:
        UiSubmitForm(static_cast<int>(fieldIdx));
        state.focus = Focus::Page;
        return;
    }
}

static void ActivateItem(GuiState& state, int item) {
    if (item < 0 || static_cast<size_t>(item) >= ItemCount(state)) {
        g_consoleStatus = L"Элемент не выбран.";
        return;
    }
    if (static_cast<size_t>(item) < g_currentLinks.size()) {
        OpenSelected(state, static_cast<size_t>(item));
        return;
    }
    const int img = ImageIndexForItem(state, item);
    if (img >= 0) {
        // No in-window image viewer in the GUI yet — open externally.
        const std::wstring url = g_currentImages[static_cast<size_t>(img)].url;
        ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr,
                      SW_SHOWNORMAL);
        g_consoleStatus = L"Изображение открыто в системном приложении.";
        return;
    }
    ActivateField(state, item);
}

// ---------------------------------------------------------------------------
// Omnibox submit — same command set as the console UI.
// ---------------------------------------------------------------------------

void GuiSubmit(GuiState& state) {
    std::wstring value = TrimWide(state.input);
    state.input.clear();
    state.caret = 0;
    state.inputViewStart = 0;
    state.browsingHistory = false;

    if (state.editingField >= 0) {
        if (static_cast<size_t>(state.editingField) < g_currentFields.size()) {
            g_currentFields[static_cast<size_t>(state.editingField)].value =
                ToUtf8(value);
            g_consoleStatus = L"Значение сохранено.";
        }
        state.editingField = -1;
        state.focus = Focus::Page;
        InvalidateGui();
        return;
    }

    if (state.findMode) {
        FindNextOnPage(state);
        InvalidateGui();
        return;
    }

    if (!state.suggestions.empty() && state.suggestIndex >= 0) {
        state.input =
            state.suggestions[static_cast<size_t>(state.suggestIndex)];
        state.suggestIndex = -1;
        state.caret = state.input.size();
        state.inputViewStart = 0;
        state.browsingHistory = false;
        InvalidateGui();
        return;
    }

    if (value.empty()) {
        const int target = state.selectedItem >= 0
            ? state.selectedItem : state.hoveredItem;
        if (g_consolePageOpen && target >= 0)
            ActivateItem(state, target);
        else if (g_consolePageOpen) state.focus = Focus::Page;
        InvalidateGui();
        return;
    }
    RememberInput(state, value);

    std::wstring lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](wchar_t ch) {
                       return static_cast<wchar_t>(std::towlower(ch));
                   });
    if (lower == L"exit" || lower == L"quit") {
        PostMessageW(state.hwnd, WM_CLOSE, 0, 0);
        return;
    }
    if (lower == L"home" || lower == L"главная") {
        UiCancelLoad();
        g_consolePageOpen = false;
        g_consoleStatus.clear();
        state.selectedItem = -1;
        state.hoveredItem = -1;
        state.editingField = -1;
        state.scrollOffset = 0;
        state.focus = Focus::Search;
        InvalidateGui();
        return;
    }
    if (lower == L"back") { ActivateAction(state, NavAction::Back); return; }
    if (lower == L"forward") { ActivateAction(state, NavAction::Forward); return; }
    if (lower == L"reload") { ActivateAction(state, NavAction::Reload); return; }
    if (lower.rfind(L"reload ", 0) == 0) {
        try {
            const int secs = std::stoi(ToUtf8(TrimWide(value.substr(7))));
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
        InvalidateGui();
        return;
    }
    if (lower == L"history") {
        UiShowHistoryPage();
        state.focus = Focus::Page;
        InvalidateGui();
        return;
    }
    if (lower == L"bookmarks") {
        UiShowBookmarksPage();
        state.focus = Focus::Page;
        InvalidateGui();
        return;
    }
    if (lower == L"downloads") {
        UiOpenInternalPage(L"downloads");
        state.focus = Focus::Page;
        InvalidateGui();
        return;
    }
    if (lower.rfind(L"edge ", 0) == 0) {
        UiOpenUrlEdge(TrimWide(value.substr(5)));
        state.focus = Focus::Page;
        InvalidateGui();
        return;
    }
    if (lower == L"stop") { StopDownload(); InvalidateGui(); return; }
    if (lower == L"pause") { UiToggleDownloadPause(); InvalidateGui(); return; }
    if (lower == L"sync" || lower.rfind(L"sync ", 0) == 0) {
        UiSync(TrimWide(value.substr(4)));
        InvalidateGui();
        return;
    }
    if (lower.rfind(L"download ", 0) == 0) {
        const std::wstring rest = TrimWide(value.substr(9));
        const size_t sp = rest.find_first_of(L" \t");
        const std::wstring dlUrl =
            sp == std::wstring::npos ? rest : rest.substr(0, sp);
        const std::string dlName = sp == std::wstring::npos
            ? std::string() : ToUtf8(TrimWide(rest.substr(sp + 1)));
        StartDownload(dlUrl, dlName);
        state.focus = Focus::Page;
        InvalidateGui();
        return;
    }
    if (lower == L"bookmark" || lower == L"bookmark add") {
        UiBookmark();
        InvalidateGui();
        return;
    }
    if (lower == L"clear-history") { UiClearHistory(); InvalidateGui(); return; }
    if (lower.rfind(L"clear-history-older ", 0) == 0) {
        try {
            ClearHistoryOlderThan(std::stoull(value.substr(20)));
        } catch (...) {
            g_consoleStatus = L"Формат: clear-history-older <дней>";
        }
        InvalidateGui();
        return;
    }
    if (lower == L"export") { ExportBookmarksHtml(); InvalidateGui(); return; }
    if (lower.rfind(L"import ", 0) == 0) {
        ImportBookmarksHtml(TrimWide(value.substr(7)));
        InvalidateGui();
        return;
    }
    if (lower == L"bookmarks-clear") {
        UiClearBookmarks();
        InvalidateGui();
        return;
    }
    if (lower == L"cache-clear") { UiClearCache(); InvalidateGui(); return; }
    if (lower == L"help") {
        g_consoleStatus =
            L"Запрос или адрес + Enter. Команды: back, forward, reload, "
            L"home, history, bookmarks, downloads, download <url> [имя], "
            L"edge <url>, stop, pause, sync push|pull, bookmark add, "
            L"clear-history, export, import, bookmarks-clear, cache-clear.";
        InvalidateGui();
        return;
    }
    if (lower.rfind(L"search ", 0) == 0) {
        value = TrimWide(value.substr(7));
    } else if (lower.rfind(L"open ", 0) == 0) {
        value = TrimWide(value.substr(5));
        const bool digits = !value.empty() && std::all_of(
            value.begin(), value.end(),
            [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; });
        if (digits) {
            try {
                const size_t n = std::stoul(value);
                if (n > 0 && n <= g_currentLinks.size())
                    OpenSelected(state, n - 1);
            } catch (...) {
                g_consoleStatus = L"Неверный номер ссылки.";
            }
            InvalidateGui();
            return;
        }
    }

    state.scrollOffset = 0;
    state.hoveredItem = -1;
    state.selectedItem = -1;
    state.editingField = -1;
    state.findMode = false;
    if (IsUrlLike(value)) {
        UiOpenUrl(value);
    } else {
        UiSearch(ToUtf8(value));
    }
    state.focus = Focus::Page;
    InvalidateGui();
}

// ---------------------------------------------------------------------------
// Keyboard
// ---------------------------------------------------------------------------

void GuiHandleKey(GuiState& state, WPARAM vk, bool shift, bool ctrl,
                  bool alt, bool& running) {
    // Ctrl+L — focus omnibox, clear field editing.
    if (ctrl && vk == 'L') {
        state.editingField = -1;
        state.input.clear();
        state.caret = 0;
        state.inputViewStart = 0;
        state.findMode = false;
        state.focus = Focus::Search;
        state.browsingHistory = false;
        InvalidateGui();
        return;
    }
    // Tabs: Ctrl+T new, Ctrl+W close, Ctrl+Tab cycle, Ctrl+1..9 jump.
    if (ctrl && vk == 'T') {
        ApplyView(state, shift ? UiReopenClosedTab(CurrentView(state))
                               : UiNewTab(CurrentView(state)));
        InvalidateGui();
        return;
    }
    if (ctrl && vk == 'W') {
        ApplyView(state, UiCloseTab(CurrentView(state)));
        InvalidateGui();
        return;
    }
    if (ctrl && vk == VK_TAB) {
        ApplyView(state, UiCycleTab(shift ? -1 : 1, CurrentView(state)));
        InvalidateGui();
        return;
    }
    if (ctrl && vk >= '1' && vk <= '9') {
        const size_t count = UiTabCount();
        if (count > 1) {
            const size_t target = vk == '9'
                ? count - 1 : static_cast<size_t>(vk - '1');
            if (target < count)
                ApplyView(state, UiSwitchTabTo(target, CurrentView(state)));
        }
        InvalidateGui();
        return;
    }
    if (ctrl && vk == 'F') {
        if (g_consolePageOpen && !g_currentText.empty()) {
            state.findMode = true;
            state.findPos = 0;
            state.findQuery.clear();
            state.input.clear();
            state.caret = 0;
            state.inputViewStart = 0;
            state.focus = Focus::Search;
        } else {
            g_consoleStatus = L"Сначала откройте страницу с текстом.";
        }
        InvalidateGui();
        return;
    }
    if (ctrl && vk == 'H') {
        UiShowHistoryPage();
        state.selectedItem = -1;
        state.hoveredItem = -1;
        state.scrollOffset = 0;
        state.focus = Focus::Page;
        InvalidateGui();
        return;
    }
    if (ctrl && vk == 'B') {
        UiShowBookmarksPage();
        state.selectedItem = -1;
        state.hoveredItem = -1;
        state.scrollOffset = 0;
        state.focus = Focus::Page;
        InvalidateGui();
        return;
    }
    if (ctrl && vk == 'J') {
        UiOpenInternalPage(L"downloads");
        state.selectedItem = -1;
        state.hoveredItem = -1;
        state.scrollOffset = 0;
        state.focus = Focus::Page;
        InvalidateGui();
        return;
    }
    if (ctrl && shift && vk == 'O') { UiOpenExternalBrowser(); InvalidateGui(); return; }
    if (ctrl && shift && vk == 'L') {
        // Toggle the GUI palette between dark and light.
        extern Palette g_palLight;
        static bool light = false;
        light = !light;
        g_pal = light ? g_palLight : Palette{
            RGB(13, 17, 23), RGB(201, 209, 217), RGB(230, 237, 243),
            RGB(125, 133, 144), RGB(94, 234, 212), RGB(121, 192, 255),
            RGB(35, 58, 82), RGB(240, 248, 255), RGB(33, 38, 48),
            RGB(248, 81, 73)};
        g_consoleStatus = light ? L"Светлая тема." : L"Тёмная тема.";
        InvalidateGui();
        return;
    }
    if (ctrl && shift && vk == 'E' && !g_currentUrl.empty()) {
        UiOpenUrlEdge(g_currentUrl);
        InvalidateGui();
        return;
    }
    if (ctrl && vk == 'C') {
        std::wstring toCopy = g_currentUrl;
        if (state.selectedItem >= 0) {
            if (static_cast<size_t>(state.selectedItem) < g_currentLinks.size())
                toCopy = g_currentLinks[
                    static_cast<size_t>(state.selectedItem)].url;
            else {
                const int img = ImageIndexForItem(state, state.selectedItem);
                if (img >= 0)
                    toCopy = g_currentImages[
                        static_cast<size_t>(img)].url;
            }
        }
        g_consoleStatus = !toCopy.empty() && UiCopyToClipboard(toCopy)
            ? L"Ссылка скопирована в буфер обмена." : L"Нечего копировать.";
        InvalidateGui();
        return;
    }
    if (ctrl && vk == 'V' && state.focus == Focus::Search) {
        const std::wstring clip = UiGetClipboardText();
        if (!clip.empty()) {
            state.input.insert(state.caret, clip);
            state.caret += clip.size();
            state.browsingHistory = false;
            state.suggestIndex = -1;
        }
        InvalidateGui();
        return;
    }
    if ((ctrl && vk == 'R') || vk == VK_F5) {
        if (g_consolePageOpen) ActivateAction(state, NavAction::Reload);
        return;
    }
    if (alt && vk == VK_LEFT) { ActivateAction(state, NavAction::Back); return; }
    if (alt && vk == VK_RIGHT) { ActivateAction(state, NavAction::Forward); return; }
    if (vk == VK_F6) {
        state.focus = Focus::Search;
        state.caret = state.input.size();
        state.browsingHistory = false;
        InvalidateGui();
        return;
    }
    if (ctrl && vk == 'D') { UiBookmark(); InvalidateGui(); return; }
    if (ctrl && vk == 'S') { UiSavePage(); InvalidateGui(); return; }
    if (vk == VK_BACK && state.focus == Focus::Page && !state.findMode) {
        ActivateAction(state, NavAction::Back);
        return;
    }
    if (vk == VK_TAB) {
        if (state.focus == Focus::Search && state.suggestIndex >= 0 &&
            state.suggestIndex <
                static_cast<int>(state.suggestions.size())) {
            state.input =
                state.suggestions[static_cast<size_t>(state.suggestIndex)];
            state.suggestIndex = -1;
            state.caret = state.input.size();
            state.inputViewStart = 0;
            state.browsingHistory = false;
            InvalidateGui();
            return;
        }
        state.focus = state.focus == Focus::Search ? Focus::Page
                                                 : Focus::Search;
        const size_t items = ItemCount(state);
        if (state.focus == Focus::Page && state.selectedItem < 0 &&
            items > 0)
            state.selectedItem = shift ? static_cast<int>(items - 1) : 0;
        else if (state.focus == Focus::Search)
            state.selectedItem = -1;
        InvalidateGui();
        return;
    }
    if (vk == VK_RETURN && ctrl) {
        if (state.editingField >= 0) {
            if (static_cast<size_t>(state.editingField) < g_currentFields.size())
                g_currentFields[static_cast<size_t>(state.editingField)]
                    .value = ToUtf8(TrimWide(state.input));
            const int fi = state.editingField;
            state.editingField = -1;
            state.input.clear();
            state.caret = 0;
            state.inputViewStart = 0;
            state.focus = Focus::Page;
            UiSubmitFormForField(fi);
        } else {
            const int target = state.selectedItem >= 0
                ? state.selectedItem : state.hoveredItem;
            const int fi = FieldIndexForItem(state, target);
            if (fi >= 0) UiSubmitFormForField(fi);
            else GuiSubmit(state);
        }
        InvalidateGui();
        return;
    }
    if (vk == VK_RETURN) {
        GuiSubmit(state);
        return;
    }
    if (vk == VK_ESCAPE) {
        if (state.editingField >= 0) {
            state.editingField = -1;
            state.input.clear();
            state.caret = 0;
            state.inputViewStart = 0;
            state.focus = Focus::Page;
        } else if (state.findMode) {
            state.findMode = false;
            state.focus = Focus::Page;
        } else if (!state.input.empty()) {
            state.input.clear();
            state.caret = 0;
            state.inputViewStart = 0;
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
        InvalidateGui();
        return;
    }

    if (state.focus == Focus::Page &&
        (vk == VK_UP || vk == VK_DOWN)) {
        const size_t items = ItemCount(state);
        if (items > 0) {
            const int maxIndex = static_cast<int>(items - 1);
            if (state.selectedItem < 0)
                state.selectedItem = vk == VK_DOWN ? 0 : maxIndex;
            else
                state.selectedItem = std::clamp(
                    state.selectedItem + (vk == VK_DOWN ? 1 : -1), 0,
                    maxIndex);
            EnsureItemVisible(state, state.selectedItem);
        } else if (state.pageLines.size() >
                   static_cast<size_t>(state.pageCapacity)) {
            const size_t maxOffset = state.pageLines.size() -
                static_cast<size_t>(state.pageCapacity);
            if (vk == VK_DOWN)
                state.scrollOffset =
                    std::min(maxOffset, state.scrollOffset + 1);
            else if (state.scrollOffset > 0) --state.scrollOffset;
        }
        InvalidateGui();
        return;
    }
    // Omnibox Up/Down: suggestions dropdown first, then input history.
    if (state.focus == Focus::Search &&
        (vk == VK_UP || vk == VK_DOWN) && !state.suggestions.empty()) {
        const int n = static_cast<int>(state.suggestions.size());
        if (state.suggestIndex < 0)
            state.suggestIndex = vk == VK_DOWN ? 0 : n - 1;
        else
            state.suggestIndex = (state.suggestIndex +
                (vk == VK_DOWN ? 1 : -1) + n) % n;
        InvalidateGui();
        return;
    }
    if (state.focus == Focus::Search &&
        (vk == VK_UP || vk == VK_DOWN)) {
        if (!state.inputHistory.empty()) {
            if (!state.browsingHistory) {
                state.historyDraft = state.input;
                state.historyIndex = state.inputHistory.size();
                state.browsingHistory = true;
            }
            if (vk == VK_UP) {
                if (state.historyIndex > 0) --state.historyIndex;
            } else if (state.historyIndex < state.inputHistory.size()) {
                ++state.historyIndex;
            }
            state.input = state.historyIndex < state.inputHistory.size()
                ? state.inputHistory[state.historyIndex]
                : state.historyDraft;
            state.caret = state.input.size();
            InvalidateGui();
            return;
        }
    }
    if (vk == VK_NEXT || vk == VK_PRIOR) {
        const size_t step =
            static_cast<size_t>(std::max(1, state.pageCapacity - 2));
        const size_t maxOffset = state.pageLines.size() >
                                     static_cast<size_t>(state.pageCapacity)
            ? state.pageLines.size() -
                static_cast<size_t>(state.pageCapacity) : 0;
        if (vk == VK_NEXT)
            state.scrollOffset = std::min(maxOffset, state.scrollOffset + step);
        else
            state.scrollOffset =
                state.scrollOffset > step ? state.scrollOffset - step : 0;
        state.selectedItem = -1;
        state.hoveredItem = -1;
        state.focus = Focus::Page;
        InvalidateGui();
        return;
    }
    if (state.focus == Focus::Page && vk == VK_HOME) {
        state.scrollOffset = 0;
        state.selectedItem = -1;
        state.hoveredItem = -1;
        InvalidateGui();
        return;
    }
    if (state.focus == Focus::Page && vk == VK_END) {
        state.scrollOffset = state.pageLines.size() >
                                 static_cast<size_t>(state.pageCapacity)
            ? state.pageLines.size() -
                static_cast<size_t>(state.pageCapacity) : 0;
        state.selectedItem = -1;
        state.hoveredItem = -1;
        InvalidateGui();
        return;
    }

    // Page-focus command keys (single letters).
    if (state.focus == Focus::Page) {
        // Map VK to a lower-case wchar for the letter commands.
        wchar_t ch = 0;
        if (vk >= 'A' && vk <= 'Z')
            ch = static_cast<wchar_t>(static_cast<int>(vk) +
                                      (shift ? 0 : 32));
        else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
            ch = static_cast<wchar_t>(L'0' + (vk - VK_NUMPAD0));

        const int maxItem = static_cast<int>(ItemCount(state)) - 1;
        const auto moveItems = [&](int delta) {
            if (maxItem < 0) return;
            if (state.selectedItem < 0)
                state.selectedItem = delta > 0 ? 0 : maxItem;
            else
                state.selectedItem = std::clamp(state.selectedItem + delta,
                                                0, maxItem);
            EnsureItemVisible(state, state.selectedItem);
        };
        const auto pageScroll = [&](int direction) {
            const size_t step =
                static_cast<size_t>(std::max(1, state.pageCapacity));
            const size_t maxOffset = state.pageLines.size() >
                static_cast<size_t>(state.pageCapacity)
                ? state.pageLines.size() -
                    static_cast<size_t>(state.pageCapacity) : 0;
            if (direction > 0)
                state.scrollOffset =
                    std::min(maxOffset, state.scrollOffset + step);
            else
                state.scrollOffset = state.scrollOffset > step
                    ? state.scrollOffset - step : 0;
            state.selectedItem = -1;
            state.hoveredItem = -1;
        };

        bool handled = true;
        switch (vk) {
        case 'J': moveItems(1); break;
        case 'K': moveItems(-1); break;
        case VK_SPACE: pageScroll(shift ? -1 : 1); break;
        case 'O':
            state.input.clear();
            state.caret = 0;
            state.inputViewStart = 0;
            state.findMode = false;
            state.focus = Focus::Search;
            state.browsingHistory = false;
            break;
        case 'B': ActivateAction(state, NavAction::Back); return;
        case 'F': ActivateAction(state, NavAction::Forward); return;
        case 'R': ActivateAction(state, NavAction::Reload); return;
        case 'Y':
            if (!g_currentUrl.empty() && UiCopyToClipboard(g_currentUrl))
                g_consoleStatus = L"URL скопирован в буфер обмена.";
            else
                g_consoleStatus = L"Не удалось скопировать.";
            break;
        case 'P':
            if (g_currentUrl == L"browser://downloads" ||
                g_downloadRunning.load() || PendingDownloadCount() > 0)
                UiToggleDownloadPause();
            else handled = false;
            break;
        case 'E':
            if (g_currentUrl == L"browser://downloads")
                UiOpenDownloadsFolder();
            else handled = false;
            break;
        case 'X':
            if (g_currentUrl == L"browser://downloads") {
                if (state.selectedItem >= 0 &&
                    static_cast<size_t>(state.selectedItem) <
                        g_currentLinks.size()) {
                    const std::wstring u = g_currentLinks[
                        static_cast<size_t>(state.selectedItem)].url;
                    if (u.rfind(L"openfile://", 0) == 0)
                        UiCancelDownloadByName(u.substr(11));
                    else
                        g_consoleStatus = L"Выберите файл в списке загрузок.";
                } else {
                    g_consoleStatus = L"Сначала выберите файл (j/k).";
                }
            } else handled = false;
            break;
        case 'V':
            if (g_currentUrl == L"browser://downloads") {
                if (state.selectedItem >= 0 &&
                    static_cast<size_t>(state.selectedItem) <
                        g_currentLinks.size()) {
                    const std::wstring u = g_currentLinks[
                        static_cast<size_t>(state.selectedItem)].url;
                    if (u.rfind(L"openfile://", 0) == 0)
                        UiVerifyDownloadedFile(u.substr(11));
                    else
                        g_consoleStatus = L"Выберите файл в списке загрузок.";
                } else {
                    g_consoleStatus = L"Сначала выберите файл (j/k).";
                }
            } else handled = false;
            break;
        case 'T':
        case 'D': {
            if (state.selectedItem < 0) {
                if (vk == 'D' && !g_currentUrl.empty() &&
                    g_currentUrl.rfind(L"http", 0) == 0) {
                    StartDownload(g_currentUrl, "");
                } else {
                    g_consoleStatus = vk == 'T'
                        ? L"Сначала выберите элемент (j/k)."
                        : L"Сначала выберите ссылку или изображение (j/k).";
                }
                break;
            }
            std::wstring target;
            if (static_cast<size_t>(state.selectedItem) < g_currentLinks.size())
                target = g_currentLinks[
                    static_cast<size_t>(state.selectedItem)].url;
            else {
                const int img = ImageIndexForItem(state, state.selectedItem);
                if (img >= 0)
                    target = g_currentImages[
                        static_cast<size_t>(img)].url;
            }
            if (target.empty()) {
                g_consoleStatus = L"У выбранного элемента нет адреса.";
                break;
            }
            if (target.rfind(L"openfile://", 0) == 0) {
                UiOpenDownloadedFile(target.substr(11));
                break;
            }
            if (vk == 'T')
                ApplyView(state, UiOpenInNewTab(target, CurrentView(state)));
            else
                StartDownload(target, "");
            break;
        }
        case 'G':
            if (shift) {
                state.scrollOffset = state.pageLines.size() >
                    static_cast<size_t>(state.pageCapacity)
                    ? state.pageLines.size() -
                        static_cast<size_t>(state.pageCapacity) : 0;
            } else {
                state.scrollOffset = 0;
            }
            state.selectedItem = -1;
            state.hoveredItem = -1;
            break;
        case VK_OEM_2: // '/' — quick find
            if (!g_currentText.empty()) {
                state.findMode = true;
                state.findPos = 0;
                state.findQuery.clear();
                state.input.clear();
                state.caret = 0;
                state.inputViewStart = 0;
                state.focus = Focus::Search;
            } else {
                g_consoleStatus = L"На странице нет текста для поиска.";
            }
            handled = true;
            break;
        default:
            handled = false;
            // Digits: quick-select numbered links.
            if ((vk >= '1' && vk <= '9') || vk == '0') {
                const int index = vk == '0' ? 9
                    : static_cast<int>(vk) - '1';
                if (index < static_cast<int>(g_currentLinks.size())) {
                    state.selectedItem = index;
                    EnsureItemVisible(state, index);
                    g_consoleStatus = L"Enter — открыть ссылку " +
                        std::to_wstring(index + 1) + L".";
                    handled = true;
                }
            }
            break;
        }
        if (handled) { InvalidateGui(); return; }
    }

    // Omnibox editing keys.
    if (state.focus == Focus::Search) {
        if (vk == VK_LEFT) {
            if (state.caret > 0) --state.caret;
            state.browsingHistory = false;
            InvalidateGui();
            return;
        }
        if (vk == VK_RIGHT) {
            if (state.caret < state.input.size()) ++state.caret;
            state.browsingHistory = false;
            InvalidateGui();
            return;
        }
        if (vk == VK_HOME) { state.caret = 0; InvalidateGui(); return; }
        if (vk == VK_END) {
            state.caret = state.input.size();
            InvalidateGui();
            return;
        }
        if (vk == VK_BACK) {
            if (state.caret > 0) {
                size_t eraseAt = state.caret - 1;
                if (eraseAt > 0 &&
                    state.input[eraseAt] >= 0xDC00 &&
                    state.input[eraseAt] <= 0xDFFF &&
                    state.input[eraseAt - 1] >= 0xD800 &&
                    state.input[eraseAt - 1] <= 0xDBFF)
                    --eraseAt;
                state.input.erase(eraseAt, state.caret - eraseAt);
                state.caret = eraseAt;
            }
            state.browsingHistory = false;
            InvalidateGui();
            return;
        }
        if (vk == VK_DELETE) {
            if (state.caret < state.input.size())
                state.input.erase(state.caret, 1);
            state.browsingHistory = false;
            InvalidateGui();
            return;
        }
    }
}

// WM_CHAR path: printable text into the omnibox.
void GuiHandleChar(GuiState& state, wchar_t ch) {
    if (state.focus != Focus::Search) {
        // Typing on a page refocuses the omnibox (like the console UI).
        if (ch >= L' ' && !state.findMode) {
            state.focus = Focus::Search;
            state.input.clear();
            state.caret = 0;
            state.inputViewStart = 0;
        }
        if (state.focus != Focus::Search) return;
    }
    if (ch >= L' ' && ch != 0) {
        state.input.insert(state.caret, 1, ch);
        ++state.caret;
        state.browsingHistory = false;
        InvalidateGui();
    }
}

// ---------------------------------------------------------------------------
// Mouse (cell coordinates)
// ---------------------------------------------------------------------------

void GuiHandleMouseMove(GuiState& state, int x, int y) {
    int hoverItem = -1;
    for (const auto& row : state.itemRows)
        if (y == row.row) { hoverItem = row.item; break; }
    int hoverSuggest = -1;
    for (const auto& hit : state.suggestHits)
        if (hit.row == y && x >= hit.left && x <= hit.right) {
            hoverSuggest = hit.index; break;
        }
    if (hoverSuggest == -1)
        for (const auto& hit : state.bookmarkHits)
            if (hit.row == y && x >= hit.left && x <= hit.right) {
                hoverSuggest = hit.index; break;
            }
    if (hoverItem != state.hoveredItem || hoverSuggest != state.hoverSuggest) {
        state.hoveredItem = hoverItem;
        state.hoverSuggest = hoverSuggest;
        if (hoverSuggest >= 0) state.suggestIndex = hoverSuggest;
        InvalidateGui();
    }
}

void GuiHandleClick(GuiState& state, int x, int y) {
    // Suggestion dropdown.
    for (const auto& hit : state.suggestHits) {
        if (hit.row == y && x >= hit.left && x <= hit.right &&
            hit.index < static_cast<int>(state.suggestions.size())) {
            state.input = state.suggestions[static_cast<size_t>(hit.index)];
            state.suggestIndex = -1;
            state.caret = state.input.size();
            state.inputViewStart = 0;
            state.browsingHistory = false;
            state.focus = Focus::Search;
            InvalidateGui();
            return;
        }
    }
    // Home-page bookmarks.
    if (!state.bookmarkHits.empty()) {
        const auto bookmarks = GetBookmarksCached();
        for (const auto& hit : state.bookmarkHits) {
            if (hit.row == y && x >= hit.left && x <= hit.right &&
                hit.index < static_cast<int>(bookmarks.size())) {
                UiOpenUrl(bookmarks[static_cast<size_t>(hit.index)]);
                InvalidateGui();
                return;
            }
        }
    }
    // Tab strip (row 0): close first, then new, then switch.
    if (y == 0) {
        for (const auto& hit : state.tabHits) {
            if (hit.isClose && x >= hit.left && x <= hit.right) {
                ApplyView(state, UiCloseTabIndex(
                    static_cast<size_t>(hit.index), CurrentView(state)));
                InvalidateGui();
                return;
            }
        }
        for (const auto& hit : state.tabHits) {
            if (hit.isNew && x >= hit.left && x <= hit.right) {
                ApplyView(state, UiNewTab(CurrentView(state)));
                InvalidateGui();
                return;
            }
            if (!hit.isNew && !hit.isClose &&
                x >= hit.left && x <= hit.right) {
                ApplyView(state, UiSwitchTabTo(
                    static_cast<size_t>(hit.index), CurrentView(state)));
                InvalidateGui();
                return;
            }
        }
    }
    // Nav buttons (row 1).
    for (const auto& hit : state.navHits) {
        if (hit.row == y && x >= hit.left && x <= hit.right) {
            ActivateAction(state,
                static_cast<NavAction>(hit.index));
            return;
        }
    }
    // Search button.
    if (state.searchButton.Contains(x, y)) {
        GuiSubmit(state);
        return;
    }
    // Omnibox area → focus + caret position.
    if (state.searchBox.Contains(x, y)) {
        state.focus = Focus::Search;
        state.hoveredItem = -1;
        InvalidateGui();
        return;
    }
    // Item rows → select + activate (same as console click).
    for (const auto& row : state.itemRows) {
        if (y == row.row) {
            state.selectedItem = row.item;
            state.hoveredItem = row.item;
            state.focus = Focus::Page;
            ActivateItem(state, row.item);
            InvalidateGui();
            return;
        }
    }
    if (g_consolePageOpen && y >= 6 && y < state.rows - 2)
        state.focus = Focus::Page;
    InvalidateGui();
}

void GuiHandleWheel(GuiState& state, int delta) {
    const size_t notches =
        static_cast<size_t>(std::max(1, std::abs(delta) / WHEEL_DELTA));
    const size_t step = notches * 3;
    const size_t maxOffset = state.pageLines.size() >
                                 static_cast<size_t>(state.pageCapacity)
        ? state.pageLines.size() -
            static_cast<size_t>(state.pageCapacity) : 0;
    if (delta > 0)
        state.scrollOffset =
            state.scrollOffset > step ? state.scrollOffset - step : 0;
    else if (delta < 0)
        state.scrollOffset = std::min(maxOffset, state.scrollOffset + step);
    state.selectedItem = -1;
    state.hoveredItem = -1;
    state.focus = Focus::Page;
    InvalidateGui();
}

// ---------------------------------------------------------------------------
// Timer tick: commit async loads, refresh animation, meta refresh.
// ---------------------------------------------------------------------------

void GuiTick(GuiState& state) {
    state.caretOn = !state.caretOn;

    const bool loadChanged = PollAsyncLoad();

    static std::wstring lastUrl;
    if (g_currentUrl != lastUrl) {
        lastUrl = g_currentUrl;
        state.selectedItem = -1;
        state.hoveredItem = -1;
        state.scrollOffset = 0;
        state.findMode = false;
        state.findPos = 0;
        state.findQuery.clear();
        state.editingField = -1;
        state.refreshScanUrl = g_currentUrl;
        state.refreshSeconds = g_refreshSeconds;
        state.refreshInterval = 0;
        state.refreshUrl = g_refreshUrl;
        state.refreshAt = Clock::now() +
            std::chrono::seconds(std::max(1, g_refreshSeconds));
        const std::wstring title = g_currentTitle.empty()
            ? g_currentUrl : L"Browser — " + ToWide(g_currentTitle);
        SetWindowTextW(state.hwnd, title.c_str());
    }

    // Rebuild the wrapped page lines only when the visible content may
    // have changed — wrapping is too expensive to run every tick.
    static std::wstring linesUrl;
    static size_t linesTextSize = 0, linesLinks = 0, linesFields = 0,
                  linesImages = 0, linesInputHash = 0;
    static int linesCols = 0, linesRows = 0;
    static bool linesFindMode = false;
    const size_t fieldsCount = [] {
        size_t n = 0;
        for (const auto& f : g_currentFields)
            if (f.kind != FieldKind::Hidden) ++n;
        return n;
    }();
    const size_t inputHash = std::hash<std::wstring>{}(state.input);
    const bool linesDirty =
        g_currentUrl != linesUrl ||
        g_currentText.size() != linesTextSize ||
        g_currentLinks.size() != linesLinks ||
        fieldsCount != linesFields ||
        g_currentImages.size() != linesImages ||
        inputHash != linesInputHash ||
        state.cols != linesCols || state.rows != linesRows ||
        state.findMode != linesFindMode ||
        state.pageLines.empty();
    if (linesDirty) {
        linesUrl = g_currentUrl;
        linesTextSize = g_currentText.size();
        linesLinks = g_currentLinks.size();
        linesFields = fieldsCount;
        linesImages = g_currentImages.size();
        linesInputHash = inputHash;
        linesCols = state.cols;
        linesRows = state.rows;
        linesFindMode = state.findMode;
        BuildPageLines(state);
    }

    if (state.refreshSeconds > 0 && Clock::now() >= state.refreshAt &&
        g_consolePageOpen && !g_loadRunning.load()) {
        if (state.refreshInterval > 0) {
            state.refreshAt = Clock::now() +
                std::chrono::seconds(state.refreshInterval);
            UiReload();
        } else {
            state.refreshSeconds = 0;
            if (state.refreshUrl.empty()) {
                UiReload();
            } else {
                const std::wstring target = ResolveRedirectUrl(
                    g_currentUrl, ToWide(state.refreshUrl));
                if (!target.empty()) UiOpenUrl(target);
            }
        }
    }

    InvalidateGui();
    (void)loadChanged;
}

} // namespace gui
