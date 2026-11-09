#include "ui_int.h"

namespace console_ui {
    void ClearSelection(State& state) {
        state.selectMode = false;
        state.selAnchor = -1;
        state.selCursor = -1;
        state.selLines.clear();
    }

    bool HasSelection(const State& state) {
        return state.selAnchor >= 0 && state.selCursor >= 0 &&
               state.selAnchor != state.selCursor;
    }

    void ClearInputSel(State& state) { state.inputSelAnchor = -1; }

    bool HasInputSel(const State& state) {
        return state.inputSelAnchor >= 0 &&
               state.inputSelAnchor <= static_cast<long>(state.input.size()) &&
               static_cast<size_t>(state.inputSelAnchor) != state.caret;
    }

    std::pair<size_t, size_t> InputSelRange(const State& state) {
        const size_t a = static_cast<size_t>(state.inputSelAnchor);
        const size_t b = state.caret;
        return {std::min(a, b), std::max(a, b)};
    }

    void DeleteInputSel(State& state) {
        if (!HasInputSel(state)) return;
        const auto [lo, hi] = InputSelRange(state);
        state.input.erase(lo, hi - lo);
        state.caret = lo;
        ClearInputSel(state);
        state.browsingHistory = false;
    }

    // Snapshot the omnibox/field editor before a mutation so Ctrl+Z can
    // restore it. Capped — long typing sessions don't grow it forever.
    void PushInputUndo(State& state) {
        if (state.inputUndo.size() >= 200)
            state.inputUndo.erase(state.inputUndo.begin());
        state.inputUndo.push_back({state.input, state.caret});
    }

