#include "browser.h"
#include "wvhost.h"

#include <array>
#include <cwchar>
#include <fstream>
#include <functional>
#include <vector>

namespace console_ui {
    // Console palette — swappable for the light theme (Ctrl+Shift+L).
    struct Palette {
        WORD text, muted, accent, bright, selected, hover;
    };
    WORD kText, kMuted, kAccent, kBright, kSelected, kHover;

    const Palette kDarkPalette{
        FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE,   // text
        FOREGROUND_INTENSITY,                                  // muted
        FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY, // accent
        FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE |
            FOREGROUND_INTENSITY,                              // bright
        (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE) |
            BACKGROUND_BLUE | BACKGROUND_GREEN,                 // selected
        (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE) |
            BACKGROUND_BLUE                                     // hover
    };
    // Light theme: black text everywhere on a light-gray cell background.
    const WORD kLightBg = BACKGROUND_RED | BACKGROUND_GREEN |
                          BACKGROUND_BLUE;                      // gray
    const Palette kLightPalette{
        0,                                                    // text (black)
        0,                                                    // muted (black)
        0,                                                    // accent (black)
        0,                                                    // bright (black)
        BACKGROUND_BLUE,                                      // selected
        BACKGROUND_GREEN | BACKGROUND_INTENSITY                // hover
    };
    bool g_lightTheme = false;

    void ApplyPalette() {
        const Palette& p = g_lightTheme ? kLightPalette : kDarkPalette;
        kText = p.text; kMuted = p.muted; kAccent = p.accent;
        kBright = p.bright; kSelected = p.selected; kHover = p.hover;
        WORD* fg[4] = {&kText, &kMuted, &kAccent, &kBright};
        if (g_lightTheme)
            for (WORD* c : fg) *c |= kLightBg;
        else
            for (WORD* c : fg) *c &= static_cast<WORD>(~kLightBg);
    }

    // ---- Theme packs -------------------------------------------------------
    // Built-ins first, then data/themes/*.json:
    //   {"name":"Ocean","mode":"dark","accent":"#58a6ff",
    //    "density":"compact|normal|spacious"}
    struct ThemePack {
        std::wstring name;
        bool light = false;
        WORD accent = 0;   // 0 = palette default
        int density = 1;   // 0 compact, 1 normal, 2 spacious
    };
    std::vector<ThemePack> g_themes;
    size_t g_themeIndex = 0;
    int g_density = 1;

    // Map #RRGGBB to the nearest of the 16 console colors.
    WORD ConsoleColorFromHex(const std::string& hex) {
        if (hex.size() != 7 || hex[0] != '#') return 0;
        auto hv = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        const int r = hv(hex[1]) * 16 + hv(hex[2]);
        const int g = hv(hex[3]) * 16 + hv(hex[4]);
        const int b = hv(hex[5]) * 16 + hv(hex[6]);
        if (r < 0 || g < 0 || b < 0) return 0;
        // Standard EGA-ish palette used by conhost.
        static const WORD kCols[16] = {
            0, FOREGROUND_BLUE, FOREGROUND_GREEN,
            FOREGROUND_BLUE | FOREGROUND_GREEN,
            FOREGROUND_RED, FOREGROUND_RED | FOREGROUND_BLUE,
            FOREGROUND_RED | FOREGROUND_GREEN,
            FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE,
            FOREGROUND_INTENSITY,
            FOREGROUND_BLUE | FOREGROUND_INTENSITY,
            FOREGROUND_GREEN | FOREGROUND_INTENSITY,
            FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY,
            FOREGROUND_RED | FOREGROUND_INTENSITY,
            FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY,
            FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY,
            FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE |
                FOREGROUND_INTENSITY };
        static const int kRgb[16][3] = {
            {0,0,0},{0,0,128},{0,128,0},{0,128,128},
            {128,0,0},{128,0,128},{128,128,0},{192,192,192},
            {128,128,128},{0,0,255},{0,255,0},{0,255,255},
            {255,0,0},{255,0,255},{255,255,0},{255,255,255} };
        int best = 7, bestD = INT32_MAX;
        for (int i = 0; i < 16; ++i) {
            const int dr = r - kRgb[i][0], dg = g - kRgb[i][1],
                      db = b - kRgb[i][2];
            const int d = dr * dr + dg * dg + db * db;
            if (d < bestD) { bestD = d; best = i; }
        }
        return kCols[best];
    }

    // Tiny JSON field reader — enough for our flat theme files.
    std::string JsonString(const std::string& js, const char* key) {
        const std::string pat = std::string("\"") + key + "\"";
        const size_t p = js.find(pat);
        if (p == std::string::npos) return {};
        const size_t colon = js.find(':', p + pat.size());
        const size_t q1 = js.find('"', colon + 1);
        if (q1 == std::string::npos) return {};
        const size_t q2 = js.find('"', q1 + 1);
        if (q2 == std::string::npos) return {};
        return js.substr(q1 + 1, q2 - q1 - 1);
    }

