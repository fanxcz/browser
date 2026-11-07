#pragma once

// gui.h — lightweight terminal-style GUI for the console browser core.
// The window renders the same reader-mode pages as the console UI and is
// driven primarily by the keyboard (mouse works too).

#include "browser.h"

#include <unordered_map>

namespace gui {

enum class Focus { Search, Page };

struct PageLine {
    std::wstring text;
    int itemIndex = -1;  // >=0: selectable item (links, fields, images)
    bool heading = false;
    bool bold = false;
    int imageIndex = -1;   // >=0: first row of an inline image block
    int imageRows = 0;     // rows the block occupies (incl. this one)
    bool imageCovered = false; // continuation row of an image block
};

// A decoded inline image plus its async-load state.
struct GuiImage {
    std::wstring url;
    std::vector<std::array<uint8_t, 3>> rgb;  // RGB on white
    uint32_t w = 0, h = 0;
    int state = 0;        // 0 = queued/loading, 1 = ready, 2 = failed
    HBITMAP dib = nullptr;
    bool dibReady = false;
};

// Absolute pageLines range covered by an inline image.
struct ImageSpan {
    int start = 0;    // pageLines index of the block's first row
    int rows = 0;
    int image = 0;    // index into GuiState::images
};

struct ItemRow {
    int row = 0;    // window row (cells)
    int item = 0;
};

struct Hit {
    int left = 0, right = 0;
    int index = 0;      // suggestion/bookmark index, or item/tab index
    int row = 0;
    bool isClose = false;
    bool isNew = false;
};

struct Rect {
    int left = 0, top = 0, right = 0, bottom = 0;
    bool Contains(int x, int y) const {
        return x >= left && x <= right && y >= top && y <= bottom;
    }
};

// Terminal palette (dark). COLORREF values.
struct Palette {
    COLORREF bg;
    COLORREF text;
    COLORREF bright;
    COLORREF muted;
    COLORREF accent;
    COLORREF hover;
    COLORREF selectedBg;
    COLORREF selectedText;
    COLORREF box;
    COLORREF warn;
};

extern Palette g_pal;
extern Palette g_palDark;
extern Palette g_palLight;
extern bool g_lightTheme;

struct GuiState {
    HWND hwnd = nullptr;
    HFONT font = nullptr;
    HFONT fontBold = nullptr;

    // Cell metrics (pixels per character cell).
    int cellW = 9;
    int cellH = 18;
    int cols = 100;
    int rows = 30;

    // ---- input / omnibox ----
    std::wstring input;
    size_t caret = 0;
    size_t inputViewStart = 0;
    Focus focus = Focus::Search;

    // ---- page interaction ----
    int selectedItem = -1;   // links [0,L), fields, images — like console UI
    int hoveredItem = -1;
    bool findMode = false;
    std::wstring findQuery;
    size_t findPos = 0;
    int editingField = -1;   // g_currentFields index being edited

    std::vector<size_t> visibleFields;
    std::vector<int> itemPageRows;      // item -> index into pageLines
    std::vector<PageLine> pageLines;
    std::vector<ItemRow> itemRows;      // painted rows -> item
    size_t scrollOffset = 0;
    int pageCapacity = 0;

    // ---- autocomplete ----
    std::vector<std::wstring> suggestions;
    std::wstring suggestSource;
    int suggestIndex = -1;
    int hoverSuggest = -1;
    std::vector<Hit> suggestHits;
    std::vector<Hit> bookmarkHits;

    // ---- input history ----
    std::vector<std::wstring> inputHistory;
    size_t historyIndex = 0;
    std::wstring historyDraft;
    bool browsingHistory = false;

    // ---- hit areas for mouse ----
    std::vector<Hit> tabHits;
    std::vector<Hit> navHits;           // index into NavAction
    Rect searchBox;
    Rect searchButton;

    // ---- inline images ----
    std::vector<GuiImage> images;
    std::unordered_map<std::wstring, int> imageByUrl;
    std::vector<ImageSpan> imageSpans;

    // ---- meta refresh ----
    int refreshSeconds = 0;
    int refreshInterval = 0;
    std::string refreshUrl;
    std::wstring refreshScanUrl;
    Clock::time_point refreshAt{};

    // ---- image viewer ----
    bool viewingImage = false;
    int viewImage = -1;              // index into images
    std::wstring viewImageUrl;
    std::wstring viewImageCaption;

    // blink phase for the caret (0/1), advanced by the UI timer.
    bool caretOn = true;
};

enum class NavAction { Back, Forward, Home, Reload };

// gui_render.cpp
void RebuildVisibleFields(GuiState& state);
size_t ItemCount(const GuiState& state);
int ImageIndexForItem(const GuiState& state, int item);
int FieldIndexForItem(const GuiState& state, int item);
int ItemIndexForField(const GuiState& state, int fieldIdx);
TabViewState CurrentView(const GuiState& state);
void ApplyView(GuiState& state, const TabViewState& view);
std::wstring GuiShorten(std::wstring value, size_t maxChars);
std::vector<std::wstring> GuiWrap(const std::wstring& source,
                                  size_t lineWidth, size_t maxLines);
void BuildPageLines(GuiState& state);
void RenderFrame(HWND hwnd, GuiState& state);
void InvalidateGui();

// gui_input.cpp
void GuiSubmit(GuiState& state);
void GuiHandleKey(GuiState& state, WPARAM vk, bool shift, bool ctrl, bool alt,
                  bool& running);
void GuiHandleChar(GuiState& state, wchar_t ch);
void GuiHandleMouseMove(GuiState& state, int x, int y);
void GuiHandleClick(GuiState& state, int x, int y);
void GuiHandleWheel(GuiState& state, int delta);
void GuiTick(GuiState& state);

// gui_images.cpp — async inline image loading (WIC decode → DIB).
int GuiImageIndex(GuiState& state, const std::wstring& url);
bool GuiSyncImages(GuiState& state);
void GuiImageDisplaySize(const GuiState& state, const GuiImage& img,
                         int& wPx, int& hPx);
void GuiPaintImage(HDC hdc, GuiImage& img, int x, int y, int w, int h);
void GuiClearImages(GuiState& state);
void GuiShutdownImages();

// Theme (settings.txt) + image viewer.
void GuiLoadTheme();
void GuiToggleTheme();
void GuiOpenImageViewer(GuiState& state, const std::wstring& url,
                        const std::string& alt);

// gui_main.cpp
int GuiMain(HINSTANCE hInstance, int nCmdShow);

} // namespace gui
