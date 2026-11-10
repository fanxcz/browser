#pragma once

// Internal shared header for the console UI modules.
// Split from console_ui.cpp — include only from src/ui_*.cpp.

#include "browser.h"
#include "wvhost.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <functional>
#include <vector>

namespace console_ui {

    // Theme packs — built-ins first, then data/themes/*.json:
    //   {"name":"Ocean","mode":"dark","accent":"#58a6ff",
    //    "density":"compact|normal|spacious"}
    struct ThemePack {
        std::wstring name;
        bool light = false;
        WORD accent = 0;   // 0 = palette default
        int density = 1;   // 0 compact, 1 normal, 2 spacious
    };

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

    inline const std::vector<MenuItem>& SettingsMenuItems() {
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

    // Command palette (Ctrl+K / Ctrl+P) entries.
    struct PaletteEntry {
        int kind = 0;          // 0 command, 1 history, 2 bookmark, 3 tab
        std::wstring text;     // command line / history text / label
        std::wstring arg;      // command desc, bookmark URL
        int tab = -1;          // tab index for kind 3
    };

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
        bool findCase = false;      // case-sensitive find (' toggles)
        bool fullscreen = false;    // F11: hide tab strip/nav/footer hints
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
        long selAnchor = -1;
        long selCursor = -1;
        std::vector<std::wstring> selLines;
        // Omnibox text selection (Ctrl+A, Shift+arrows later); anchor is the
        // fixed end, caret is the moving end.
        long inputSelAnchor = -1;
        // Terminal-style screen selection: drag over ANY cells (links,
        // status bar, tabs — everything), release keeps the highlight.
        bool mouseSelDown = false;
        int selSX = 0, selSY = 0, selEX = 0, selEY = 0;
        bool screenSelValid = false;
        int ssX1 = 0, ssY1 = 0, ssX2 = 0, ssY2 = 0;
        size_t selStartScroll = 0; // scrollOffset when the drag began
        // Click counting for double/triple-click word/line selection.
        int lastClickX = -1, lastClickY = -1;
        int clickCount = 0;
        Clock::time_point lastClickTime{};
        // Deferred single click: dispatched ~250 ms after release so a
        // fast second press can promote it to word/line selection.
        bool pendingClickValid = false;
        int pendingClickX = 0, pendingClickY = 0;
        bool pendingClickCtrl = false;
        Clock::time_point pendingClickTime{};
        bool clickCtrl = false;      // Ctrl held at press → open in new tab
        // Download speed sampling for the status bar.
        uint64_t dlPrevBytes = 0;
        Clock::time_point dlPrevTime{};
        // Scroll/resize invalidates the screen-selection rectangle — the
        // rows it pointed at no longer hold the same text.
        size_t lastScrollOffset = static_cast<size_t>(-1);
        SHORT lastSelW = -1;
        SHORT lastSelH = -1;
        // Omnibox/field undo stack: {text, caret} snapshots before edits.
        std::vector<std::pair<std::wstring, size_t>> inputUndo;
        // Command palette overlay (Ctrl+K): fuzzy list of commands,
        // history, bookmarks and tabs.
        bool paletteOpen = false;
        std::wstring paletteQuery;
        std::vector<PaletteEntry> paletteEntries;
        std::vector<int> paletteMatch;   // indices into paletteEntries
        int paletteIndex = 0;
        std::vector<SuggestHit> paletteHits;
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
        // Frame buffer: Render() draws into `cells` via At(); FlushFrame()
        // diffs against `prevCells` and writes only changed rows — the
        // console is touched only where pixels actually differ.
        std::vector<CHAR_INFO> cells;
        std::vector<CHAR_INFO> prevCells;
    };

    // Palette colors — assigned by ApplyPalette() (ui_theme.cpp).
    extern WORD kText, kMuted, kAccent, kBright, kSelected, kHover;
    extern std::vector<ThemePack> g_themes;
    extern size_t g_themeIndex;
    extern int g_density;
    extern int g_scrollLines;