    void LoadThemePacks() {
        g_themes.clear();
        g_themes.push_back({L"Тёмная", false, 0, 1});
        g_themes.push_back({L"Светлая", true, 0, 1});
        std::error_code ec;
        const fs::path dir = DataDirPath() / L"themes";
        if (fs::exists(dir, ec)) {
            for (const auto& e :
                 fs::directory_iterator(dir, ec)) {
                if (e.path().extension() != L".json") continue;
                std::ifstream in(e.path(), std::ios::binary);
                std::string js((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
                ThemePack p;
                p.name = ToWide(JsonString(js, "name"));
                if (p.name.empty())
                    p.name = ToWide(e.path().stem().string());
                const std::string mode = JsonString(js, "mode");
                p.light = mode == "light";
                p.accent = ConsoleColorFromHex(JsonString(js, "accent"));
                const std::string den = JsonString(js, "density");
                p.density = den == "compact" ? 0
                            : den == "spacious" ? 2 : 1;
                g_themes.push_back(std::move(p));
            }
        }
        if (g_themes.empty())
            g_themes.push_back({L"Тёмная", false, 0, 1});
    }

    void ApplyTheme(size_t index) {
        if (g_themes.empty()) return;
        g_themeIndex = index % g_themes.size();
        const ThemePack& p = g_themes[g_themeIndex];
        g_lightTheme = p.light;
        ApplyPalette();
        if (p.accent && !p.light) kAccent = p.accent;
        g_density = p.density;
    }

    void LoadThemeSetting() {
        std::wstring wanted = L"dark";
        {
            std::wifstream in(DataDirPath() / L"settings.txt");
            std::wstring line;
            while (std::getline(in, line)) {
                if (line.rfind(L"theme=", 0) == 0) {
                    wanted = line.substr(6);
                    break;
                }
            }
        }
        for (size_t i = 0; i < g_themes.size(); ++i) {
            const std::string nm = ToUtf8(g_themes[i].name);
            if (wanted == L"light" && g_themes[i].light &&
                g_themes[i].accent == 0) { ApplyTheme(i); return; }
            if (wanted == L"dark" && !g_themes[i].light &&
                g_themes[i].accent == 0) { ApplyTheme(i); return; }
            if (ToWide(nm) == wanted) { ApplyTheme(i); return; }
        }
        ApplyTheme(0);
    }

    void SaveThemeSetting() {
        // Rewrite settings.txt preserving other keys (e.g. downloads=).
        std::vector<std::wstring> lines;
        {
            std::wifstream in(DataDirPath() / L"settings.txt");
            std::wstring line;
            while (std::getline(in, line))
                if (line.rfind(L"theme=", 0) != 0) lines.push_back(line);
        }
        const std::string nm = ToUtf8(g_themes[g_themeIndex].name);
        lines.push_back(L"theme=" + ToWide(nm));
        std::wofstream out(DataDirPath() / L"settings.txt",
                           std::ios::trunc);
        for (const auto& l : lines) out << l << L'\n';
    }

    enum class Focus { Search, Page };
    enum class NavAction { Back, Forward, Home, Reload, Settings };

    // Settings dropdown (top-right "⚙" button) — mouse + keyboard.
    enum class MenuAction {
        Theme, PageInfo, ViewSource, CopyUrl, External,
        NewTab, CloseTab, NextTab, PrevTab,
        Downloads, HistoryPg, BookmarksPg,
        ClearHistory, ClearBookmarks, ClearCache, Export,
        SyncPush, SyncPull, SyncStatus,
        WebOff, Help,
    };
    struct MenuItem { std::wstring label; MenuAction action; };

    const std::vector<MenuItem>& SettingsMenuItems() {
        static const std::vector<MenuItem> items = {
            {L"Переключить тему (светлая/тёмная)", MenuAction::Theme},
            {L"Информация о странице",             MenuAction::PageInfo},
            {L"Исходный код страницы",             MenuAction::ViewSource},
            {L"Копировать URL страницы",           MenuAction::CopyUrl},
            {L"Открыть во внешнем браузере",       MenuAction::External},
            {L"Новая вкладка",                     MenuAction::NewTab},
            {L"Закрыть вкладку",                   MenuAction::CloseTab},
            {L"Следующая вкладка",                 MenuAction::NextTab},
            {L"Предыдущая вкладка",                MenuAction::PrevTab},
            {L"Загрузки",                          MenuAction::Downloads},
            {L"История",                           MenuAction::HistoryPg},
            {L"Закладки",                          MenuAction::BookmarksPg},
            {L"Очистить историю",                  MenuAction::ClearHistory},
            {L"Очистить закладки",                 MenuAction::ClearBookmarks},
            {L"Очистить кэш",                      MenuAction::ClearCache},
            {L"Экспорт истории и закладок (HTML)", MenuAction::Export},
            {L"Синхронизация: отправить (push)",   MenuAction::SyncPush},
            {L"Синхронизация: получить (pull)",    MenuAction::SyncPull},
            {L"Синхронизация: статус",             MenuAction::SyncStatus},
            {L"Скрыть WebView2-окно",              MenuAction::WebOff},
            {L"Справка",                           MenuAction::Help},
        };
        return items;
    }

    struct ItemRow { int row = 0; int item = 0; };       // link or form field
    struct ActionHit { int row = 0; int left = 0; int right = 0; NavAction action = NavAction::Home; };
    struct TabHit { int left = 0; int right = 0; size_t index = 0; bool isNew = false; bool isClose = false; };
    struct SuggestHit { int row = 0; int left = 0; int right = 0; int index = 0; };
    struct State {
        std::wstring input;
        size_t caret = 0;
        size_t inputViewStart = 0;
        Focus focus = Focus::Search;
        int selectedItem = -1;      // 0..links-1 = link; links.. = visible field
        int hoveredItem = -1;
        int hoveredAction = -1;
        int hoveredSearchButton = 0;
        int searchRow = 0;
        int searchLeft = 0;
        int searchRight = 0;
        int searchBoxLeft = 0;
        int searchBoxRight = 0;
        int searchBoxTop = 0;
        int searchBoxBottom = 0;
        int searchButtonLeft = -1;
        int searchButtonRight = -1;
        bool findMode = false;
        std::wstring findQuery;
        size_t findPos = 0;
        int editingField = -1;      // >= 0: omnibox edits g_currentFields[N]
        std::vector<ItemRow> itemRows;
        std::vector<size_t> itemPageRows;
        std::vector<size_t> visibleFields; // g_currentFields indices, non-hidden
        std::vector<ActionHit> actionHits;
        std::vector<TabHit> tabHits;
        std::vector<SuggestHit> suggestHits;   // address-bar dropdown rows
        std::vector<SuggestHit> bookmarkHits;  // home-page bookmark rows
        std::vector<std::wstring> suggestions; // current dropdown entries
        std::wstring suggestSource;            // input the list was built for
        int suggestIndex = -1;                 // highlighted suggestion (-1 none)
        int hoverSuggest = -1;                 // hovered suggestion index
        int dropdownTop = 5;                   // first suggestion row
        // Settings dropdown (⚙ button, top-right).
        bool settingsOpen = false;
        int settingsSel = 0;                   // keyboard-selected item
        int settingsHover = -1;                // hovered item (-1 none)
        int settingsScroll = 0;                // first visible menu row
        std::vector<SuggestHit> settingsHits;
        bool homeBookmarksShown = false;
        // Full-screen image viewer (truecolor half-block rendering).
        bool viewingImage = false;
        std::wstring viewImageUrl;
        std::wstring viewImageCaption;
        std::vector<std::array<uint8_t, 3>> imgRgb;
        uint32_t imgW = 0;
        uint32_t imgH = 0;
        std::vector<std::wstring> inputHistory;
        size_t historyIndex = 0;
        std::wstring historyDraft;
        bool browsingHistory = false;
        size_t scrollOffset = 0;
        size_t pageLineCount = 0;
        int pageCapacity = 0;
        // Text selection: absolute page-line range. Set by mouse drag over
        // plain text, or by V-mode where j/k move the cursor and y copies.
        bool selectMode = false;
        bool selDragging = false;
        long selAnchor = -1;
        long selCursor = -1;
        std::vector<std::wstring> selLines;
        // <meta http-equiv="refresh"> auto-reload timer.
        int refreshSeconds = 0;
        int refreshInterval = 0;   // >0: repeating interval (reload N)
        std::string refreshUrl;
        std::wstring refreshScanUrl;
        Clock::time_point refreshAt{};
        HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
        HANDLE inputHandle = GetStdHandle(STD_INPUT_HANDLE);
        CONSOLE_SCREEN_BUFFER_INFO info{};
        SHORT width = 80;
        SHORT height = 25;
        SHORT left = 0;
        SHORT top = 0;
    };

    void CloseImageViewer(State& state);
    void DrawImageViewer(State& state);

    // Visible (non-hidden) form fields in display order.
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
        for (SHORT row = 0; row < state.height; ++row) {
            COORD pos{state.left, static_cast<SHORT>(state.top + row)};
            DWORD written = 0;
            FillConsoleOutputCharacterW(state.output, L' ', state.width, pos, &written);
            FillConsoleOutputAttribute(state.output, kText, state.width, pos, &written);
        }
    }

    void At(State& state, int x, int y, const std::wstring& value,
            WORD color = kText, int maxWidth = -1) {
        if (y < 0 || y >= state.height || x < 0 || x >= state.width) return;
        const int room = state.width - x;
        const int limit = maxWidth < 0 ? room : std::min(room, maxWidth);
        if (limit <= 0) return;
        std::wstring text = value;
        for (wchar_t& ch : text)
            if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
        if (static_cast<int>(text.size()) > limit)
            text.resize(static_cast<size_t>(limit));
        COORD pos{static_cast<SHORT>(state.left + x),
                  static_cast<SHORT>(state.top + y)};
        SetConsoleCursorPosition(state.output, pos);
        SetConsoleTextAttribute(state.output, color);
        DWORD written = 0;
        if (!text.empty())
            WriteConsoleW(state.output, text.data(), static_cast<DWORD>(text.size()),
                          &written, nullptr);
    }

    // Draw a text line, highlighting every occurrence of `highlight`
    // (the last Ctrl+F / "/" query) with the selection color.
    void DrawText(State& state, int x, int y, const std::wstring& text,
                  WORD color, const std::wstring& highlight) {
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

    void Center(State& state, int y, const std::wstring& value, WORD color = kText) {
        const int x = std::max(0, (static_cast<int>(state.width) -
                                   static_cast<int>(value.size())) / 2);
        At(state, x, y, value, color);
    }

    std::wstring Shorten(std::wstring value, size_t maxChars) {
        if (value.size() <= maxChars) return value;
        if (maxChars < 4) return value.substr(0, maxChars);
        return value.substr(0, maxChars - 3) + L"...";
    }

    std::vector<std::wstring> Wrap(const std::wstring& source, size_t lineWidth,
                                   size_t maxLines) {
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
        At(state, x + 3, row, shown, inputColor, fieldWidth);
        At(state, x + boxWidth - 1, row, L"│", kMuted);
        At(state, x, row + 1, L"╰" + std::wstring(boxWidth - 2, L'─') + L"╯", kMuted);
        if (boxWidth >= 30)
            DrawSearchButton(state, row, x + boxWidth - 10);
    }

    int DrawNavButton(State& state, int x, const std::wstring& label,
                      NavAction action, bool enabled, int row = 1) {
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
    void DrawHomeBookmarks(State& state, int topRow) {
        state.bookmarkHits.clear();
        state.homeBookmarksShown = false;
        const auto bookmarks = GetBookmarksCached();
        if (bookmarks.empty() || !state.input.empty()) return;
        // Fit between the hints and the status/footer rows.
        int room = state.height - 3 - topRow - 1;
        const size_t shown = std::min<size_t>(
            std::min<size_t>(bookmarks.size(), 6),
            static_cast<size_t>(std::max(0, room)));
        if (shown == 0) return;
        state.homeBookmarksShown = true;
        Center(state, topRow, L"ЗАКЛАДКИ", kMuted);
        for (size_t i = 0; i < shown; ++i) {
            const int y = topRow + 1 + static_cast<int>(i);
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

    struct PageLine {
        std::wstring text;
        int itemIndex = -1;  // >=0: selectable link (0..L-1) or field (L..)
        bool heading = false;
    };

    void ClearSelection(State& state) {
        state.selectMode = false;
        state.selDragging = false;
        state.selAnchor = -1;
        state.selCursor = -1;
        state.selLines.clear();
    }

    bool HasSelection(const State& state) {
        return state.selAnchor >= 0 && state.selCursor >= 0 &&
               state.selAnchor != state.selCursor;
    }

    // Join the selected page lines into plain text for the clipboard.
    std::wstring SelectedText(const State& state) {
        if (!HasSelection(state)) return {};
        const long lo = std::min(state.selAnchor, state.selCursor);
        const long hi = std::max(state.selAnchor, state.selCursor);
        std::wstring out;
        for (long i = lo; i <= hi; ++i) {
            if (i < 0 || i >= static_cast<long>(state.selLines.size()))
                continue;
            if (!out.empty()) out += L'\n';
            out += state.selLines[static_cast<size_t>(i)];
        }
        return out;
    }

    void CopySelectionOrUrl(State& state) {
        const std::wstring sel = SelectedText(state);
        if (!sel.empty()) {
            if (UiCopyToClipboard(sel)) {
                g_consoleStatus = L"Выделенный текст скопирован (" +
                    std::to_wstring(sel.size()) + L" симв.).";
                return;
            }
        }
        // Fall back to the previous behaviour: link/image URL or page URL.
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
        if (!toCopy.empty() && UiCopyToClipboard(toCopy))
            g_consoleStatus = L"Ссылка скопирована в буфер обмена.";
        else
            g_consoleStatus = L"Нечего копировать.";
    }

    void Render(State& state) {
        RefreshSize(state);
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
                    L"help", L"open", L"search", L"download" };
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

        DrawTabStrip(state); // row 0: browser tabs, always visible
        DrawSettingsButton(state); // row 1, right edge: ⚙ Настройки

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
            // ":cmd" input renders as a bottom command line instead of
            // the top omnibox (like Vim's command line).
            DrawSearchBox(state, center - 2);
            Center(state, center + 2,
                   L"Введите запрос или адрес  ·  Enter ", kMuted);
            Center(state, center + 4,
                   L"Ctrl+H — история   Ctrl+F — найти   j/k — ссылки   Enter — открыть   o — адрес", kMuted);
            DrawHomeBookmarks(state, center + 6);
            Center(state, height - 2,
                   L"Tab — поле/ссылки   Ctrl+L — адрес   F6 — поле   b/f — назад/вперёд   Esc — выход", kMuted);
            std::wstring statusLine = g_consoleStatus;
            if (loading) {
                statusLine = statusLine.empty() ? L"Загрузка…" : statusLine;
            }
            if (!statusLine.empty())
                Center(state, height - 1,
                       Shorten(statusLine, static_cast<size_t>(width - 4)),
                       loading ? kAccent : kMuted);
        } else {
            // Browser chrome: row 0 = tabs, row 1 = nav buttons,
            // rows 2-4 = omnibox (address bar), content below.
            int navX = 2;
            const bool canBack = UiNavCanBack();
            const bool canForward = UiNavCanForward();
            const bool canReload = !g_currentUrl.empty();
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
            DrawSearchBox(state, 3);

            const size_t textLimit = std::min<size_t>(g_currentText.size(), MAX_DISPLAY_CHARS);
            const std::wstring pageText = ToWide(g_currentText.substr(0, textLimit));
            std::vector<PageLine> pageLines;
            const size_t lineWidth = static_cast<size_t>(std::max(20, width - 8));
            const auto wrapped = Wrap(pageText, lineWidth, 3000);
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
            if (!g_currentImages.empty()) {
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

            const int contentTop = 6;
            const int footerRow = height - 2;
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
                    At(state, 4, y, line.text, color, width - 8);
                } else {
                    DrawText(state, 4, y, line.text, color, state.findQuery);
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
                status = L"Загрузка… " + FormatBytes(got);
                if (total > 0) {
                    const int pct = static_cast<int>(got * 100 / total);
                    status += L" / " + FormatBytes(total) + L" (" +
                        std::to_wstring(std::min(100, pct)) + L"%)";
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
                std::wstring dl = L"⇩ " + FormatBytes(TotalDownloadProgress());
                const size_t pending = PendingDownloadCount();
                if (pending > 0)
                    dl += L" (+" + std::to_wstring(pending) + L" в очереди)";
                status = dl + L"   " + status;
            }
            At(state, 2, height - 3,
               Shorten(status, static_cast<size_t>(width - 4)),
               loading ? kAccent : kMuted, width - 4);
            // Scroll position indicator, right-aligned on the status row.
            if (state.pageLineCount > static_cast<size_t>(state.pageCapacity)) {
                const size_t cap = static_cast<size_t>(state.pageCapacity);
                const size_t pages =
                    (state.pageLineCount + cap - 1) / cap;
                const size_t cur = std::min(pages,
                    state.scrollOffset / cap + 1);
                const size_t maxOff = state.pageLineCount - cap;
                const int pct = maxOff
                    ? static_cast<int>(state.scrollOffset * 100 / maxOff) : 0;
                // Extras: link count always; feed item count when the page
                // is an RSS/Atom document; offline flag when the last fetch
                // failed at network level.
                std::wstring ind;
                if (g_offline) ind += L"офлайн · ";
                ind += L"стр " + std::to_wstring(cur) + L"/" +
                    std::to_wstring(pages) + L" · " +
                    std::to_wstring(pct) + L"% · " +
                    std::to_wstring(g_currentLinks.size()) + L" сс.";
                if (g_isFeed) {
                    size_t items = 0;
                    for (size_t i = 0; i + 2 < pageText.size(); ++i)
                        if (pageText[i] == L'\n' && pageText[i + 1] == L'•')
                            ++items;
                    if (pageText.rfind(L"•", 0) == 0) ++items;
                    ind += L" · лента: " + std::to_wstring(items);
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
        }

        // Autocomplete dropdown paints last so it overlays page content.
        DrawSuggestions(state);
        DrawSettingsMenu(state);

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

    bool IsUrlLikeInput(const std::wstring& value) {
        return IsUrlLike(value);
    }

    void OpenSelected(State& state, size_t index) {
        if (index >= g_currentLinks.size()) {
            g_consoleStatus = L"Ссылка не найдена.";
            return;
        }
        const std::wstring target = g_currentLinks[index].url;
        // Downloads-page entries open local files via the shell.
        if (target.rfind(L"openfile://", 0) == 0) {
            UiOpenDownloadedFile(target.substr(11));
            return;
        }
        // Links to files we can't render go straight to the downloader —
        // same as a browser's "download" behavior for binary content.
        static const wchar_t* kFileExts[] = {
            L".zip", L".rar", L".7z", L".tar", L".gz", L".bz2", L".xz",
            L".exe", L".msi", L".dll", L".dmg", L".pkg", L".deb", L".rpm",
            L".apk", L".ipa", L".pdf", L".doc", L".docx", L".xls", L".xlsx",
            L".ppt", L".pptx", L".odt", L".ods", L".mp3", L".mp4", L".avi",
            L".mkv", L".mov", L".wmv", L".flv", L".wav", L".flac", L".ogg",
            L".iso", L".img", L".bin", L".torrent", L".psd", L".ai",
            L".vsix", L".appimage", L".jar", L".war", L".ear", L".xpi",
            L".crx", L".aab", L".apk", L".run", L".app", L".msu", L".sys",
            L".cab", L".chm", L".epub", L".cbz", L".sketch", L".fig", L".xd",
        };
        std::wstring lowerTarget = target;
        std::transform(lowerTarget.begin(), lowerTarget.end(),
                       lowerTarget.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        // Strip query/fragment before checking the extension.
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
        state.hoveredAction = -1;
        state.scrollOffset = 0;
        UiOpenUrl(target);
    }

    // Activate a form field: text-like fields enter edit mode, the rest
    // (checkbox/radio/select/submit) are handled by UiSubmitForm.
    void ActivateField(State& state, int itemIndex) {
        const size_t linkCount = g_currentLinks.size();
        if (static_cast<size_t>(itemIndex) < linkCount ||
            state.visibleFields.empty()) {
            g_consoleStatus = L"Элемент не найден.";
            return;
        }
        const size_t fieldPos =
            static_cast<size_t>(itemIndex) - linkCount;
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

    // Activate whatever item (link, field or image) is selected/hovered.
    // Decode + show an image full-area with truecolor half blocks.
    void OpenImageViewer(State& state, int item) {
        const int img = ImageIndexForItem(state, item);
        if (img < 0 || g_currentImages[static_cast<size_t>(img)].url.empty()) {
            g_consoleStatus = L"У изображения нет адреса.";
            return;
        }
        const PageImage& image = g_currentImages[static_cast<size_t>(img)];
        // Decode is bounded by the overlay area; each cell row covers 2 px.
        const uint32_t maxW = static_cast<uint32_t>(
            std::max(8, state.width - 4));
        const uint32_t maxH = static_cast<uint32_t>(
            std::max(4, (state.height - 14)) * 2);
        std::vector<std::array<uint8_t, 3>> rgb;
        uint32_t w = 0, h = 0;
        g_consoleStatus = L"Загрузка изображения…";
        if (!DecodeImagePixels(image.url, maxW, maxH, rgb, w, h)) {
            g_consoleStatus = L"Не удалось загрузить изображение.";
            return;
        }
        state.viewingImage = true;
        state.viewImageUrl = image.url;
        state.viewImageCaption = image.alt.empty()
            ? image.url : ToWide(image.alt);
        state.imgRgb = std::move(rgb);
        state.imgW = w;
        state.imgH = h;
    }

    void CloseImageViewer(State& state) {
        state.viewingImage = false;
        state.imgRgb.clear();
        state.imgW = state.imgH = 0;
    }

    // Paint the image viewer overlay: half-block "▀" cells, fg = top pixel,
    // bg = bottom pixel, via ANSI truecolor (VT processing is enabled).
    void DrawImageViewer(State& state) {
        const int top = 4;
        const int bottom = state.height - 2;
        const int availW = state.width - 2;
        const int availH = bottom - top;
        if (availW < 8 || availH < 4) return;

        const int cellRows = static_cast<int>((state.imgH + 1) / 2);
        const int startX = 1 + std::max(0, (availW - static_cast<int>(state.imgW)) / 2);
        const int startY = top + std::max(0, (availH - cellRows) / 2);

        // Clear the overlay area.
        const std::wstring blank(static_cast<size_t>(state.width), L' ');
        for (int y = top; y <= bottom; ++y) {
            COORD pos{static_cast<SHORT>(state.left),
                      static_cast<SHORT>(state.top + y)};
            SetConsoleCursorPosition(state.output, pos);
            DWORD written = 0;
            WriteConsoleW(state.output, blank.data(),
                          static_cast<DWORD>(blank.size()), &written, nullptr);
        }

        At(state, 0, 3,
           L" [IMAGE] " + state.viewImageCaption.substr(0, 90) +
               L"  (" + std::to_wstring(state.imgW) + L"x" +
               std::to_wstring(state.imgH) + L")",
           kAccent);
        At(state, 0, bottom,
           L"  Enter/Esc — назад    D — скачать    ссылка открывается из списка",
           kMuted);

        for (int cy = 0; cy < cellRows; ++cy) {
            std::wstring line;
            line.reserve(static_cast<size_t>(state.imgW) * 40);
            for (uint32_t x = 0; x < state.imgW; ++x) {
                const uint32_t y0 = static_cast<uint32_t>(cy) * 2;
                const auto& t = state.imgRgb[static_cast<size_t>(y0) * state.imgW + x];
                uint8_t bcol[3] = {255, 255, 255};
                if (y0 + 1 < state.imgH) {
                    const auto& p = state.imgRgb[static_cast<size_t>(y0 + 1) * state.imgW + x];
                    bcol[0] = p[0]; bcol[1] = p[1]; bcol[2] = p[2];
                }
                wchar_t seq[64];
                std::swprintf(seq, 64,
                    L"\x1b[38;2;%d;%d;%dm\x1b[48;2;%d;%d;%dm",
                    t[0], t[1], t[2], bcol[0], bcol[1], bcol[2]);
                line += seq;
                line += L'\u2580';
            }
            line += L"\x1b[0m";
            COORD pos{static_cast<SHORT>(state.left + startX),
                      static_cast<SHORT>(state.top + startY + cy)};
            SetConsoleCursorPosition(state.output, pos);
            DWORD written = 0;
            WriteConsoleW(state.output, line.data(),
                          static_cast<DWORD>(line.size()), &written, nullptr);
        }
        // Restore default attribute for anything drawn after.
        SetConsoleTextAttribute(state.output, kText);
    }

    void ActivateItem(State& state, int item) {
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
            OpenImageViewer(state, item);
            return;
        }
        ActivateField(state, item);
    }

    void RememberInput(State& state, const std::wstring& value) {
        if (value.empty()) return;
        if (state.inputHistory.empty() || state.inputHistory.back() != value)
            state.inputHistory.push_back(value);
        if (state.inputHistory.size() > 80)
            state.inputHistory.erase(state.inputHistory.begin());
        state.historyIndex = state.inputHistory.size();
        state.browsingHistory = false;
        state.historyDraft.clear();
    }

    void ActivateAction(State& state, NavAction action) {
        CloseImageViewer(state);
        state.selectedItem = -1;
        state.hoveredItem = -1;
        state.hoveredAction = -1;
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
        case NavAction::Settings:
            state.settingsOpen = !state.settingsOpen;
            state.settingsSel = 0;
            state.settingsHover = -1;
            state.settingsScroll = 0;
            return; // don't steal focus / reset scroll for a menu toggle
        }
        if (action != NavAction::Home)
            state.focus = Focus::Page;
    }

    void Submit(State& state);

    void ToggleTheme() {
        ApplyTheme(g_themeIndex + 1);
        SaveThemeSetting();
        g_consoleStatus = L"Тема: " + g_themes[g_themeIndex].name;
    }

    void PageFocusReset(State& state) {
        state.selectedItem = -1;
        state.hoveredItem = -1;
        state.scrollOffset = 0;
        state.focus = Focus::Page;
        ClearSelection(state);
    }

    // Runs one entry of the ⚙ settings dropdown.
    void ActivateSettingsItem(State& state, int index) {
        state.settingsOpen = false;
        state.settingsHover = -1;
        const auto& items = SettingsMenuItems();
        if (index < 0 || index >= static_cast<int>(items.size())) return;
        switch (items[static_cast<size_t>(index)].action) {
        case MenuAction::Theme:       ToggleTheme(); break;
        case MenuAction::PageInfo:    UiPageInfoPage(); break;
        case MenuAction::ViewSource:  UiViewSourcePage(); break;
        case MenuAction::CopyUrl:
            if (!g_currentUrl.empty() && UiCopyToClipboard(g_currentUrl))
                g_consoleStatus = L"URL страницы скопирован.";
            else
                g_consoleStatus = L"Нечего копировать.";
            break;
        case MenuAction::External:    UiOpenExternalBrowser(); break;
        case MenuAction::NewTab:
            ApplyView(state, UiNewTab(CurrentView(state))); break;
        case MenuAction::CloseTab:
            ApplyView(state, UiCloseTab(CurrentView(state))); break;
        case MenuAction::NextTab:
            ApplyView(state, UiCycleTab(1, CurrentView(state))); break;
        case MenuAction::PrevTab:
            ApplyView(state, UiCycleTab(-1, CurrentView(state))); break;
        case MenuAction::Downloads:
            UiOpenInternalPage(L"downloads"); PageFocusReset(state); break;
        case MenuAction::HistoryPg:
            UiShowHistoryPage(); PageFocusReset(state); break;
        case MenuAction::BookmarksPg:
            UiShowBookmarksPage(); PageFocusReset(state); break;
        case MenuAction::ClearHistory:   UiClearHistory(); break;
        case MenuAction::ClearBookmarks: UiClearBookmarks(); break;
        case MenuAction::ClearCache:     UiClearCache(); break;
        case MenuAction::Export:         ExportBookmarksHtml(); break;
        case MenuAction::SyncPush:       UiSync(L"push"); break;
        case MenuAction::SyncPull:       UiSync(L"pull"); break;
        case MenuAction::SyncStatus:     UiSync(L"status"); break;
        case MenuAction::WebOff:
            wv::Hide();
            g_consoleStatus = L"WebView2-окно скрыто.";
            break;
        case MenuAction::Help:
            state.input = L"help";
            state.caret = state.input.size();
            state.inputViewStart = 0;
            state.focus = Focus::Search;
            Submit(state);
            break;
        }
    }

    void FindNextOnPage(State& state) {
        std::wstring query = TrimWide(state.input);
        if (query.empty())
            query = state.findQuery;
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
            pos = haystack.find(needle); // wrap around
        if (pos == std::wstring::npos) {
            g_consoleStatus = L"Текст не найден на странице.";
            state.findPos = 0;
            return;
        }

        state.findPos = pos + needle.size();
        if (state.findPos >= haystack.size())
            state.findPos = 0;

        // Scroll so the match is roughly centered — count newlines before
        // the hit as an approximation of the display row (wrapped lines
        // make it approximate, but it lands near the match).
        size_t lineIdx = 0;
        for (size_t i = 0; i < pos; ++i)
            if (haystack[i] == L'\n') ++lineIdx;
        const size_t cap = state.pageCapacity > 0
            ? static_cast<size_t>(state.pageCapacity) : 20;
        state.scrollOffset = lineIdx > cap / 2 ? lineIdx - cap / 2 : 0;

        // Count total occurrences for the status line.
        size_t count = 0;
        for (size_t p = haystack.find(needle); p != std::wstring::npos;
             p = haystack.find(needle, p + needle.size()))
            ++count;

        // Build a readable snippet around the match.
        const std::wstring wideText = ToWide(g_currentText);
        const size_t start = pos > 40 ? pos - 40 : 0;
        const size_t end = std::min(wideText.size(), pos + needle.size() + 60);
        std::wstring snippet = wideText.substr(start, end - start);
        if (start > 0) snippet = L"…" + snippet;
        if (end < wideText.size()) snippet += L"…";

        g_consoleStatus = L"Найдено " + std::to_wstring(count) +
                          L" · " + snippet;
        state.focus = Focus::Page;
    }

    void Submit(State& state) {
        std::wstring value = TrimWide(state.input);
        // ':' command prefix — strip so ":back" behaves like "back".
        if (!value.empty() && value.front() == L':')
            value = TrimWide(value.substr(1));
        state.input.clear();
        state.caret = 0;
        state.inputViewStart = 0;
        state.browsingHistory = false;

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
            FindNextOnPage(state);
            return;
        }

        // Enter with a highlighted autocomplete suggestion fills the input
        // instead of submitting — press Enter again to go.
        if (!state.suggestions.empty() && state.suggestIndex >= 0) {
            state.input = state.suggestions[static_cast<size_t>(state.suggestIndex)];
            state.suggestIndex = -1;
            state.caret = state.input.size();
            state.inputViewStart = 0;
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
            ImportBookmarksHtml(TrimWide(value.substr(7)));
            return;
        }
        if (lower == L"bookmarks-clear") { UiClearBookmarks(); return; }
        if (lower == L"cache-clear") { UiClearCache(); return; }
        if (lower == L"help") {
            g_consoleStatus = L"Запрос или адрес + Enter. Команды: back, forward, reload, home, history, bookmarks, downloads, download <url> [имя], edge <url>, web [url|back|find <т>], ai [@] <вопрос>, ai sum, stop, bookmark add, stats, tabs, dup, reopen, copy, source, info, reader, md, backup, session, clear-history, clear-history-older <дн>, export, import <файл>, bookmarks-clear, cache-clear, exit. Клавиши на странице: j/k, Enter, o, /, b/f, g/G, u — исходник, i — инфо, h — история, n/w — вкладки, ,/. — навигация, s — сохранить (Shift+S — markdown), a — закладка, e — внешний браузер, v — вставить, c — копировать ссылку, z/Ctrl+Shift+C — текст страницы, V — выделение мышью/клавиатами + y — копировать, F3 — следующее совпадение, ? — справка.";
            return;
        }
        if (lower == L"stats") { UiShowStats(); return; }
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
        if (lower == L"copy") {
            if (!g_currentUrl.empty() && UiCopyToClipboard(g_currentUrl))
                g_consoleStatus = L"URL страницы скопирован.";
            else
                g_consoleStatus = L"Нечего копировать.";
            return;
        }
        if (lower == L"source") { UiViewSourcePage(); return; }
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

    void HandleKey(State& state, const KEY_EVENT_RECORD& key, bool& running) {
        if (!key.bKeyDown) return;
        const DWORD modifiers = key.dwControlKeyState;
        const bool ctrl = (modifiers & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
        const bool alt = (modifiers & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;
        const bool shift = (modifiers & SHIFT_PRESSED) != 0;
        const WORD code = key.wVirtualKeyCode;

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

        // Embedded shell: Ctrl+` opens cmd.exe in this console.
        // Type `exit` (or close the shell) to return to the browser.
        if (ctrl && code == VK_OEM_3) {
            CloseImageViewer(state);
            g_consoleStatus = L"Встроенный терминал: exit — вернуться.";
            Render(state);
            std::system("cmd.exe");
            Render(state);
            return;
        }

        // Ctrl+L also cancels field editing. (!shift — Ctrl+Shift+L is the
        // theme toggle below; VK_L is the same for both.)
        if (ctrl && !shift && code == 'L') {
            state.editingField = -1;
            state.input.clear(); state.caret = 0; state.inputViewStart = 0;
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
        if (ctrl && code == 'V' && state.focus == Focus::Search) {
            // Paste clipboard text into the omnibox at the caret.
            const std::wstring clip = UiGetClipboardText();
            if (!clip.empty()) {
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
        if (code == VK_F3) { // find next occurrence on the page
            FindNextOnPage(state);
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
            // Clear the omnibox.
            state.input.clear();
            state.caret = 0;
            state.inputViewStart = 0;
            state.suggestIndex = -1;
            return;
        }
        if (ctrl && code == VK_BACK && state.focus == Focus::Search) {
            // Delete the word before the caret.
            size_t p = state.caret;
            while (p > 0 && (state.input[p - 1] == L' ' ||
                             state.input[p - 1] == L'\t'))
                --p;
            while (p > 0 && state.input[p - 1] != L' ' &&
                   state.input[p - 1] != L'\t')
                --p;
            if (p < state.caret) {
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
                if (fi >= 0) UiSubmitFormForField(fi);
                else Submit(state);
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
            } else if (state.selAnchor >= 0) {
                // Esc clears an active text selection first.
                ClearSelection(state);
            } else if (!state.input.empty()) {
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

        if (code == VK_LEFT) { if (state.caret > 0) --state.caret; state.browsingHistory = false; return; }
        if (code == VK_RIGHT) { if (state.caret < state.input.size()) ++state.caret; state.browsingHistory = false; return; }
        if (code == VK_HOME) { state.caret = 0; return; }
        if (code == VK_END) { state.caret = state.input.size(); return; }
        if (code == VK_BACK) {
            if (state.caret > 0) {
                size_t eraseAt = state.caret - 1;
                if (eraseAt > 0 && state.input[eraseAt] >= 0xDC00 && state.input[eraseAt] <= 0xDFFF &&
                    state.input[eraseAt - 1] >= 0xD800 && state.input[eraseAt - 1] <= 0xDBFF)
                    --eraseAt;
                state.input.erase(eraseAt, state.caret - eraseAt);
                state.caret = eraseAt;
            }
            state.browsingHistory = false;
            return;
        }
        if (code == VK_DELETE) {
            if (state.caret < state.input.size()) state.input.erase(state.caret, 1);
            state.browsingHistory = false;
            return;
        }
        const wchar_t ch = key.uChar.UnicodeChar;
        if (!ctrl && !alt && ch >= L' ' && ch != 0) {
            state.input.insert(state.caret, 1, ch);
            ++state.caret;
            state.browsingHistory = false;
        }
    }

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
            // Density: compact scrolls further per notch, spacious less.
            const size_t step = notches *
                (g_density == 0 ? 5 : g_density == 2 ? 2 : 3);
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

        // Only plain first presses count as clicks. DOUBLE_CLICK is ignored:
        // the first click already opened the link, and a second hit would land
        // on whatever row now occupies those coordinates on the new page.
        const bool isClick = mouse.dwEventFlags == 0 &&
            (mouse.dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0;

        if (mouse.dwEventFlags == MOUSE_MOVED) {
            // Drag-selection: button held over page text extends the range.
            if (state.selDragging &&
                (mouse.dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0 &&
                g_consolePageOpen && y >= 6) {
                const size_t total = state.selLines.empty()
                    ? state.pageLineCount : state.selLines.size();
                if (total > 0) {
                    long line = static_cast<long>(state.scrollOffset) + (y - 6);
                    state.selCursor = std::clamp(line, 0L,
                        static_cast<long>(total) - 1);
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
            return;
        }
        // Left-button release ends a drag selection (release itself is not
        // a click — presses are handled below).
        if (state.selDragging &&
            (mouse.dwButtonState & (FROM_LEFT_1ST_BUTTON_PRESSED |
                                    FROM_LEFT_2ND_BUTTON_PRESSED)) == 0) {
            state.selDragging = false;
            // A click without a drag leaves no range — leave select mode.
            if (!HasSelection(state)) state.selectMode = false;
            return;
        }
        if (!isClick) return;

        RefreshSize(state);
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
        // Press on plain page text starts a drag selection; presses on
        // links/fields below still activate the item immediately.
        if (g_consolePageOpen && y >= 6 && y < state.height - 3) {
            bool onItem = false;
            for (const auto& r : state.itemRows)
                if (r.row == y) { onItem = true; break; }
            if (!onItem && state.pageLineCount > 0) {
                state.selectMode = true;
                state.selDragging = true;
                const long line = static_cast<long>(state.scrollOffset) + (y - 6);
                const long maxLine = static_cast<long>(
                    state.selLines.empty() ? state.pageLineCount
                                           : state.selLines.size()) - 1;
                state.selAnchor = state.selCursor =
                    std::clamp(line, 0L, std::max(0L, maxLine));
                state.selectedItem = -1;
                state.hoveredItem = -1;
                state.focus = Focus::Page;
                return;
            }
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
        // Clicking in the page body gives the page keyboard focus for arrow scrolling.
        if (g_consolePageOpen && y >= 6 && y < state.height - 3)
            state.focus = Focus::Page;
        // Any click that didn't hit the menu closes it.
        if (state.settingsOpen) {
            state.settingsOpen = false;
            state.settingsHover = -1;
        }
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
        const DWORD newMode = (oldMode | ENABLE_EXTENDED_FLAGS | ENABLE_MOUSE_INPUT |
                               ENABLE_WINDOW_INPUT) &
                              ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_QUICK_EDIT_MODE);
        if (!SetConsoleMode(state.inputHandle, newMode)) return false;

        g_consoleUiMode = true;
        LoadThemePacks();
        LoadThemeSetting();
        // Maintenance: once-a-day data backup + cache size cap.
        std::thread([] {
            try { MaybeDailyBackup(); EvictCacheOverflow(); }
            catch (...) {}
        }).detach();
        state.focus = Focus::Search;
        state.historyIndex = 0;
        bool running = true;
        // Restore tabs saved in the previous session (data/tabs.txt).
        RestoreTabs();
        std::wstring lastUrl = g_currentUrl;
        Render(state);

        auto lastAnim = Clock::now();
        while (running) {
            // Commit any finished background load and pick up queued jobs.
            const bool loadChanged = PollAsyncLoad();
            // Periodic re-render animates the "loading" indicator.
            bool animTick = false;
            if (g_loadRunning.load() &&
                Clock::now() - lastAnim > std::chrono::milliseconds(400)) {
                lastAnim = Clock::now();
                animTick = true;
            }

            // If a new page arrived, reset scroll/selection state.
            if (g_currentUrl != lastUrl) {
                lastUrl = g_currentUrl;
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
                            const int previousHover = state.hoveredItem;
                            const int previousAction = state.hoveredAction;
                            const int previousButton = state.hoveredSearchButton;
                            const int previousSuggest = state.hoverSuggest;
                            const int previousMenu = state.settingsHover;
                            HandleMouse(state, mouse);
                            redraw = redraw || previousHover != state.hoveredItem ||
                                     previousAction != state.hoveredAction ||
                                     previousButton != state.hoveredSearchButton ||
                                     previousSuggest != state.hoverSuggest ||
                                     previousMenu != state.settingsHover;
                        } else if (mouse.dwButtonState == 0 &&
                                   mouse.dwEventFlags != DOUBLE_CLICK) {
                            // button release — no redraw needed
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
        SetConsoleMode(state.inputHandle, oldMode);
        SetConsoleTextAttribute(state.output, kText);
        CONSOLE_CURSOR_INFO cursorInfo{20, TRUE};
        SetConsoleCursorInfo(state.output, &cursorInfo);
        return true;
    }
}
