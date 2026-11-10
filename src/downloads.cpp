// downloads.cpp — download manager (queue, parallel workers, resume,
// SHA-256 sidecar verification) and the browser://downloads page.
// Split out of app.cpp.

#include "browser.h"

#include <deque>
#include <shellapi.h>
#include <unordered_map>

struct DownloadJob {
    std::wstring url;
    std::string filename; // UTF-8
};
// Per-download state: own cancel flag + progress, so up to
// MAX_PARALLEL_DOWNLOADS files can run concurrently.
struct ActiveDownload {
    std::wstring filename;
    std::shared_ptr<std::atomic_bool> cancel =
        std::make_shared<std::atomic_bool>(false);
    std::atomic_uint64_t progress{0};
    std::atomic_bool done{false};
};
std::mutex g_dlQueueMutex;
std::deque<DownloadJob> g_dlQueue;
std::mutex g_dlActiveMutex;
std::vector<std::shared_ptr<ActiveDownload>> g_activeDownloads;
constexpr size_t MAX_PARALLEL_DOWNLOADS = 3;

// ---------------------------------------------------------------------------
// Streaming background downloads
// ---------------------------------------------------------------------------

static std::wstring DefaultDownloadName(const std::wstring& url) {
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);

    if (WinHttpCrackUrl(url.c_str(), 0, 0, &parts) && parts.dwUrlPathLength) {
        std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
        const size_t slash = path.find_last_of(L'/');
        std::wstring name = slash == std::wstring::npos
                                ? path : path.substr(slash + 1);
        // Strip query string if it leaked into the path portion.
        const size_t query = name.find(L'?');
        if (query != std::wstring::npos) name.resize(query);
        if (!name.empty() && name != L"/")
            return name;
    }

    return L"download-" + ToWide(HashUrl(url)) + L".bin";
}

// Route download messages to the status line in console-UI mode instead of
// writing raw text over the rendered page.
static void ReportDownload(const std::string& utf8) {
    if (g_consoleUiMode) {
        g_consoleStatus = L"Загрузка: " + ToWide(utf8);
    } else {
        PrintLine(utf8);
    }
}


// Extract a usable filename from a Content-Disposition header (handles
// filename= and RFC 5987 filename*=UTF-8''...). Returns "" when absent
// or unsafe (path components, traversal, control chars).
static std::string ContentDispositionFilename(const std::wstring& header) {
    if (header.empty()) return {};
    const std::string h = ToUtf8(header);
    const std::string low = LowerAscii(h);
    std::string name;

    size_t p = low.find("filename*=");
    if (p != std::string::npos) {
        std::string v = h.substr(p + 10);
        const size_t semi = v.find(';');
        if (semi != std::string::npos) v.resize(semi);
        v = Trim(v);
        // Strip "charset'lang'" prefix of the extended notation.
        const size_t q1 = v.find('\'');
        if (q1 != std::string::npos) {
            const size_t q2 = v.find('\'', q1 + 1);
            v = q2 != std::string::npos ? v.substr(q2 + 1)
                                      : v.substr(q1 + 1);
        }
        name = PercentDecode(v);
    }
    if (name.empty()) {
        p = low.find("filename=");
        if (p == std::string::npos) return {};
        std::string v = h.substr(p + 9);
        const size_t semi = v.find(';');
        if (semi != std::string::npos) v.resize(semi);
        v = Trim(v);
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
            v = v.substr(1, v.size() - 2);
        name = v;
    }

    for (const char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 32 || c == '/' || c == '\\' || c == ':')
            return {};
    }
    if (name.empty() || name == "." || name == ".." ||
        name.find("..") != std::string::npos)
        return {};
    return name;
}

// Extract the first 64-hex-digit token from a checksum file body.
static std::string ExtractSha256(const std::string& text) {
    std::string hex;
    for (char ch : text) {
        const char c = static_cast<char>(std::tolower(
            static_cast<unsigned char>(ch)));
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))
            hex.push_back(c);
        else if (!hex.empty())
            break;
    }
    return hex.size() == 64 ? hex : std::string();
}

