// storage.cpp — history, bookmarks, export/import, URL suggestions.
// Split out of app.cpp; shared state lives in browser.h externs.

#include "browser.h"

#include <cstdio>
#include <ctime>
#include <unordered_set>

fs::path HistoryFilePath() { return DataDirPath() / L"history.txt"; }
fs::path BookmarksFilePath() { return DataDirPath() / L"bookmarks.txt"; }

bool IsInternalPageUrl(const std::wstring& url) {
    return url.rfind(kInternalPrefix, 0) == 0;
}

namespace {

// history.txt lines may carry a leading "<unixTime>\t" prefix (Browser 4.0+);
// older plain-URL lines remain readable.
bool ParseHistoryLine(const std::string& raw, std::wstring& url,
                      uint64_t& timestamp) {
    std::string line = Trim(raw);
    if (line.empty()) return false;
    timestamp = 0;
    const size_t tab = line.find('\t');
    if (tab != std::string::npos && tab > 0) {
        bool digits = true;
        for (size_t i = 0; i < tab; ++i)
            if (!std::isdigit(static_cast<unsigned char>(line[i]))) {
                digits = false;
                break;
            }
        if (digits) {
            timestamp = std::strtoull(line.substr(0, tab).c_str(), nullptr, 10);
            line = Trim(line.substr(tab + 1));
        }
    }
    url = NormalizeUrl(ToWide(line));
    return !url.empty();
}

std::vector<std::wstring> ReadUrlFile(const fs::path& path) {
    std::vector<std::wstring> result;
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        std::wstring url;
        uint64_t ts = 0;
        if (ParseHistoryLine(line, url, ts))
            result.push_back(url);
    }
    return result;
}

// File order preserved (oldest first); timestamps may be 0 for old entries.

// "только что" / "5 мин. назад" / "3 ч. назад" / "2 дн. назад" / "12.03.2025"

struct CachedUrlFile {
    fs::file_time_type mtime{};
    bool valid = false;
    std::vector<std::wstring> urls;
};

void RefreshUrlCache(CachedUrlFile& cache, const fs::path& path) {
    std::error_code ec;
    const bool exists = fs::exists(path, ec);
    const fs::file_time_type mtime =
        exists ? fs::last_write_time(path, ec) : fs::file_time_type{};
    if (cache.valid && mtime == cache.mtime && !ec)
        return;
    cache.urls = ReadUrlFile(path);
    cache.mtime = mtime;
    cache.valid = true;
}

} // namespace

std::vector<HistoryEntry> ReadHistoryEntries(const fs::path& path) {
    std::vector<HistoryEntry> result;
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        HistoryEntry entry;
        if (ParseHistoryLine(line, entry.url, entry.timestamp))
            result.push_back(std::move(entry));
    }
    return result;
}

std::string FormatVisitTime(uint64_t timestamp) {
    if (timestamp == 0) return {};
    const uint64_t now = UnixNow();
    if (now <= timestamp) return "только что";
    const uint64_t diff = now - timestamp;
    const uint64_t minutes = diff / 60;
    const uint64_t hours = diff / 3600;
    const uint64_t days = diff / 86400;
    char buf[64];
    if (minutes < 1) return "только что";
    if (minutes < 60) {
        std::snprintf(buf, sizeof(buf), "%llu мин. назад",
                      static_cast<unsigned long long>(minutes));
        return buf;
    }
    if (hours < 24) {
        std::snprintf(buf, sizeof(buf), "%llu ч. назад",
                      static_cast<unsigned long long>(hours));
        return buf;
    }
    if (days < 7) {
        std::snprintf(buf, sizeof(buf), "%llu дн. назад",
                      static_cast<unsigned long long>(days));
        return buf;
    }
    const std::time_t t = static_cast<std::time_t>(timestamp);
    std::tm tm{};
    localtime_s(&tm, &t);
    std::snprintf(buf, sizeof(buf), "%02d.%02d.%04d",
                  tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900);
    return buf;
}

void AppendHistory(const std::wstring& url) {
    // Skip duplicates: check the tail of the file plus an in-memory set that
    // is seeded once from the existing history.
    static std::unordered_set<std::wstring> seen;
    static bool seenInit = false;
    if (!seenInit) {
        seenInit = true;
        for (const auto& entry : ReadUrlFile(HistoryFilePath()))
            seen.insert(entry);
    }
    if (!seen.insert(url).second)
        return;

    std::error_code ec;
    const uintmax_t size = fs::exists(HistoryFilePath(), ec) ? fs::file_size(HistoryFilePath(), ec) : 0;

    // Compact the file once it grows past a reasonable size.
    if (!ec && size > 128 * 1024) {
        auto entries = ReadHistoryEntries(HistoryFilePath());
        const size_t keep = std::min<size_t>(entries.size(), MAX_HISTORY_ENTRIES);
        entries.erase(entries.begin(), entries.end() - static_cast<long>(keep));
        std::ofstream rewrite(HistoryFilePath(), std::ios::trunc);
        for (const auto& entry : entries) {
            if (entry.timestamp)
                rewrite << entry.timestamp << '\t';
            rewrite << ToUtf8(entry.url) << '\n';
        }
        return;
    }

    std::ofstream file(HistoryFilePath(), std::ios::app);
    if (file)
        file << UnixNow() << '\t' << ToUtf8(url) << '\n';
}

