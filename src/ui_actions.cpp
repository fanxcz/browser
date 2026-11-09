#include "ui_int.h"

namespace console_ui {
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
        // Sessions-page entries restore a saved session file.
        if (target.rfind(L"restore://", 0) == 0) {
            UiRestoreSession(target.substr(10));
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
            state.inputUndo.clear();
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
        state.prevCells.clear(); // force full repaint over the overlay area
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
        SaveInputHistory(state.inputHistory);
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

    void FindOnPage(State& state, bool backwards) {
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

        const std::wstring wideAll = ToWide(g_currentText);
        const std::wstring haystack = state.findCase
            ? wideAll : LowerWide(wideAll);
        const std::wstring needle = state.findCase
            ? query : LowerWide(query);
        if (needle.empty()) {
            g_consoleStatus = L"Текст не найден.";
            return;
        }

        size_t pos;
        if (!backwards) {
            pos = haystack.find(needle, state.findPos);
            if (pos == std::wstring::npos)
                pos = haystack.find(needle); // wrap around
        } else {
            // Previous occurrence: search backwards from just before the
            // last match; wrap to the last occurrence in the document.
            const size_t cur = state.findPos > needle.size()
                ? state.findPos - needle.size() : 0;
            pos = cur > 0 ? haystack.rfind(needle, cur - 1)
                          : std::wstring::npos;
            if (pos == std::wstring::npos)
                pos = haystack.rfind(needle);
        }
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

}
