// gui_render.cpp — page model + GDI painting for the terminal-style GUI.

#include "gui.h"

namespace gui {

Palette g_pal{
    RGB(13, 17, 23),    // bg        — near-black blue
    RGB(201, 209, 217), // text
    RGB(230, 237, 243), // bright
    RGB(125, 133, 144), // muted
    RGB(94, 234, 212),  // accent    — terminal mint
    RGB(121, 192, 255), // hover     — soft blue
    RGB(35, 58, 82),    // selectedBg
    RGB(240, 248, 255), // selectedText
    RGB(33, 38, 48),    // box bg
    RGB(248, 81, 73),   // warn red
};

Palette g_palLight{
    RGB(246, 248, 250), // bg
    RGB(31, 35, 40),    // text
    RGB(89, 99, 110),   // bright -> muted-ish
    RGB(89, 99, 110),   // muted
    RGB(14, 116, 144),  // accent — deep cyan
    RGB(9, 105, 218),   // hover — link blue
    RGB(207, 226, 243), // selectedBg
    RGB(255, 255, 255), // selectedText
    RGB(255, 255, 255), // box bg
    RGB(207, 34, 46),   // warn
};

static size_t ItemToRowLocal(const GuiState& state, int item);

static GuiState* g_state = nullptr;

void InvalidateGui() {
    if (g_state && g_state->hwnd)
        InvalidateRect(g_state->hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Small text helpers (same semantics as the console UI versions)
// ---------------------------------------------------------------------------

std::wstring GuiShorten(std::wstring value, size_t maxChars) {
    if (value.size() <= maxChars) return value;
    if (maxChars < 4) return value.substr(0, maxChars);
    return value.substr(0, maxChars - 3) + L"...";
}

std::vector<std::wstring> GuiWrap(const std::wstring& source,
                                  size_t lineWidth, size_t maxLines) {
    std::vector<std::wstring> lines;
    if (lineWidth < 10 || maxLines == 0) return lines;

    std::wistringstream paragraphs(source);
    std::wstring paragraph;
    while (std::getline(paragraphs, paragraph) && lines.size() < maxLines) {
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

// ---------------------------------------------------------------------------
// Item model: links [0,L), then visible fields, then images — same indexing
// as the console UI so TabViewState stays compatible.
// ---------------------------------------------------------------------------

void RebuildVisibleFields(GuiState& state) {
    state.visibleFields.clear();
    for (size_t i = 0; i < g_currentFields.size(); ++i)
        if (g_currentFields[i].kind != FieldKind::Hidden)
            state.visibleFields.push_back(i);
}

size_t ItemCount(const GuiState& state) {
    return g_currentLinks.size() + state.visibleFields.size() +
           g_currentImages.size();
}

int ImageIndexForItem(const GuiState& state, int item) {
    const int base = static_cast<int>(g_currentLinks.size() +
                                      state.visibleFields.size());
    if (item < base ||
        static_cast<size_t>(item - base) >= g_currentImages.size())
        return -1;
    return item - base;
}

int FieldIndexForItem(const GuiState& state, int item) {
    if (item < 0 || static_cast<size_t>(item) >= ItemCount(state) ||
        static_cast<size_t>(item) < g_currentLinks.size())
        return -1;
    const size_t pos = static_cast<size_t>(item) - g_currentLinks.size();
    if (pos >= state.visibleFields.size()) return -1;
    return static_cast<int>(state.visibleFields[pos]);
}

int ItemIndexForField(const GuiState& state, int fieldIdx) {
    for (size_t pos = 0; pos < state.visibleFields.size(); ++pos)
        if (static_cast<int>(state.visibleFields[pos]) == fieldIdx)
            return static_cast<int>(g_currentLinks.size() + pos);
    return -1;
}

TabViewState CurrentView(const GuiState& state) {
    return {state.scrollOffset, state.selectedItem, state.hoveredItem,
            state.findMode, state.findQuery, state.findPos,
            state.editingField};
}

void ApplyView(GuiState& state, const TabViewState& view) {
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
        state.input = ToWide(
            g_currentFields[static_cast<size_t>(view.editingField)].value);
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

// ---------------------------------------------------------------------------
// Page lines: wrapped text + ССЫЛКИ + ПОЛЯ ФОРМЫ + ИЗОБРАЖЕНИЯ sections
// ---------------------------------------------------------------------------

void BuildPageLines(GuiState& state) {
    state.pageLines.clear();
    state.itemPageRows.clear();
    state.itemRows.clear();
    state.suggestHits.clear();
    state.bookmarkHits.clear();
    RebuildVisibleFields(state);

    // Address-bar autocomplete (same rules as console UI).
    state.suggestions.clear();
    if (state.input != state.suggestSource) {
        state.suggestSource = state.input;
        state.suggestIndex = -1;
    }
    if (state.focus == Focus::Search && !state.findMode &&
        state.editingField < 0 && state.input.size() >= 2)
        state.suggestions = GetUrlSuggestions(state.input, 8);
    if (state.suggestIndex >=
        static_cast<int>(state.suggestions.size()))
        state.suggestIndex = -1;

    const size_t textLimit =
        std::min<size_t>(g_currentText.size(), MAX_DISPLAY_CHARS);
    const std::wstring pageText = ToWide(g_currentText.substr(0, textLimit));
    const size_t lineWidth =
        static_cast<size_t>(std::max(20, state.cols - 8));

    state.pageLines.reserve(4096);
    const auto wrapped = GuiWrap(pageText, lineWidth, 3000);
    for (const auto& line : wrapped)
        state.pageLines.push_back({line, -1, false});

    if (state.pageLines.empty())
        state.pageLines.push_back(
            {L"На странице нет доступного текстового содержимого.", -1, false});

    if (!g_currentLinks.empty()) {
        state.pageLines.push_back({L"", -1, false});
        state.pageLines.push_back(
            {L"── ССЫЛКИ ──────────────────────────────", -1, true});
        for (size_t i = 0; i < g_currentLinks.size(); ++i) {
            std::wstring label = ToWide(g_currentLinks[i].label);
            if (label.empty()) label = L"Открыть ссылку";
            const std::wstring num = L"[" + std::to_wstring(i + 1) + L"]  ";
            state.pageLines.push_back({num + GuiShorten(
                label, lineWidth > num.size() + 4
                           ? lineWidth - num.size() - 4 : lineWidth),
                static_cast<int>(i), false});
            state.itemPageRows.push_back(
                static_cast<int>(state.pageLines.size() - 1));
        }
    }

    if (!state.visibleFields.empty()) {
        state.pageLines.push_back({L"", -1, false});
        state.pageLines.push_back(
            {L"── ПОЛЯ ФОРМЫ ──  j/k — выбор   Enter — изменить", -1, true});
        const size_t linkCount = g_currentLinks.size();
        for (size_t pos = 0; pos < state.visibleFields.size(); ++pos) {
            const FormField& field =
                g_currentFields[state.visibleFields[pos]];
            std::wstring marker, value;
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
                        L" = ••••••";
                break;
            default:
                marker = L"> ";
                value = ToWide(field.label.empty() ? field.name : field.label);
                {
                    std::wstring v = ToWide(field.value);
                    if (v.size() > 40) v = v.substr(0, 37) + L"...";
                    if (!v.empty()) value += L" = " + v;
                }
                break;
            }
            state.pageLines.push_back({marker + GuiShorten(
                value, lineWidth > marker.size() + 8
                           ? lineWidth - marker.size() - 8 : lineWidth),
                static_cast<int>(linkCount + pos), false});
            state.itemPageRows.push_back(
                static_cast<int>(state.pageLines.size() - 1));
        }
    }

    if (!g_currentImages.empty()) {
        state.pageLines.push_back({L"", -1, false});
        state.pageLines.push_back(
            {L"── ИЗОБРАЖЕНИЯ ──  Enter — открыть", -1, true});
        const int imgBase = static_cast<int>(
            g_currentLinks.size() + state.visibleFields.size());
        const size_t shown = std::min<size_t>(g_currentImages.size(), 40);
        for (size_t i = 0; i < shown; ++i) {
            const PageImage& img = g_currentImages[i];
            std::wstring label = ToWide(img.alt);
            if (label.empty()) {
                std::wstring u = img.url;
                const size_t slash = u.find_last_of(L'/');
                label = slash == std::wstring::npos ? u : u.substr(slash + 1);
                if (label.empty()) label = L"image";
            }
            state.pageLines.push_back({L"▣ " + GuiShorten(
                label, lineWidth > 12 ? lineWidth - 12 : lineWidth),
                imgBase + static_cast<int>(i), false});
            state.itemPageRows.push_back(
                static_cast<int>(state.pageLines.size() - 1));
        }
        if (g_currentImages.size() > shown)
            state.pageLines.push_back({L"… и ещё " +
                std::to_wstring(g_currentImages.size() - shown), -1, false});
    }

    const int contentTop = 6;
    const int footerRow = state.rows - 2;
    state.pageCapacity = std::max(1, footerRow - 1 - contentTop);
    const size_t maxOffset = state.pageLines.size() >
                                 static_cast<size_t>(state.pageCapacity)
        ? state.pageLines.size() - static_cast<size_t>(state.pageCapacity)
        : 0;
    state.scrollOffset = std::min(state.scrollOffset, maxOffset);
    if (state.selectedItem >= 0) {
        const size_t row = ItemToRowLocal(state, state.selectedItem);
        if (row < state.scrollOffset)
            state.scrollOffset = row;
        else if (row >= state.scrollOffset +
                          static_cast<size_t>(state.pageCapacity))
            state.scrollOffset = row - state.pageCapacity + 1;
    }
}

static size_t ItemToRowLocal(const GuiState& state, int item) {
    if (item < 0 || static_cast<size_t>(item) >= state.itemPageRows.size())
        return static_cast<size_t>(-1);
    return static_cast<size_t>(state.itemPageRows[static_cast<size_t>(item)]);
}

// ---------------------------------------------------------------------------
// GDI drawing helpers
// ---------------------------------------------------------------------------

struct Painter {
    HDC dc = nullptr;
    HFONT font = nullptr;
    int cellW = 9, cellH = 18;
    int cols = 0, rows = 0;

    void FillRectCells(int x, int y, int w, int h, COLORREF color) {
        RECT rc{ x * cellW, y * cellH, (x + w) * cellW, (y + h) * cellH };
        HBRUSH br = CreateSolidBrush(color);
        FillRect(dc, &rc, br);
        DeleteObject(br);
    }

    void Text(int x, int y, const std::wstring& s, COLORREF color,
              int maxCells = -1) {
        if (s.empty()) return;
        std::wstring out = s;
        if (maxCells >= 0 && static_cast<int>(out.size()) > maxCells)
            out = GuiShorten(out, static_cast<size_t>(maxCells));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, color);
        HFONT old = static_cast<HFONT>(SelectObject(dc, font));
        TextOutW(dc, x * cellW, y * cellH, out.c_str(),
                 static_cast<int>(out.size()));
        SelectObject(dc, old);
    }

    // Text with find-query highlighting (segments painted separately).
    void TextHighlight(int x, int y, const std::wstring& s, COLORREF color,
                       const std::wstring& needleWide) {
        if (needleWide.empty()) { Text(x, y, s, color); return; }
        const std::wstring hay = LowerWide(s);
        const std::wstring ndl = LowerWide(needleWide);
        const size_t qlen = ndl.size();
        int cx = x;
        size_t seg = 0;
        for (size_t p = hay.find(ndl); p != std::wstring::npos;
             p = hay.find(ndl, p + qlen)) {
            if (p > seg) { Text(cx, y, s.substr(seg, p - seg), color); cx += static_cast<int>(p - seg); }
            Text(cx, y, s.substr(p, qlen), g_pal.selectedText);
            // highlight background behind the match
            RECT rc{ cx * cellW, y * cellH, (cx + static_cast<int>(qlen)) * cellW,
                     (y + 1) * cellH };
            HBRUSH br = CreateSolidBrush(RGB(180, 90, 0));
            FrameRect(dc, &rc, br); // cheap underline-ish marker
            DeleteObject(br);
            cx += static_cast<int>(qlen);
            seg = p + qlen;
        }
        if (seg < s.size()) Text(cx, y, s.substr(seg), color);
    }
};

static void DrawCentered(Painter& p, int y, const std::wstring& s,
                         COLORREF color) {
    const int x = std::max(0, (p.cols - static_cast<int>(s.size())) / 2);
    p.Text(x, y, s, color);
}

static void DrawTabStrip(Painter& p, GuiState& state) {
    state.tabHits.clear();
    const size_t count = UiTabCount();
    const int active = static_cast<int>(UiActiveTabIndex());
    int x = 1;
    const int avail = p.cols - 8;
    const int perTab = count
        ? std::clamp(avail / static_cast<int>(count), 9, 24) : 0;

    for (size_t i = 0; i < count; ++i) {
        const std::wstring num = L" " + std::to_wstring(i + 1) + L" ";
        std::wstring title = GuiShorten(UiTabTitle(i),
            static_cast<size_t>(std::max(4, perTab -
                static_cast<int>(num.size()) - 1)));
        std::wstring label = num + title;
        const bool isActive = static_cast<int>(i) == active;
        if (isActive && g_downloadRunning.load())
            label = L"⇩" + label;
        if (isActive) label += L"×";
        const int w = static_cast<int>(label.size()) + 1;
        if (x + w > p.cols - 4) break;

        if (isActive) {
            std::wstring block(static_cast<size_t>(w), L' ');
            block.replace(0, label.size(), label);
            p.FillRectCells(x, 0, w, 1, g_pal.selectedBg);
            p.Text(x, 0, block, g_pal.selectedText);
            // hit: body = switch, last cell of label = close
            const int bodyRight = x + w - 3; // before "×"
            state.tabHits.push_back({x, bodyRight, 0, 0, false, false});
            state.tabHits.back().index = static_cast<int>(i);
            state.tabHits.push_back({x + w - 2, x + w - 1, 0, 0, true, false});
            state.tabHits.back().index = static_cast<int>(i);
        } else {
            p.Text(x, 0, label, g_pal.accent);
            state.tabHits.push_back({x, x + w - 1, static_cast<int>(i),
                                     0, false, false});
        }
        x += w + 1;
    }
    if (x + 4 <= p.cols - 2) {
        p.Text(x, 0, L"[+]", g_pal.accent);
        state.tabHits.push_back({x, x + 3, static_cast<int>(count),
                                 0, false, true});
    }
}

static void DrawNavButtons(Painter& p, GuiState& state) {
    state.navHits.clear();
    int x = 2;
    const bool wide = p.cols >= 58;
    struct Btn { const wchar_t* label; NavAction action; bool enabled; };
    Btn btns[] = {
        {wide ? L"[ ← Назад ]" : L"[←]", NavAction::Back, UiNavCanBack()},
        {wide ? L"[ Вперёд → ]" : L"[→]", NavAction::Forward, UiNavCanForward()},
        {wide ? L"[ ⌂ Домой ]" : L"[⌂]", NavAction::Home, true},
        {wide ? L"[ ↻ Обновить ]" : L"[↻]", NavAction::Reload,
         !g_currentUrl.empty()},
    };
    for (const auto& b : btns) {
        const int w = static_cast<int>(wcslen(b.label));
        COLORREF c = b.enabled ? g_pal.accent : g_pal.muted;
        p.Text(x, 1, b.label, c);
        state.navHits.push_back({x, x + w - 1,
                                 static_cast<int>(b.action), 1, false, false});
        x += w + 2;
    }
}

// Omnibox: rows 2..4 — box with input, caret, "[ Найти ]" button.
static void DrawOmnibox(Painter& p, GuiState& state) {
    const int boxW = std::min(72, p.cols - 8);
    const int x = (p.cols - boxW) / 2;
    const int fieldW = boxW - 6 - (boxW >= 30 ? 9 : 0);
    const int row = 3;

    state.searchBox = {x, row - 1, x + boxW - 1, row + 1};
    state.searchButton = boxW >= 30
        ? Rect{x + boxW - 10, row, x + boxW - 2, row} : Rect{};

    state.caret = std::min(state.caret, state.input.size());
    if (state.caret < state.inputViewStart)
        state.inputViewStart = state.caret;
    else if (state.caret >= state.inputViewStart +
                          static_cast<size_t>(fieldW))
        state.inputViewStart = state.caret - static_cast<size_t>(fieldW) + 1;
    const size_t maxStart = state.input.size() >
                                static_cast<size_t>(fieldW)
        ? state.input.size() - static_cast<size_t>(fieldW) : 0;
    state.inputViewStart = std::min(state.inputViewStart, maxStart);

    p.Text(x, row - 1, L"╭" + std::wstring(boxW - 2, L'─') + L"╮",
           g_pal.muted);
    p.FillRectCells(x, row, boxW, 1, g_pal.box);
    p.Text(x, row, L"│", g_pal.muted);

    std::wstring shown;
    COLORREF inputColor = g_pal.bright;
    if (state.editingField >= 0 &&
        static_cast<size_t>(state.editingField) < g_currentFields.size()) {
        const FormField& field =
            g_currentFields[static_cast<size_t>(state.editingField)];
        std::wstring hint = field.label.empty()
            ? ToWide(field.name) : ToWide(field.label);
        if (hint.empty()) hint = L"значение поля";
        if (state.input.empty()) {
            shown = L"✎ " + hint;
            inputColor = g_pal.muted;
        } else {
            shown = state.input.substr(state.inputViewStart,
                static_cast<size_t>(fieldW));
        }
    } else if (state.input.empty()) {
        if (state.findMode) shown = L"Найти на странице…";
        else if (g_consolePageOpen) shown = g_currentUrl;
        else shown = L"Поиск или адрес сайта  ·  Enter";
        inputColor = g_pal.muted;
    } else {
        shown = state.input.substr(state.inputViewStart,
            static_cast<size_t>(fieldW));
    }
    p.Text(x + 3, row, shown, inputColor, fieldW);

    // Caret (blinking block) when the omnibox has focus.
    if (state.focus == Focus::Search && state.caretOn) {
        const size_t caretCol = state.inputViewStart < state.caret
            ? state.caret - state.inputViewStart : 0;
        if (caretCol < static_cast<size_t>(fieldW)) {
            RECT rc{ (x + 3 + static_cast<int>(caretCol)) * p.cellW,
                     row * p.cellH,
                     (x + 4 + static_cast<int>(caretCol)) * p.cellW,
                     (row + 1) * p.cellH };
            // Draw via a 1-cell-wide text "█" in accent color.
            p.Text(x + 3 + static_cast<int>(caretCol), row,
                   L"█", g_pal.accent);
            // Restore the char under the caret.
            if (!shown.empty() &&
                caretCol < shown.size() && shown[caretCol] != L'█')
                p.Text(x + 3 + static_cast<int>(caretCol), row,
                       std::wstring(1, shown[caretCol]), inputColor);
            (void)rc;
        }
    }

    p.Text(x + boxW - 1, row, L"│", g_pal.muted);
    p.Text(x, row + 1, L"╰" + std::wstring(boxW - 2, L'─') + L"╯",
           g_pal.muted);
    if (boxW >= 30)
        p.Text(x + boxW - 10, row, L"[ Найти ]", g_pal.accent);
}

static void DrawSuggestions(Painter& p, GuiState& state) {
    if (state.suggestions.empty()) return;
    const int maxRows = std::min<int>(
        static_cast<int>(state.suggestions.size()),
        std::max(1, state.rows - 4 - 4));
    for (int i = 0; i < maxRows; ++i) {
        const int y = 5 + i;
        const bool active = i == state.suggestIndex || i == state.hoverSuggest;
        std::wstring text = L"  → " + GuiShorten(
            state.suggestions[static_cast<size_t>(i)],
            static_cast<size_t>(p.cols - 8));
        if (active) p.FillRectCells(1, y, p.cols - 2, 1, g_pal.selectedBg);
        p.Text(2, y, text, active ? g_pal.selectedText : g_pal.muted,
               p.cols - 4);
        state.suggestHits.push_back({1, p.cols - 2, i, y, false, false});
    }
}

static void DrawHomeBookmarks(Painter& p, GuiState& state, int topRow) {
    const auto bookmarks = GetBookmarksCached();
    if (bookmarks.empty() || !state.input.empty()) return;
    int room = state.rows - 3 - topRow - 1;
    const size_t shown = std::min<size_t>(
        std::min<size_t>(bookmarks.size(), 6),
        static_cast<size_t>(std::max(0, room)));
    if (shown == 0) return;
    DrawCentered(p, topRow, L"ЗАКЛАДКИ", g_pal.muted);
    for (size_t i = 0; i < shown; ++i) {
        const int y = topRow + 1 + static_cast<int>(i);
        std::wstring label = bookmarks[i];
        const size_t schemeEnd = label.find(L"://");
        if (schemeEnd != std::wstring::npos)
            label = label.substr(schemeEnd + 3);
        label = GuiShorten(label, 48);
        const int x = std::max(2, (p.cols - 50) / 2);
        p.Text(x, y, L"  ★ " + label, g_pal.accent, 50);
        state.bookmarkHits.push_back({x, x + 52, static_cast<int>(i), y,
                                      false, false});
    }
}

// ---------------------------------------------------------------------------
// Main frame
// ---------------------------------------------------------------------------

void RenderFrame(HWND hwnd, GuiState& state) {
    g_state = &state;
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT client;
    GetClientRect(hwnd, &client);

    Painter p;
    p.dc = hdc;
    p.font = state.font;
    p.cols = std::max(40, static_cast<int>(client.right) / state.cellW);
    p.rows = std::max(15, static_cast<int>(client.bottom) / state.cellH);
    state.cols = p.cols;
    state.rows = p.rows;

    // Background.
    HBRUSH bg = CreateSolidBrush(g_pal.bg);
    FillRect(hdc, &client, bg);
    DeleteObject(bg);

    // Double-buffer-free direct paint is fine at this size.
    DrawTabStrip(p, state);

    if (!g_consolePageOpen) {
        state.focus = Focus::Search;
        const int center = p.rows / 2;
        if (p.cols >= 44) {
            DrawCentered(p, center - 5,
                L"▛▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▜", g_pal.accent);
            DrawCentered(p, center - 4,
                L"▌   B R O W S E R   G U I   ▐", g_pal.bright);
            DrawCentered(p, center - 3,
                L"▙▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▟", g_pal.accent);
            DrawCentered(p, center - 1, L"v1.0", g_pal.muted);
        } else {
            DrawCentered(p, center - 4, L"Browser", g_pal.bright);
        }
        DrawOmnibox(p, state);
        DrawCentered(p, center + 2,
            L"Введите запрос или адрес сайта  ·  Enter", g_pal.muted);
        DrawCentered(p, center + 4,
            L"j/k — ссылки   Enter — открыть   o — адрес   Ctrl+J — загрузки   help — команды",
            g_pal.muted);
        DrawHomeBookmarks(p, state, center + 6);
        DrawCentered(p, p.rows - 2,
            L"Tab — поле/ссылки   Ctrl+L — адрес   F6 — поле   b/f — назад/вперёд   Esc — выход",
            g_pal.muted);
        std::wstring statusLine = g_consoleStatus;
        if (g_loadRunning.load())
            statusLine = statusLine.empty() ? L"Загрузка…" : statusLine;
        if (!statusLine.empty())
            DrawCentered(p, p.rows - 1,
                GuiShorten(statusLine, static_cast<size_t>(p.cols - 4)),
                g_loadRunning.load() ? g_pal.accent : g_pal.muted);
        EndPaint(hwnd, &ps);
        return;
    }

    // Browser chrome: row 1 = nav, rows 2-4 = omnibox, content below.
    DrawNavButtons(p, state);
    DrawOmnibox(p, state);

    const int contentTop = 6;
    const int footerRow = p.rows - 2;
    const int capacity = std::max(1, footerRow - 1 - contentTop);
    state.pageCapacity = capacity;
    const size_t maxOffset = state.pageLines.size() >
                                 static_cast<size_t>(capacity)
        ? state.pageLines.size() - static_cast<size_t>(capacity) : 0;
    state.scrollOffset = std::min(state.scrollOffset, maxOffset);

    // Ensure selected item visible.
    if (state.selectedItem >= 0) {
        const size_t row = ItemToRowLocal(state, state.selectedItem);
        if (row < state.scrollOffset)
            state.scrollOffset = row;
        else if (row >= state.scrollOffset +
                          static_cast<size_t>(capacity))
            state.scrollOffset = row - capacity + 1;
    }

    for (int visible = 0; visible < capacity; ++visible) {
        const size_t lineIndex = state.scrollOffset +
                                 static_cast<size_t>(visible);
        if (lineIndex >= state.pageLines.size()) break;
        const PageLine& line = state.pageLines[lineIndex];
        const int y = contentTop + visible;
        COLORREF color = line.heading ? g_pal.accent : g_pal.text;
        if (!line.text.empty() && line.itemIndex < 0) {
            if (line.text.front() == L'■') color = g_pal.accent;
            else if (line.text.front() == L'•' || line.text.front() == L'◦' ||
                     line.text.front() == L'▪') color = g_pal.bright;
            else if (line.text.front() == L'▎') color = g_pal.muted;
        }
        if (line.itemIndex >= 0) {
            const bool highlighted = line.itemIndex == state.selectedItem ||
                                     line.itemIndex == state.hoveredItem;
            color = highlighted ? g_pal.selectedText : g_pal.bright;
            if (highlighted)
                p.FillRectCells(1, y, p.cols - 2, 1, g_pal.selectedBg);
            p.Text(4, y, line.text, color, p.cols - 8);
            state.itemRows.push_back({y, line.itemIndex});
        } else {
            p.TextHighlight(4, y, line.text, color, state.findQuery);
        }
    }

    // Position indicator (top-right of content area).
    if (!state.pageLines.empty()) {
        const std::wstring pos =
            std::to_wstring(state.scrollOffset + 1) + L"-" +
            std::to_wstring(std::min(state.pageLines.size(),
                state.scrollOffset + static_cast<size_t>(capacity))) +
            L" / " + std::to_wstring(state.pageLines.size());
        p.Text(std::max(2, p.cols - static_cast<int>(pos.size()) - 3), 5,
               pos, g_pal.muted);
    }

    // Suggestions overlay.
    DrawSuggestions(p, state);

    // Status line (second to last row).
    std::wstring status = g_consoleStatus;
    if (g_loadRunning.load()) {
        const uint64_t got = g_loadDownloaded.load();
        status = L"Загрузка… " +
                 std::to_wstring(got / 1024) + L" КБ   Esc — отмена   " + status;
    } else if (state.editingField >= 0) {
        status = L"Enter — сохранить   Ctrl+Enter — отправить   Esc — отменить";
    } else if (state.hoveredItem >= 0 &&
               static_cast<size_t>(state.hoveredItem) < ItemCount(state)) {
        const int hi = state.hoveredItem;
        if (static_cast<size_t>(hi) < g_currentLinks.size())
            status = g_currentLinks[static_cast<size_t>(hi)].url;
        else if (static_cast<size_t>(hi) <
                 g_currentLinks.size() + g_currentImages.size() +
                     state.visibleFields.size()) {
            const int img = ImageIndexForItem(state, hi);
            if (img >= 0)
                status = g_currentImages[static_cast<size_t>(img)].url;
        }
    }
    if (!status.empty())
        p.Text(2, footerRow, GuiShorten(status,
            static_cast<size_t>(p.cols - 4)),
            g_loadRunning.load() ? g_pal.accent : g_pal.muted);

    // Hint footer on the last row.
    DrawCentered(p, p.rows - 1,
        L"j/k — ссылки   Enter — открыть   o — адрес   d — скачать   Ctrl+J — загрузки   Esc — назад",
        g_pal.muted);

    EndPaint(hwnd, &ps);
}

} // namespace gui