    void ApplyPalette();
    WORD ConsoleColorFromHex(const std::string& hex);
    std::string JsonString(const std::string& js, const char* key);
    void LoadThemePacks();
    void ApplyTheme(size_t index);
    void LoadThemeSetting();
    void SaveThemeSetting();
    bool SaveThemeAs(const std::wstring& name);
    void ApplySiteTheme(const std::wstring& url);
    bool PullTheme(const std::wstring& name);
    void RebuildVisibleFields(State& state);
    size_t ItemCount(const State& state);
    int ImageIndexForItem(const State& state, int item);
    size_t ItemToRow(const State& state, int item);
    int FieldIndexForItem(const State& state, int item);
    int ItemIndexForField(const State& state, int fieldIdx);
    std::wstring FormatBytes(uint64_t bytes);
    TabViewState CurrentView(const State& state);
    void ClearSelection(State& state); // defined below, near PageLine
    void ApplyView(State& state, const TabViewState& view);
    void RefreshSize(State& state);
    void Clear(State& state);
    void At(State& state, int x, int y, const std::wstring& value, WORD color = kText, int maxWidth = -1);
    void DrawText(State& state, int x, int y, const std::wstring& text, WORD color, const std::wstring& highlight);
    void Center(State& state, int y, const std::wstring& value, WORD color = kText);
    std::wstring Shorten(std::wstring value, size_t maxChars);
    std::vector<std::wstring> Wrap(const std::wstring& source, size_t lineWidth, size_t maxLines);
    void DrawSearchButton(State& state, int row, int left);
    void DrawSearchBox(State& state, int row);
    int DrawNavButton(State& state, int x, const std::wstring& label, NavAction action, bool enabled, int row = 1);
    void DrawSettingsButton(State& state);
    void EnsureItemVisible(State& state, int item);
    void DrawSuggestions(State& state);
    void DrawSettingsMenu(State& state);
    std::vector<std::wstring> RecentTopSites();
    void DrawHomeBookmarks(State& state, int topRow);
    void DrawTabStrip(State& state);
    bool HasSelection(const State& state);
    void ClearInputSel(State& state);
    bool HasInputSel(const State& state);
    std::pair<size_t, size_t> InputSelRange(const State& state);
    void DeleteInputSel(State& state);
    void PushInputUndo(State& state);
    void UndoInput(State& state);
    std::wstring SelectedText(const State& state);
    std::wstring ScreenSelectionText(const State& state); // defined below
    void CopySelectionOrUrl(State& state);
    void HandleLeftClick(State& state, int x, int y); // defined below
    bool IsWordChar(wchar_t ch);
    void SelectWordAt(State& state, int x, int y);
    void SelectLineAt(State& state, int y);
    void DispatchPendingClick(State& state);
    void FinishScreenSel(State& state, int ex, int ey);
    void DrawScreenSelection(State& state);
    void FlushFrame(State& state);
    WORD StatusColor(const std::wstring& status, bool loading);
    void Render(State& state);
    bool IsUrlLikeInput(const std::wstring& value);
    void OpenSelected(State& state, size_t index);
    void ActivateField(State& state, int itemIndex);
    void OpenImageViewer(State& state, int item);
    void CloseImageViewer(State& state);
    void DrawImageViewer(State& state);
    void ActivateItem(State& state, int item);
    void RememberInput(State& state, const std::wstring& value);
    void ActivateAction(State& state, NavAction action);
    void ToggleTheme();
    void PageFocusReset(State& state);
    void ActivateSettingsItem(State& state, int index);
    void FindOnPage(State& state, bool backwards = false);
    void Submit(State& state);
    void HandleKey(State& state, const KEY_EVENT_RECORD& key, bool& running);
    void HandleMouse(State& state, const MOUSE_EVENT_RECORD& mouse);
    // Command palette (ui_palette.cpp).
    void PaletteOpen(State& state);
    void PaletteClose(State& state);
    void PaletteRebuild(State& state);
    void PaletteActivate(State& state, int matchPos);
    void DrawPalette(State& state);
    bool PaletteHandleKey(State& state, const KEY_EVENT_RECORD& key);
    BOOL WINAPI ConsoleCtrlHandler(DWORD type);
    bool Run();

} // namespace console_ui
