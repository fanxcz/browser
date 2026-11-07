// gui_images.cpp — asynchronous inline image pipeline for the GUI.
// URLs found in page text are queued to a worker that decodes them via
// WIC (DecodeImagePixels); finished bitmaps are uploaded to a DIB on
// the UI thread and painted with StretchBlt.

#include "gui.h"

#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace gui {

namespace {

struct DecodedImage {
    std::vector<std::array<uint8_t, 3>> rgb;
    uint32_t w = 0, h = 0;
    bool ok = false;
};

std::mutex g_imgMutex;
std::condition_variable g_imgCv;
std::vector<std::wstring> g_imgQueue;
std::unordered_map<std::wstring, DecodedImage> g_imgResults;
std::unordered_set<std::wstring> g_imgQueued;
bool g_imgStop = false;
std::thread g_imgThread;
bool g_imgThreadStarted = false;

void ImageWorker() {
    for (;;) {
        std::wstring url;
        {
            std::unique_lock<std::mutex> lock(g_imgMutex);
            g_imgCv.wait(lock, [] {
                return g_imgStop || !g_imgQueue.empty();
            });
            if (g_imgStop) return;
            url = std::move(g_imgQueue.front());
            g_imgQueue.erase(g_imgQueue.begin());
        }

        DecodedImage decoded;
        // Generous bounds; the UI scales down to the content width anyway.
        decoded.ok = DecodeImagePixels(url, 1600, 900,
                                       decoded.rgb, decoded.w, decoded.h);

        {
            std::lock_guard<std::mutex> lock(g_imgMutex);
            g_imgResults[url] = std::move(decoded);
        }
    }
}

void EnsureWorker() {
    std::lock_guard<std::mutex> lock(g_imgMutex);
    if (!g_imgThreadStarted) {
        g_imgThreadStarted = true;
        g_imgThread = std::thread(ImageWorker);
    }
}

} // namespace

// Get (or create + enqueue) the cache slot for an image URL.
int GuiImageIndex(GuiState& state, const std::wstring& url) {
    const auto it = state.imageByUrl.find(url);
    if (it != state.imageByUrl.end())
        return it->second;

    // Bound the cache: drop everything when it grows too large.
    if (state.images.size() >= 64) {
        GuiClearImages(state);
    }

    GuiImage img;
    img.url = url;
    img.state = 0;
    const int index = static_cast<int>(state.images.size());
    state.images.push_back(std::move(img));
    state.imageByUrl[url] = index;

    {
        std::lock_guard<std::mutex> lock(g_imgMutex);
        if (g_imgQueued.insert(url).second)
            g_imgQueue.push_back(url);
    }
    g_imgCv.notify_one();
    EnsureWorker();
    return index;
}

// Move finished decode results into the UI-side cache. Returns true when
// any image changed state (caller should rebuild page lines).
bool GuiSyncImages(GuiState& state) {
    std::vector<std::pair<std::wstring, DecodedImage>> ready;
    {
        std::lock_guard<std::mutex> lock(g_imgMutex);
        for (auto& [url, decoded] : g_imgResults) {
            auto it = state.imageByUrl.find(url);
            if (it != state.imageByUrl.end() &&
                state.images[static_cast<size_t>(it->second)].state == 0)
                ready.emplace_back(url, std::move(decoded));
        }
        for (const auto& [url, _] : ready)
            g_imgResults.erase(url);
    }
    if (ready.empty()) return false;

    for (const auto& [url, decoded] : ready) {
        GuiImage& img = state.images[static_cast<size_t>(
            state.imageByUrl[url])];
        img.rgb = std::move(decoded.rgb);
        img.w = decoded.w;
        img.h = decoded.h;
        img.state = decoded.ok ? 1 : 2;
    }
    return true;
}

// Pixel size the image occupies in the content area (aspect preserved,
// fits the column width, capped to ~26 rows tall).
void GuiImageDisplaySize(const GuiState& state, const GuiImage& img,
                         int& wPx, int& hPx) {
    if (!img.w || !img.h) { wPx = hPx = 0; return; }
    const int maxW = std::max(64, (state.cols - 8) * state.cellW);
    const int maxH = 26 * state.cellH;
    const double scale = std::min(
        {1.0, static_cast<double>(maxW) / img.w,
         static_cast<double>(maxH) / img.h});
    wPx = std::max(1, static_cast<int>(img.w * scale));
    hPx = std::max(1, static_cast<int>(img.h * scale));
}

// Lazily build a top-down DIB from the RGB buffer, then blit scaled.
void GuiPaintImage(HDC hdc, GuiImage& img, int x, int y, int w, int h) {
    if (img.state != 1 || !img.w || !img.h) return;

    if (!img.dibReady) {
        BITMAPINFOHEADER bi{};
        bi.biSize = sizeof(bi);
        bi.biWidth = static_cast<LONG>(img.w);
        bi.biHeight = -static_cast<LONG>(img.h); // top-down
        bi.biPlanes = 1;
        bi.biBitCount = 32;
        bi.biCompression = BI_RGB;

        void* bits = nullptr;
        img.dib = CreateDIBSection(nullptr, reinterpret_cast<BITMAPINFO*>(&bi),
                                   DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!img.dib || !bits) {
            img.dibReady = true; // don't retry every frame
            return;
        }
        auto* dst = static_cast<BYTE*>(bits);
        const size_t stride = static_cast<size_t>(img.w) * 4;
        for (uint32_t py = 0; py < img.h; ++py) {
            BYTE* row = dst + static_cast<size_t>(py) * stride;
            for (uint32_t px = 0; px < img.w; ++px) {
                const auto& c = img.rgb[static_cast<size_t>(py) * img.w + px];
                row[px * 4 + 0] = static_cast<BYTE>(c[2]); // B
                row[px * 4 + 1] = static_cast<BYTE>(c[1]); // G
                row[px * 4 + 2] = static_cast<BYTE>(c[0]); // R
                row[px * 4 + 3] = 255;
            }
        }
        img.dibReady = true;
    }
    if (!img.dib) return;

    HDC mem = CreateCompatibleDC(hdc);
    HGDIOBJ old = SelectObject(mem, img.dib);
    SetStretchBltMode(hdc, HALFTONE);
    SetBrushOrgEx(hdc, 0, 0, nullptr);
    StretchBlt(hdc, x, y, w, h, mem, 0, 0,
               static_cast<int>(img.w), static_cast<int>(img.h), SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);
}

void GuiClearImages(GuiState& state) {
    for (auto& img : state.images)
        if (img.dib) { DeleteObject(img.dib); img.dib = nullptr; }
    state.images.clear();
    state.imageByUrl.clear();
    // imageSpans is owned by BuildPageLines — it is cleared there.
}

void GuiShutdownImages() {
    {
        std::lock_guard<std::mutex> lock(g_imgMutex);
        g_imgStop = true;
    }
    g_imgCv.notify_all();
    if (g_imgThread.joinable())
        g_imgThread.join();
    g_imgThreadStarted = false;
}

} // namespace gui