// Fetch "<fileUrl>.sha256.txt" (or ".sha256") and verify the downloaded
// file against it. Silent when no checksum is published.
static void VerifyDownloadChecksum(const std::wstring& fileUrl,
                                   const fs::path& savedPath,
                                   const std::string& fileName) {
    std::string expected;
    for (const wchar_t* suffix : {L".sha256.txt", L".sha256"}) {
        Response sum = Request(fileUrl + suffix, CacheMode::Bypass,
                               nullptr);
        if (!sum.ok || sum.body.empty() || sum.body.size() > 8192)
            continue;
        const std::string text(sum.body.begin(), sum.body.end());
        expected = ExtractSha256(text);
        if (!expected.empty())
            break;
    }
    if (expected.empty())
        return;

    std::array<BYTE, 32> digest{};
    if (!Sha256File(savedPath, digest))
        return;

    const std::string actual = Sha256Hex(digest);
    if (actual == expected) {
        ReportDownload("SHA-256 OK: " + fileName);
        // Persist the result so the downloads page can show a ✓ mark later.
        std::error_code ec;
        const uintmax_t size = fs::file_size(savedPath, ec);
        std::wofstream vf(DataDirPath() / L"verified.txt",
                          std::ios::app);
        if (vf)
            vf << ToWide(fileName) << L'\t' << size << L'\t'
               << ToWide(actual) << L'\n';
    } else {
        ReportDownload("SHA-256 MISMATCH for " + fileName +
                       " — file may be corrupt!");
    }
}

// Loads "name<TAB>size<TAB>sha256" records written by VerifyDownloadChecksum.
static std::unordered_map<std::wstring, std::pair<uintmax_t, std::wstring>>
LoadVerifiedDownloads() {
    std::unordered_map<std::wstring, std::pair<uintmax_t, std::wstring>> out;
    std::wifstream in(DataDirPath() / L"verified.txt");
    std::wstring line;
    while (std::getline(in, line)) {
        const size_t t1 = line.find(L'\t');
        const size_t t2 = t1 == std::wstring::npos
                              ? std::wstring::npos : line.find(L'\t', t1 + 1);
        if (t1 == std::wstring::npos || t2 == std::wstring::npos) continue;
        try {
            out[line.substr(0, t1)] = {
                std::stoull(line.substr(t1 + 1, t2 - t1 - 1)),
                line.substr(t2 + 1)};
        } catch (...) {}
    }
    return out;
}