    void UndoInput(State& state) {
        if (state.inputUndo.empty()) {
            g_consoleStatus = L"Нечего отменять.";
            return;
        }
        const auto [text, caret] = state.inputUndo.back();
        state.inputUndo.pop_back();
        state.input = text;
        state.caret = std::min(caret, text.size());
        state.inputViewStart = 0;
        state.browsingHistory = false;
        state.suggestIndex = -1;
        ClearInputSel(state);
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

    std::wstring ScreenSelectionText(const State& state); // defined below

    void CopySelectionOrUrl(State& state) {
        // 1) Terminal-style screen selection (any dragged cells).
        const std::wstring screen = TrimWide(ScreenSelectionText(state));
        if (!screen.empty()) {
            if (UiCopyToClipboard(screen)) {
                g_consoleStatus = L"Выделенный текст скопирован (" +
                    std::to_wstring(screen.size()) + L" симв.).";
                return;
            }
        }
        // 2) Page-line selection (V-mode).
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

    void HandleLeftClick(State& state, int x, int y); // defined below

    bool IsWordChar(wchar_t ch) {
        return std::iswalnum(ch) || ch == L'_' || ch == L'-' ||
               ch == L'@' || ch == L'.' || ch == L'/' || ch == L':' ||
               ch == L'?' || ch == L'=' || ch == L'&' || ch == L'%' ||
               ch == L'+' || ch == L'#';
    }

    // Double-click: select the word under (x, y) as a screen rect.
    void SelectWordAt(State& state, int x, int y) {
        const size_t need =
            (static_cast<size_t>(y) + 1) * state.width;
        if (y < 0 || y >= state.height || state.cells.size() < need ||
            x < 0 || x >= state.width) return;
        const CHAR_INFO* row = state.cells.data() +
            static_cast<size_t>(y) * state.width;
        if (!IsWordChar(row[x].Char.UnicodeChar)) return;
        int cx = x;
        while (cx > 0 && IsWordChar(row[cx - 1].Char.UnicodeChar)) --cx;
        int ex = x;
        while (ex + 1 < state.width &&
               IsWordChar(row[ex + 1].Char.UnicodeChar)) ++ex;
        state.ssX1 = cx; state.ssY1 = y;
        state.ssX2 = ex; state.ssY2 = y;
        state.screenSelValid = true;
        state.selAnchor = state.selCursor = -1;
        g_consoleStatus = L"Слово выделено — c / Ctrl+C копирует.";
    }

    // Triple-click: select the whole page line under y (falls back to
    // the screen row when outside the content area).
    void SelectLineAt(State& state, int y) {
        const int contentTop = 6;
        const int cap = state.pageCapacity > 0
            ? state.pageCapacity : state.height - 9;
        if (g_consolePageOpen && y >= contentTop &&
            y < contentTop + cap && state.pageLineCount > 0) {
            const long lineIdx = std::min(
                static_cast<long>(state.scrollOffset) + (y - contentTop),
                static_cast<long>(state.pageLineCount) - 1);
            state.selAnchor = state.selCursor = lineIdx;
            state.selectMode = false;
            state.screenSelValid = false;
            g_consoleStatus = L"Строка выделена — c / Ctrl+C копирует.";
            return;
        }
        state.ssX1 = 0; state.ssY1 = y;
        state.ssX2 = state.width - 1; state.ssY2 = y;
        state.screenSelValid = true;
        state.selAnchor = state.selCursor = -1;
        g_consoleStatus = L"Строка выделена — c / Ctrl+C копирует.";
    }


    // Fire a deferred click: Ctrl+click opens the link/image under the
    // cursor in a new tab, otherwise normal click dispatch.
    void DispatchPendingClick(State& state) {
        if (!state.pendingClickValid) return;
        state.pendingClickValid = false;
        const int x = state.pendingClickX;
        const int y = state.pendingClickY;
        if (state.pendingClickCtrl) {
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
            return; // Ctrl+click on empty space does nothing
        }
        HandleLeftClick(state, x, y);
    }

    // End a drag: tiny movement = plain click (deferred for multi-click
    // detection), otherwise commit the rectangle as screen selection.
    void FinishScreenSel(State& state, int ex, int ey) {
        state.selEX = ex;
        state.selEY = ey;
        state.mouseSelDown = false;
        const int ddx = std::abs(ex - state.selSX);
        const int ddy = std::abs(ey - state.selSY);
        // Jitter under ~3 cells is a click, not a selection — otherwise
        // every ordinary click would "select" a couple of characters.
        if (ddx + ddy <= 3) {
            // Multi-click presses are consumed on press (word/line
            // selection); only a genuine first click gets deferred.
            if (state.clickCount <= 1) {
                state.pendingClickValid = true;
                state.pendingClickX = state.selSX;
                state.pendingClickY = state.selSY;
                state.pendingClickCtrl = state.clickCtrl;
                state.pendingClickTime = Clock::now();
            }
        } else {
            state.ssX1 = std::min(state.selSX, ex);
            state.ssY1 = std::min(state.selSY, ey);
            state.ssX2 = std::max(state.selSX, ex);
            state.ssY2 = std::max(state.selSY, ey);
            state.screenSelValid = true;
            state.clickCount = 0; // a drag is not a click streak
            g_consoleStatus = L"Выделено — c / Ctrl+C копирует.";
        }
    }

    // Read the committed screen-selection rectangle straight from the
    // console buffer — works over links, buttons, status bar, everything.
    std::wstring ScreenSelectionText(const State& state) {
        if (!state.screenSelValid) return {};
        const int x1 = std::min(state.ssX1, state.ssX2);
        const int x2 = std::max(state.ssX1, state.ssX2);
        const int y1 = std::min(state.ssY1, state.ssY2);
        const int y2 = std::max(state.ssY1, state.ssY2);
        const size_t cellCount =
            static_cast<size_t>(state.width) * state.height;
        if (state.cells.size() < cellCount) return {};
        std::wstring out;
        for (int y = y1; y <= y2; ++y) {
            const int w = x2 - x1 + 1;
            if (w <= 0) continue;
            std::wstring buf(static_cast<size_t>(w), L' ');
            const CHAR_INFO* row = state.cells.data() +
                static_cast<size_t>(y) * state.width + x1;
            for (int i = 0; i < w; ++i) buf[static_cast<size_t>(i)] = row[i].Char.UnicodeChar;
            size_t end = buf.size();
            while (end > 0 && buf[end - 1] == L' ') --end;
            buf.resize(end);
            if (!out.empty()) out += L'\n';
            out += buf;
        }
        return out;
    }

    // Repaint the selection rectangle with the selection attribute —
    // reads the freshly drawn frame back from the buffer, so it overlays
    // links, fields, status bar and tabs alike.
    void DrawScreenSelection(State& state) {
        int x1, y1, x2, y2;
        if (state.mouseSelDown) {
            x1 = std::min(state.selSX, state.selEX);
            x2 = std::max(state.selSX, state.selEX);
            y1 = std::min(state.selSY, state.selEY);
            y2 = std::max(state.selSY, state.selEY);
        } else if (state.screenSelValid) {
            x1 = std::min(state.ssX1, state.ssX2);
            x2 = std::max(state.ssX1, state.ssX2);
            y1 = std::min(state.ssY1, state.ssY2);
            y2 = std::max(state.ssY1, state.ssY2);
        } else {
            return;
        }
        x1 = std::clamp(x1, 0, state.width - 1);
        x2 = std::clamp(x2, 0, state.width - 1);
        y1 = std::clamp(y1, 0, state.height - 1);
        y2 = std::clamp(y2, 0, state.height - 1);
        if (x1 == x2 && y1 == y2) return;
        const size_t cellCount =
            static_cast<size_t>(state.width) * state.height;
        if (state.cells.size() < cellCount) return;
        for (int y = y1; y <= y2; ++y) {
            const int w = x2 - x1 + 1;
            std::wstring buf(static_cast<size_t>(w), L' ');
            const CHAR_INFO* row = state.cells.data() +
                static_cast<size_t>(y) * state.width + x1;
            for (int i = 0; i < w; ++i) buf[static_cast<size_t>(i)] = row[i].Char.UnicodeChar;
            At(state, x1, y, buf, kSelected, w);
        }
    }
}