void LoadHistory() {
    g_navigation = ReadUrlFile(HistoryFilePath());
    // Keep only the tail of the history as the navigation stack.
    if (g_navigation.size() > MAX_HISTORY_ENTRIES) {
        g_navigation.erase(g_navigation.begin(),
                           g_navigation.end() - static_cast<long>(MAX_HISTORY_ENTRIES));
    }
    if (!g_navigation.empty()) {
        g_hasNavigation = true;
        g_navigationIndex = g_navigation.size() - 1;
    }
}

void ShowHistory() {
    auto entries = ReadHistoryEntries(HistoryFilePath());
    std::cout << "\n--- HISTORY ---\n";
    if (entries.empty()) {
        std::cout << "(empty)\n";
        return;
    }
    for (size_t i = entries.size(); i-- > 0;) {
        std::cout << ToUtf8(entries[i].url);
        const std::string when = FormatVisitTime(entries[i].timestamp);
        if (!when.empty())
            std::cout << "  (" << when << ")";
        std::cout << '\n';
    }
}

void ClearHistory() {
    std::ofstream file(HistoryFilePath(), std::ios::trunc);
    g_navigation.clear();
    g_navigationIndex = 0;
    g_hasNavigation = false;
    if (g_consoleUiMode) g_consoleStatus = L"История очищена.";
    else std::cout << "History cleared.\n";
}

// Drop history entries older than `days` days (legacy 0-timestamp lines
// are kept — their age is unknown).
void ClearHistoryOlderThan(uint64_t days) {
    const uint64_t cutoff = UnixNow() - days * 86400;
    auto entries = ReadHistoryEntries(HistoryFilePath());
    size_t removed = 0;
    {
        std::ofstream out(HistoryFilePath(), std::ios::trunc);
        for (const auto& e : entries) {
            if (e.timestamp && e.timestamp < cutoff) { ++removed; continue; }
            if (e.timestamp) out << e.timestamp << '\t';
            out << ToUtf8(e.url) << '\n';
        }
    }
    const std::string msg = "History: removed " + std::to_string(removed) +
                            " entries older than " + std::to_string(days) +
                            " days.";
    if (g_consoleUiMode)
        g_consoleStatus = L"История: удалено " + std::to_wstring(removed) +
                          L" записей старше " + std::to_wstring(days) + L" дн.";
    else
        std::cout << msg << '\n';
}

// Netscape bookmark-file export: bookmarks + recent history.
void ExportBookmarksHtml() {
    const fs::path path = SavedDirPath() / L"browser-export.html";
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        if (g_consoleUiMode) g_consoleStatus = L"Не удалось создать файл экспорта.";
        else std::cout << "Cannot create export file.\n";
        return;
    }
    out << "<!DOCTYPE NETSCAPE-Bookmark-file-1>\n"
        << "<META HTTP-EQUIV=\"Content-Type\" CONTENT=\"text/html;"
        << " charset=UTF-8\">\n<TITLE>Bookmarks</TITLE>\n<H1>Browser</H1>\n"
        << "<DL><p>\n<H3>Bookmarks</H3>\n<DL><p>\n";
    for (const auto& url : LoadBookmarks())
        out << "    <DT><A HREF=\"" << ToUtf8(url) << "\">"
            << ToUtf8(url) << "</A>\n";
    out << "</DL><p>\n<H3>History</H3>\n<DL><p>\n";
    auto entries = ReadHistoryEntries(HistoryFilePath());
    for (auto it = entries.rbegin(); it != entries.rend(); ++it)
        out << "    <DT><A HREF=\"" << ToUtf8(it->url) << "\">"
            << ToUtf8(it->url) << "</A>\n";
    out << "</DL><p>\n</DL><p>\n";
    const std::string msg = "Exported to " + ToUtf8(path.wstring());
    if (g_consoleUiMode) g_consoleStatus = ToWide(msg);
    else std::cout << msg << '\n';
}