static void DownloadWorker(std::wstring url, std::string filename,
                           const std::shared_ptr<ActiveDownload>& act) {
    // filename arrives as UTF-8; convert to wide for correct Unicode paths.
    fs::path destination = DownloadsDirPath() / ToWide(filename);
    fs::path temporary = fs::path(destination.string() + ".part");

    // Resume support: an existing .part file means a previous download was
    // interrupted — ask the server for the remaining bytes via Range.
    uint64_t resumeFrom = 0;
    {
        std::error_code ec;
        if (fs::exists(temporary, ec))
            resumeFrom = fs::file_size(temporary, ec);
        if (ec || resumeFrom == 0)
            resumeFrom = 0;
    }
    // Range resumes need a dedicated session WITHOUT transparent
    // decompression: on g_session WinHTTP auto-negotiates gzip, and a
    // gzip-encoded 206 cannot be inflated mid-stream (read aborts).
    HINTERNET dlSession = nullptr;
    if (resumeFrom > 0) {
        dlSession = WinHttpOpen(L"Browser-Downloads/1.0",
                                WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
        if (dlSession)
            WinHttpSetTimeouts(dlSession, 5000, 5000, 15000, 30000);
    }
    struct SessionGuard {
        HINTERNET h = nullptr;
        ~SessionGuard() { if (h) WinHttpCloseHandle(h); }
    } sessionGuard{dlSession};

    // Follow redirects manually (CreateRequest disables WinHTTP's own
    // redirect handling so page loads and downloads behave consistently).
    std::wstring currentUrl = url;
    RequestHandles handles;
    const auto addRangeHeader = [&]() {
        if (resumeFrom == 0) return;
        const std::wstring range = L"Range: bytes=" +
            std::to_wstring(resumeFrom) + L"-";
        WinHttpAddRequestHeaders(handles.request, range.c_str(),
                                 static_cast<DWORD>(-1),
                                 WINHTTP_ADDREQ_FLAG_ADD |
                                 WINHTTP_ADDREQ_FLAG_REPLACE);
        // REPLACE (not ADD): WinHTTP auto-adds "Accept-Encoding: gzip,
        // deflate" because session decompression is on. A gzip-encoded 206
        // cannot be decompressed mid-stream and aborts the read, so the
        // auto header must be overridden, not duplicated.
        WinHttpAddRequestHeaders(
            handles.request, L"Accept-Encoding: identity",
            static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_REPLACE);
    };
    DWORD status = 0;
    bool requestReady = false;

    for (size_t hop = 0; hop < MAX_REDIRECTS && !requestReady; ++hop) {
        CloseRequest(handles);
        if (!CreateRequest(currentUrl, handles, L"GET", dlSession)) {
            ReportDownload("Download: cannot create HTTP request.");

            return;
        }
        addRangeHeader();

        g_networkRequests.fetch_add(1);

        BOOL sent = WinHttpSendRequest(
            handles.request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0);

        if (sent)
            sent = WinHttpReceiveResponse(handles.request, nullptr);

        // A pooled keep-alive connection can be silently closed by the
        // server while idle — retry once on a fresh connection.
        if (!sent && handles.borrowedConnection) {
            handles.discardConnection = true;
            CloseRequest(handles);
            if (CreateRequest(currentUrl, handles, L"GET", dlSession)) {
                addRangeHeader();
                g_networkRequests.fetch_add(1);
                sent = WinHttpSendRequest(
                    handles.request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                    WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
                if (sent)
                    sent = WinHttpReceiveResponse(handles.request, nullptr);
            }
        }

        if (!sent) {
            CloseRequest(handles);
            ReportDownload("Download: network request failed.");

            return;
        }

        DWORD statusSize = sizeof(status);
        status = 0;
        WinHttpQueryHeaders(
            handles.request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status, &statusSize, WINHTTP_NO_HEADER_INDEX);

        const bool redirect = status >= 300 && status < 400 &&
                              status != 304 /*Not Modified*/;
        if (!redirect) {
            requestReady = true;
            break;
        }

        const std::wstring location =
            QueryHeaderValue(handles.request, L"Location");
        const std::wstring next = ResolveRedirectUrl(currentUrl, location);
        if (next.empty())
            break; // give up: report the redirect status below
        currentUrl = next;
    }

    // 416 = "range not satisfiable": the .part already holds the whole file
    // (e.g. it finished but the rename never happened). Promote it directly.
    if (requestReady && status == 416 && resumeFrom > 0) {
        CloseRequest(handles);
        std::error_code ec;
        fs::remove(destination, ec);
        fs::rename(temporary, destination, ec);
        if (ec) {
            ReportDownload("Download: .part есть, но переименовать не удалось.");
        } else {
            ReportDownload("Downloaded (resumed, already complete): " +
                           ToUtf8(destination.wstring()) + " (" +
                           std::to_string(resumeFrom) + " bytes)");
            VerifyDownloadChecksum(url, destination, filename);
        }

        return;
    }

    if (!requestReady || status < 200 || status >= 300) {
        CloseRequest(handles);
        ReportDownload("Download failed. HTTP " + std::to_string(status));

        return;
    }

    // 206 = partial content (resume accepted); 200 = full body — if we asked
    // for a range and got 200, the server ignored it: start over cleanly.
    const bool resumed = resumeFrom > 0 && status == 206;
    const uint64_t baseBytes = resumed ? resumeFrom : 0;
    if (resumeFrom > 0 && !resumed)
        ReportDownload("Сервер не поддерживает докачку — начинаю с нуля.");

    uint64_t totalBytes = 0;
    const std::wstring contentLength =
        QueryHeader(handles.request, WINHTTP_QUERY_CONTENT_LENGTH);
    if (!contentLength.empty()) {
        try { totalBytes = std::stoull(contentLength); }
        catch (...) { totalBytes = 0; }
    }
    if (resumed)
        totalBytes += baseBytes; // Content-Length on 206 is the remainder

    // Content-Disposition can carry a better name than the URL path
    // (e.g. download.php?file=123 → report.pdf). Skipped while resuming —
    // the .part file is keyed to the originally requested name.
    if (resumeFrom == 0) {
        const std::string disp = ContentDispositionFilename(
            QueryHeaderValue(handles.request, L"Content-Disposition"));
        if (!disp.empty() && disp != filename) {
            filename = disp;
            destination = DownloadsDirPath() / ToWide(filename);
            temporary = fs::path(destination.string() + ".part");
        }
    }

    std::ofstream file(temporary,
                       std::ios::binary | (resumed ? std::ios::app
                                                   : std::ios::trunc));
    if (!file) {
        CloseRequest(handles);
        ReportDownload("Download: cannot create output file.");

        return;
    }

    uint64_t receivedTotal = 0;
    uint64_t lastPrintedBytes = 0;
    const auto started = Clock::now();
    bool readOk = true;
    DWORD readErr = 0;
    std::vector<char> buffer(DOWNLOAD_BUFFER_SIZE);

    // WinHttpReadData directly: synchronous reads return TRUE with
    // dwBytesRead == 0 at end of body. (QueryDataAvailable misbehaves on
    // some 206 partial responses.)
    while (!act->cancel->load()) {
        if (baseBytes + receivedTotal >= MAX_DOWNLOAD_BYTES) {
            readErr = ERROR_DISK_FULL;
            readOk = false;
            break;
        }
        DWORD received = 0;
        if (!WinHttpReadData(handles.request, buffer.data(),
                             static_cast<DWORD>(buffer.size()), &received)) {
            readErr = GetLastError();
            readOk = false;
            break;
        }
        if (received == 0)
            break; // end of body

        file.write(buffer.data(), static_cast<std::streamsize>(received));
        if (!file) {
            readErr = ERROR_WRITE_FAULT;
            readOk = false;
            break;
        }

        const uint64_t totalReceived = baseBytes + receivedTotal + received;
        receivedTotal += received;
        act->progress.store(baseBytes + receivedTotal);
        g_downloadedBytes.fetch_add(received);

        const auto now = Clock::now();
        const double seconds =
            std::max(0.001, std::chrono::duration<double>(now - started).count());
        const double mbPerSecond =
            static_cast<double>(receivedTotal) / (1024.0 * 1024.0) / seconds;

        if (!g_consoleUiMode &&
            (totalReceived - lastPrintedBytes >= 256 * 1024 ||
             (totalBytes > 0 && totalReceived >= totalBytes))) {
            std::lock_guard<std::mutex> lock(g_outputMutex);
            std::cout << "\rDownload: " << std::fixed << std::setprecision(2)
                      << (static_cast<double>(totalReceived) / (1024.0 * 1024.0))
                      << " MB, " << mbPerSecond << " MB/s";

            if (totalBytes > 0) {
                const double percent =
                    100.0 * static_cast<double>(totalReceived) /
                    static_cast<double>(totalBytes);
                std::cout << ", " << std::setprecision(1)
                          << std::min(100.0, percent) << "%";
            }
            std::cout << "       " << std::flush;
            lastPrintedBytes = totalReceived;
        }
    }

    file.close();
    CloseRequest(handles);

    if (act->cancel->load()) {
        // Keep the .part file — a repeated `download` of the same URL will
        // resume from where it stopped (HTTP Range).
        ReportDownload("Download cancelled — часть сохранена (.part), "
                       "повторите download для продолжения.");
    } else if (!readOk || !file || baseBytes + receivedTotal >= MAX_DOWNLOAD_BYTES) {
        std::error_code ec;
        fs::remove(temporary, ec);
        ReportDownload("Download failed while writing or reading data. err=" +
                       std::to_string(readErr) + " got=" +
                       std::to_string(baseBytes + receivedTotal));
    } else {
        std::error_code ec;
        fs::remove(destination, ec);
        fs::rename(temporary, destination, ec);
        if (ec) {
            fs::remove(temporary);
            ReportDownload("Download finished but could not move the file into downloads/.");
        } else {
            const double seconds = std::max(
                0.001, std::chrono::duration<double>(Clock::now() - started).count());
            const double speed =
                static_cast<double>(receivedTotal) / (1024.0 * 1024.0) / seconds;
            std::ostringstream message;
            message << "\nDownloaded: " << ToUtf8(destination.wstring())
                    << " (" << baseBytes + receivedTotal << " bytes, "
                    << std::fixed << std::setprecision(2)
                    << speed << " MB/s)";
            if (resumed)
                message << " [докачано с " << baseBytes << " байт]";
            ReportDownload(message.str());
            UiDownloadToast(destination.filename().wstring());
            // Sites like fanxcz.github.io publish <file>.sha256.txt next to
            // each download — verify integrity when present.
            VerifyDownloadChecksum(url, destination, filename);
        }
    }


}

// Dispatcher: keeps up to MAX_PARALLEL_DOWNLOADS workers busy pulling jobs
// off the queue. Runs until the queue drains and all workers finish.
static void DownloadDispatcher() {
    std::vector<std::pair<std::thread, std::shared_ptr<ActiveDownload>>>
        workers;
    for (;;) {
        // Reap finished workers.
        for (auto it = workers.begin(); it != workers.end();) {
            if (it->second->done.load(std::memory_order_relaxed)) {
                it->first.join();
                it = workers.erase(it);
            } else {
                ++it;
            }
        }
        // Start new jobs while slots are free (unless the queue is paused).
        while (!g_downloadsPaused.load() &&
               workers.size() < MAX_PARALLEL_DOWNLOADS) {
            DownloadJob job;
            {
                std::lock_guard<std::mutex> lock(g_dlQueueMutex);
                if (g_dlQueue.empty())
                    break;
                job = g_dlQueue.front();
                g_dlQueue.pop_front();
            }
            auto act = std::make_shared<ActiveDownload>();
            act->filename = ToWide(job.filename);
            {
                std::lock_guard<std::mutex> lock(g_dlActiveMutex);
                g_activeDownloads.push_back(act);
            }
            workers.emplace_back(
                std::thread([job, act]() {
                    DownloadWorker(job.url, job.filename, act);
                    {
                        std::lock_guard<std::mutex> lock(g_dlActiveMutex);
                        auto& vec = g_activeDownloads;
                        vec.erase(std::remove(vec.begin(), vec.end(), act),
                                  vec.end());
                    }
                    act->done.store(true);
                }),
                act);
        }
        {
            // Exit check under the queue mutex so a concurrent push can't
            // slip between "empty" and "stop running".
            std::lock_guard<std::mutex> lock(g_dlQueueMutex);
            if (workers.empty() && g_dlQueue.empty()) {
                g_downloadRunning.store(false);
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}

// Pause/resume the download queue: in-flight files finish, queued jobs
// wait. Resume continues them automatically (.part files allow Range
// resume if a job had to restart).
void UiToggleDownloadPause() {
    const bool nowPaused = !g_downloadsPaused.exchange(
        !g_downloadsPaused.load());
    if (g_consoleUiMode) {
        g_consoleStatus = nowPaused
            ? L"Загрузки на паузе — очередь ждёт (p — продолжить)."
            : L"Загрузки продолжены.";
    } else {
        std::cout << (nowPaused ? "Downloads paused.\n"
                                : "Downloads resumed.\n");
    }
}

bool DownloadsPaused() { return g_downloadsPaused.load(); }

// Sum of per-file progress across all active downloads (for the status line).
uint64_t TotalDownloadProgress() {
    uint64_t sum = 0;
    std::lock_guard<std::mutex> lock(g_dlActiveMutex);
    for (const auto& act : g_activeDownloads)
        sum += act->progress.load(std::memory_order_relaxed);
    return sum;
}

size_t ActiveDownloadCount() {
    std::lock_guard<std::mutex> lock(g_dlActiveMutex);
    return g_activeDownloads.size();
}

// Request cancellation of every in-flight download (keeps .part files).
void CancelAllDownloads() {
    std::lock_guard<std::mutex> lock(g_dlActiveMutex);
    for (const auto& act : g_activeDownloads)
        act->cancel->store(true);
}

// Cancel a single download by file name — active worker or queued job.
void UiCancelDownloadByName(const std::wstring& name) {
    if (name.empty()) return;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(g_dlActiveMutex);
        for (const auto& act : g_activeDownloads) {
            if (act->filename == name) {
                act->cancel->store(true);
                found = true;
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_dlQueueMutex);
        const std::string utf8 = ToUtf8(name);
        for (auto it = g_dlQueue.begin(); it != g_dlQueue.end();) {
            if (it->filename == utf8) { it = g_dlQueue.erase(it); found = true; }
            else ++it;
        }
    }
    g_consoleStatus = found
        ? L"Отмена загрузки: " + name + L" (.part сохранён для докачки)"
        : L"Активной загрузки " + name + L" нет.";
}

void StartDownload(const std::wstring& rawUrl, std::string filename) {
    const std::wstring url = NormalizeUrl(rawUrl);
    const bool ui = g_consoleUiMode;
    auto report = [&](const std::wstring& msg) {
        if (ui) g_consoleStatus = msg; else PrintLine(ToUtf8(msg));
    };

    // Only http(s) can be downloaded.
    const std::wstring lower = LowerWide(url);
    if (lower.rfind(L"http://", 0) != 0 && lower.rfind(L"https://", 0) != 0) {
        report(L"Скачивание поддерживает только http/https ссылки.");
        return;
    }

    if (filename.empty())
        filename = ToUtf8(DefaultDownloadName(url));

    // Reject path traversal and directory components.
    if (filename.empty() ||
        filename.find("..") != std::string::npos ||
        filename.find('/') != std::string::npos ||
        filename.find('\\') != std::string::npos ||
        filename.find(':') != std::string::npos) {
        report(L"Недопустимое имя файла — без папок и спецсимволов.");
        return;
    }

    size_t queuedTotal = 0;
    {
        std::lock_guard<std::mutex> lock(g_dlQueueMutex);
        g_dlQueue.push_back({url, filename});
        queuedTotal = g_dlQueue.size();
    }

    if (!g_downloadRunning.exchange(true)) {
        if (g_downloadThread.joinable())
            g_downloadThread.join();
        g_downloadThread = std::thread(DownloadDispatcher);
        report(L"Скачивание начато: " + ToWide(filename) +
               L"  (Ctrl+J — список загрузок)");
    } else {
        report(L"В очереди загрузок (" + std::to_wstring(queuedTotal) +
               L" ждёт): " + ToWide(filename));
    }
}

void StopDownload() {
    if (!g_downloadRunning.load()) {
        if (g_consoleUiMode) g_consoleStatus = L"Нет активной загрузки.";
        else std::cout << "No active download.\n";
        return;
    }
    CancelAllDownloads();
    // `stop` means stop: drop queued jobs too, otherwise they'd start
    // downloading right after the active ones are cancelled.
    size_t queued = 0;
    {
        std::lock_guard<std::mutex> lock(g_dlQueueMutex);
        queued = g_dlQueue.size();
        g_dlQueue.clear();
    }
    const size_t active = ActiveDownloadCount();
    const std::wstring extra =
        (active + queued) > 1
            ? L" (остановлено: " + std::to_wstring(active + queued) + L")"
            : std::wstring();
    if (g_consoleUiMode)
        g_consoleStatus = L"Загрузка отменена." + extra;
    else
        std::cout << "Download cancelled." << ToUtf8(extra) << "\n";
}

size_t PendingDownloadCount() {
    std::lock_guard<std::mutex> lock(g_dlQueueMutex);
    return g_dlQueue.size();
}

// Names of jobs waiting behind the active download (for the UI status).
std::wstring DownloadQueueSummary() {
    std::lock_guard<std::mutex> lock(g_dlQueueMutex);
    if (g_dlQueue.empty())
        return {};
    std::wstring out = L"В очереди: ";
    size_t shown = 0;
    for (const auto& job : g_dlQueue) {
        if (shown++ >= 3) { out += L"…"; break; }
        if (shown > 1) out += L", ";
        out += ToWide(job.filename);
    }
    return out;
}

// Opens the downloads folder in Explorer (downloads page, `e` key).
void UiOpenDownloadsFolder() {
    const fs::path dir = DownloadsDirPath();
    std::error_code ec;
    fs::create_directories(dir, ec);
    const HINSTANCE rc = ShellExecuteW(nullptr, L"explore", dir.c_str(),
                                       nullptr, nullptr, SW_SHOWNORMAL);
    g_consoleStatus = rc > reinterpret_cast<HINSTANCE>(32)
        ? L"Открываю папку загрузок."
        : L"Не удалось открыть папку загрузок.";
}

void ShowDownloads() {
    std::cout << "\n--- DOWNLOADS ---\n";
    bool found = false;

    if (fs::exists(DownloadsDirPath())) {
        for (const auto& entry : fs::directory_iterator(DownloadsDirPath())) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() == ".part") continue;
            found = true;
            std::cout << ToUtf8(entry.path().filename().wstring())
                      << " (" << entry.file_size() << " bytes)\n";
        }
    }

    if (!found)
        std::cout << "(empty)\n";
}

static std::string FormatBytesShort(uint64_t bytes) {
    char buf[32];
    if (bytes >= 1024 * 1024)
        std::snprintf(buf, sizeof(buf), "%.1f МБ",
                      bytes / (1024.0 * 1024.0));
    else if (bytes >= 1024)
        std::snprintf(buf, sizeof(buf), "%.1f КБ", bytes / 1024.0);
    else
        std::snprintf(buf, sizeof(buf), "%llu Б",
                      static_cast<unsigned long long>(bytes));
    return buf;
}

// Builds the downloads page as a plain FetchOutcome — usable both for the
// instant Ctrl+J path and for real tab navigation (LoadJob::Internal).
FetchOutcome BuildDownloadsPage() {
    FetchOutcome out;
    out.ok = true;
    out.finalUrl = std::wstring(kInternalPrefix) + L"downloads";
    out.title = "Загрузки";

    struct DlEntry {
        std::wstring name;
        uintmax_t size = 0;
        fs::file_time_type mtime{};
        bool partial = false;
    };
    std::vector<DlEntry> items;
    std::error_code ec;
    const fs::path dir = DownloadsDirPath();
    if (fs::exists(dir)) {
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;
            DlEntry item;
            item.name = entry.path().filename().wstring();
            item.size = entry.file_size();
            item.mtime = fs::last_write_time(entry.path(), ec);
            item.partial = entry.path().extension() == L".part";
            items.push_back(std::move(item));
        }
    }
    std::sort(items.begin(), items.end(),
              [](const DlEntry& a, const DlEntry& b) { return a.mtime > b.mtime; });
    if (items.size() > 100) items.resize(100);

    const auto verified = LoadVerifiedDownloads();
    const fs::path dirPath = DownloadsDirPath();

    std::string text = "Загрузки — файлы из папки downloads/.\n"
                       "Enter — открыть, x — отменить, v — sha256.\n\n";
    for (size_t i = 0; i < items.size(); ++i) {
        const DlEntry& item = items[i];
        text += std::to_string(i + 1) + ". " + ToUtf8(item.name) +
                "  —  " + FormatBytesShort(item.size);
        if (item.partial) {
            text += "  (скачивается…)";
        } else {
            // Verified mark: recorded at download time, or a local
            // <name>.sha256.txt sidecar is present.
            const auto it = verified.find(item.name);
            if (it != verified.end() && it->second.first == item.size)
                text += "  ✓ sha256";
            else {
                std::error_code ec2;
                if (fs::exists(dirPath / (item.name + L".sha256.txt"), ec2) ||
                    fs::exists(dirPath / (item.name + L".sha256"), ec2))
                    text += "  (.sha256.txt рядом — v проверит)";
            }
            // Clickable/Enter-openable entry.
            out.links.push_back(
                {ToUtf8(item.name), L"openfile://" + item.name});
        }
        text += '\n';
    }
    if (items.empty())
        text += "Загрузок пока нет. Нажмите d на ссылке или изображении, "
                "чтобы скачать файл.";

    out.text = NormalizeDisplayText(text);
    return out;
}

// In-browser downloads page: files in downloads/, newest first.
void UiShowDownloadsPage() {
    const FetchOutcome out = BuildDownloadsPage();
    g_currentUrl = out.finalUrl;
    g_currentTitle = out.title;
    g_currentText = out.text;
    g_currentHtml.clear();
    g_currentLinks = out.links;
    g_currentImages.clear();
    g_currentFields.clear();
    g_currentForms.clear();
    g_consolePageOpen = true;
    g_consoleStatus = L"Enter — открыть, v — sha256, x — отменить, "
                      L"e — папка, r — обновить.";
    const std::wstring queued = DownloadQueueSummary();
    if (!queued.empty())
        g_consoleStatus += L"  |  " + queued;
}

// Opens a finished download with the shell's default handler (ShellExecute).
void UiOpenDownloadedFile(const std::wstring& name) {
    // Reject anything that could escape downloads/.
    if (name.empty() || name.find_first_of(L"/\\") != std::wstring::npos ||
        name == L"." || name == L"..") {
        g_consoleStatus = L"Некорректное имя файла.";
        return;
    }
    const fs::path path = DownloadsDirPath() / name;
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        g_consoleStatus = L"Файл не найден: " + name;
        return;
    }
    const HINSTANCE rc = ShellExecuteW(nullptr, L"open", path.c_str(),
                                       nullptr, nullptr, SW_SHOWNORMAL);
    g_consoleStatus = rc > reinterpret_cast<HINSTANCE>(32)
        ? L"Открываю: " + name
        : L"Не удалось открыть файл (код " +
          std::to_wstring(reinterpret_cast<intptr_t>(rc)) + L").";
}

// Verifies a finished download against a local <name>.sha256[.txt] sidecar.
void UiVerifyDownloadedFile(const std::wstring& name) {
    if (name.empty() || name.find_first_of(L"/\\") != std::wstring::npos) {
        g_consoleStatus = L"Некорректное имя файла.";
        return;
    }
    const fs::path path = DownloadsDirPath() / name;
    const fs::path dir = DownloadsDirPath();
    std::string expected;
    for (const wchar_t* suffix : {L".sha256.txt", L".sha256"}) {
        const fs::path sum = dir / (name + suffix);
        std::error_code ec;
        if (!fs::exists(sum, ec)) continue;
        std::ifstream in(sum, std::ios::binary);
        if (!in) continue;
        std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        if (text.size() <= 8192) {
            expected = ExtractSha256(text);
            if (!expected.empty()) break;
        }
    }
    if (expected.empty()) {
        g_consoleStatus = L"Нет файла контрольной суммы (.sha256.txt) рядом.";
        return;
    }
    std::array<BYTE, 32> digest{};
    if (!Sha256File(path, digest)) {
        g_consoleStatus = L"Не удалось прочитать файл для проверки.";
        return;
    }
    const std::string actual = Sha256Hex(digest);
    if (actual == expected) {
        std::error_code ec;
        const uintmax_t size = fs::file_size(path, ec);
        std::wofstream vf(DataDirPath() / L"verified.txt", std::ios::app);
        if (vf) vf << name << L'\t' << size << L'\t'
                   << ToWide(actual) << L'\n';
        g_consoleStatus = L"SHA-256 OK: " + name;
    } else {
        g_consoleStatus = L"SHA-256 MISMATCH: " + name +
                          L" — файл может быть повреждён!";
    }
}