// Import URLs from a bookmark HTML file into bookmarks (deduped).
size_t ImportBookmarksHtml(const std::wstring& path) {
    std::wifstream in(path);
    if (!in) {
        if (g_consoleUiMode) g_consoleStatus = L"Не удалось открыть файл импорта.";
        else std::cout << "Cannot open import file.\n";
        return 0;
    }
    std::wstring content((std::istreambuf_iterator<wchar_t>(in)),
                         std::istreambuf_iterator<wchar_t>());
    auto bookmarks = LoadBookmarks();
    std::unordered_set<std::wstring> have(bookmarks.begin(), bookmarks.end());
    size_t added = 0;
    // Pull every http(s) URL out of href="..." attributes and bare text.
    size_t pos = 0;
    while ((pos = content.find(L"http", pos)) != std::wstring::npos) {
        size_t end = pos;
        while (end < content.size() &&
               (std::iswalnum(content[end]) ||
                content[end] == L':' || content[end] == L'/' ||
                content[end] == L'.' || content[end] == L'-' ||
                content[end] == L'_' || content[end] == L'~' ||
                content[end] == L'%' || content[end] == L'?' ||
                content[end] == L'&' || content[end] == L'=' ||
                content[end] == L'+' || content[end] == L'#'))
            ++end;
        const std::wstring url = NormalizeUrl(content.substr(pos, end - pos));
        if (!url.empty() && have.insert(url).second) {
            bookmarks.push_back(url);
            ++added;
        }
        pos = end;
    }
    if (added > 0) {
        std::wofstream out(BookmarksFilePath(), std::ios::trunc);
        for (const auto& b : bookmarks) out << b << L'\n';
    }
    const std::string msg = "Imported " + std::to_string(added) +
                            " new bookmarks.";
    if (g_consoleUiMode)
        g_consoleStatus = L"Импортировано закладок: " +
                          std::to_wstring(added) + L".";
    else
        std::cout << msg << '\n';
    return added;
}

std::vector<std::wstring> LoadBookmarks() {
    return ReadUrlFile(BookmarksFilePath());
}

void ShowBookmarks() {
    const auto bookmarks = LoadBookmarks();
    std::cout << "\n--- BOOKMARKS ---\n";
    if (bookmarks.empty()) {
        std::cout << "(empty)\n";
        return;
    }
    for (size_t i = 0; i < bookmarks.size(); ++i)
        std::cout << '[' << i + 1 << "] " << ToUtf8(bookmarks[i]) << '\n';
}

void ClearBookmarks() {
    std::ofstream file(BookmarksFilePath(), std::ios::trunc);
    if (g_consoleUiMode) g_consoleStatus = L"Закладки очищены.";
    else std::cout << "Bookmarks cleared.\n";
}

void AddBookmark() {
    if (g_currentUrl.empty() || IsInternalPageUrl(g_currentUrl)) {
        if (g_consoleUiMode) {
            g_consoleStatus = L"Сначала откройте страницу, которую хотите сохранить.";
        } else {
            std::cout << "Open a page first.\n";
        }
        return;
    }

    auto bookmarks = LoadBookmarks();
    if (std::find(bookmarks.begin(), bookmarks.end(), g_currentUrl) != bookmarks.end()) {
        if (g_consoleUiMode) g_consoleStatus = L"Эта страница уже в закладках.";
        else std::cout << "This page is already bookmarked.\n";
        return;
    }

    std::ofstream file(BookmarksFilePath(), std::ios::app);
    if (!file) {
        if (g_consoleUiMode) g_consoleStatus = L"Не удалось сохранить закладку.";
        else std::cout << "Cannot write bookmarks file.\n";
        return;
    }

    file << ToUtf8(g_currentUrl) << '\n';
    if (g_consoleUiMode) g_consoleStatus = L"Страница добавлена в закладки.";
    else std::cout << "Bookmark saved.\n";
}

std::vector<std::wstring> GetBookmarksCached() {
    static CachedUrlFile cache;
    RefreshUrlCache(cache, BookmarksFilePath());
    return cache.urls;
}

std::vector<std::wstring> GetUrlSuggestions(const std::wstring& prefix,
                                            size_t maxCount) {
    static CachedUrlFile historyCache;
    static CachedUrlFile bookmarkCache;
    RefreshUrlCache(historyCache, HistoryFilePath());
    RefreshUrlCache(bookmarkCache, BookmarksFilePath());

    const std::wstring needle = LowerWide(prefix);
    if (needle.size() < 2)
        return {};

    std::vector<std::wstring> result;
    std::unordered_set<std::wstring> seen;
    auto tryAdd = [&](const std::wstring& url) {
        if (result.size() >= maxCount) return;
        if (seen.insert(url).second &&
            LowerWide(url).find(needle) != std::wstring::npos)
            result.push_back(url);
    };

    // Bookmarks first (explicit user choice), then history newest-first.
    for (const auto& url : bookmarkCache.urls)
        tryAdd(url);
    for (auto it = historyCache.urls.rbegin();
         it != historyCache.urls.rend(); ++it)
        tryAdd(*it);
    return result;
}
