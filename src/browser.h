#pragma once

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <winhttp.h>
#include <shlwapi.h>

#include <lexbor/html/html.h>
#include <lexbor/html/encoding.h>
#include <lexbor/encoding/encoding.h>
#include <lexbor/dom/collection.h>
#include <lexbor/dom/interfaces/element.h>
#include <lexbor/dom/interfaces/node.h>
#include <lexbor/dom/interfaces/character_data.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr size_t MAX_PREFETCH_THREADS = 8;
static constexpr size_t DOWNLOAD_BUFFER_SIZE = 64 * 1024;
static constexpr size_t MAX_DISPLAY_CHARS = 50000;
static constexpr size_t MAX_REDIRECTS = 8;
static constexpr size_t MAX_PAGE_LINKS = 150;
static constexpr size_t MAX_PAGE_FIELDS = 120;
static constexpr size_t MAX_PAGE_FORMS = 20;
static constexpr size_t MAX_DOM_DEPTH = 512;
static constexpr size_t MAX_HISTORY_ENTRIES = 500;
static constexpr uint64_t MAX_DOWNLOAD_BYTES = 2ULL * 1024 * 1024 * 1024; // 2 GiB
static const wchar_t* USER_AGENT = L"Browser/1.0.0 (Windows; Console; C++20)";

enum class CacheMode {
    Use,
    Revalidate,
    Bypass
};

// ---------------------------------------------------------------------------
// Data structures
// ---------------------------------------------------------------------------

struct Response {
    DWORD status = 0;
    std::vector<char> body;
    std::wstring contentType;
    uint64_t contentLength = 0; // from Content-Length header, 0 = unknown
    bool ok = false;
    bool fromCache = false;
};

struct CacheMeta {
    uint64_t savedAt = 0;
    uint64_t maxAge = 60;
    std::wstring etag;
    std::wstring lastModified;
    std::wstring contentType;
};

struct Link {
    std::string label;   // UTF-8
    std::wstring url;    // absolute, normalized
};

struct PageImage {
    std::wstring url;
    std::string alt;     // UTF-8
};

enum class FieldKind {
    Text,       // text/search/url/email/tel/number/date/... inputs
    Password,   // rendered masked
    Hidden,     // not shown, submitted silently
    Checkbox,
    Radio,
    Submit,     // submit button / input[type=submit]
    Textarea,
    Select      // rendered with current option; Enter cycles options
};

struct FormField {
    FieldKind kind = FieldKind::Text;
    std::string name;                 // UTF-8 ("" allowed for submit)
    std::string value;                // current/default value, UTF-8
    std::string label;                // UTF-8 ("" → fallback to name)
    bool checked = false;
    std::vector<std::string> options;      // Select only, display text, UTF-8
    std::vector<std::string> optionValues; // Select only, submitted value
    size_t selected = 0;
    int formIndex = -1;               // index into ParsedPage::forms
};

struct FormDef {
    std::wstring action;              // absolute URL; empty = page URL
    std::string method = "get";       // "get" | "post"
    std::vector<int> fields;          // indices into ParsedPage::fields
};

// One dated history line (timestamp 0 = legacy entry without a date).
struct HistoryEntry {
    std::wstring url;
    uint64_t timestamp = 0;
};

// URL scheme of built-in pages (downloads, and reserved for others).
static constexpr const wchar_t* kInternalPrefix = L"browser://";

struct ParsedPage {
    std::string title;   // UTF-8
    std::string text;    // UTF-8
    std::vector<Link> links;
    std::vector<PageImage> images;
    std::vector<FormField> fields;
    std::vector<FormDef> forms;
    int refreshSeconds = 0;   // <meta http-equiv="refresh">
    std::string refreshUrl;   // UTF-8, may be empty (reload same page)
};

// Result of a completed network fetch + parse (no UI state touched).
struct FetchOutcome {
    bool ok = false;
    bool cancelled = false;
    DWORD status = 0;
    bool fromCache = false;
    size_t bodyBytes = 0;
    std::wstring finalUrl;
    std::string title;
    std::string text;
    std::vector<Link> links;
    std::vector<PageImage> images;
    std::vector<FormField> fields;
    std::vector<FormDef> forms;
    std::string html;        // decoded UTF-8 source, capped (for Ctrl+S)
    int refreshSeconds = 0;
    std::string refreshUrl;
    bool isFeed = false;    // RSS/Atom/RDF rendered by feeds.cpp
    std::wstring error;  // human-readable, UTF-16
};

// Meta refresh state of the current page (0 = none).
extern int g_refreshSeconds;
extern std::string g_refreshUrl;
extern bool g_isFeed;   // current page is an RSS/Atom feed view
extern bool g_userOffline; // user-enabled offline mode (cache only)

// A navigation request executed on a worker thread.
struct LoadJob {
    enum class Kind { Url, Search, Internal, EdgeDom, Ai } kind = Kind::Url;
    std::wstring url;          // Kind::Url
    std::string query;         // Kind::Search (UTF-8)
    CacheMode mode = CacheMode::Use;
    bool addToNavigation = true;
    bool recordVisit = true;
    int navIndexTarget = -1;   // >= 0: set g_navigationIndex on success
    size_t tabIndex = 0;       // tab the job was started for
    std::string postBody;      // non-empty → POST with this urlencoded body
    bool aiContext = false;    // Kind::Ai: include current page text
};

// ---------------------------------------------------------------------------
// Global state (defined in app.cpp)
// ---------------------------------------------------------------------------

extern HINTERNET g_session;
extern std::mutex g_outputMutex;
extern std::mutex g_cacheMutex;
extern std::atomic_bool g_downloadRunning;
extern std::atomic_bool g_downloadsPaused;
extern std::thread g_downloadThread;

extern std::atomic_uint64_t g_networkRequests;
extern std::atomic_uint64_t g_cacheHits;
extern std::atomic_uint64_t g_downloadedBytes;

extern std::wstring g_currentUrl;
extern std::string g_currentTitle;
extern std::string g_currentText;
extern bool g_consoleUiMode;
extern bool g_consolePageOpen;
extern bool g_offline;             // last fetch failed at network level
extern std::wstring g_consoleStatus;
extern std::vector<Link> g_currentLinks;
extern std::vector<PageImage> g_currentImages;
extern std::vector<FormField> g_currentFields;
extern std::vector<FormDef> g_currentForms;
extern std::string g_currentHtml;      // raw page source for "save page"
extern std::atomic_uint64_t g_loadDownloaded;
extern std::atomic_uint64_t g_loadTotal;   // Content-Length, 0 = unknown // bytes received by active load
extern std::vector<std::wstring> g_navigation;
extern size_t g_navigationIndex;
extern bool g_hasNavigation;

extern bool g_useAnsi;

// Async page loading (console UI).
extern std::atomic_bool g_loadRunning;
extern std::atomic_bool g_loadCancel;
extern std::mutex g_loadMutex;
extern Clock::time_point g_loadStart; // when the active load began (speed/ETA)

// ---------------------------------------------------------------------------
// util.cpp — encoding, hashing, small helpers, print helpers
// ---------------------------------------------------------------------------

std::wstring ToWide(const std::string& value);          // UTF-8 -> UTF-16
std::string ToUtf8(const std::wstring& value);          // UTF-16 -> UTF-8
std::string HashUrl(const std::wstring& url);
// User-data directories, resolved next to the executable (auto-created).
fs::path DataDirPath();
fs::path DownloadsDirPath();
fs::path CacheDirPath();
fs::path SavedDirPath();
fs::path TabsFilePath();
uint64_t UnixNow();
void PrintLine(const std::string& text);

std::wstring LowerWide(std::wstring value);
std::string LowerAscii(std::string value);
std::string Trim(const std::string& value);
std::wstring TrimWide(std::wstring value);
std::string UrlEncode(const std::string& text);
std::string PercentDecode(const std::string& text);
bool Sha256File(const fs::path& path, std::array<BYTE, 32>& digest);
std::string Sha256Hex(const std::array<BYTE, 32>& digest);
std::wstring NormalizeUrl(std::wstring url);
bool IsUrlLike(const std::wstring& value);

// Print helpers (ANSI aware).
namespace ui {
    extern const char* reset;
    extern const char* cyan;
    extern const char* blue;
    extern const char* green;
    extern const char* yellow;
    extern const char* red;
    extern const char* muted;
    extern const char* white;
    extern const char* bold;
    extern const char* magenta;

    std::string color(const char* code, const std::string& value);
    std::string color(const char* first, const char* second,
                      const std::string& value);
    bool Enable();
    void Banner();
    void Prompt();
    void Section(const std::string& title);
    void Notice(const std::string& label, const std::string& value,
                bool success = true);
    void PageHeader(const std::string& title, const std::wstring& url,
                    DWORD status, size_t bytes, bool fromCache);
    void Footer();
    void Good(const std::string& text);
    void Warn(const std::string& text);
    void Error(const std::string& text);
}

// ---------------------------------------------------------------------------
// network.cpp — WinHTTP session requests, redirects, cache
// ---------------------------------------------------------------------------

std::string CacheBodyFile(const std::wstring& url);
std::string CacheMetaFile(const std::wstring& url);
bool LoadCache(const std::wstring& url, Response& response, CacheMeta& meta);
void SaveCache(const std::wstring& url, const Response& response,
               const CacheMeta& meta);
void RemoveCacheEntry(const std::wstring& url);
uint64_t ReadMaxAge(const std::wstring& cacheControl);
bool CacheIsFresh(const CacheMeta& meta);

// Performs a GET with redirect following. `cancel` (optional) aborts reads.
// `resolvedUrl` (optional) receives the final URL after redirects.
// `postBody` non-null switches to POST (never cached).
// `progressBytes` (optional) accumulates received body bytes.
Response Request(const std::wstring& url, CacheMode mode = CacheMode::Use,
                 std::atomic_bool* cancel = nullptr,
                 std::wstring* resolvedUrl = nullptr,
                 const std::string* postBody = nullptr,
                 std::atomic_uint64_t* progressBytes = nullptr);

// Low-level request helpers shared with the download worker.
struct RequestHandles {
    HINTERNET connection = nullptr;
    HINTERNET request = nullptr;
    std::wstring poolKey;          // keep-alive pool key (empty = no pooling)
    bool pooledConnection = false; // connection came from / returns to pool
    bool borrowedConnection = false; // connection was taken from the pool
    bool discardConnection = false; // drop connection instead of pooling
};
bool CreateRequest(const std::wstring& url, RequestHandles& handles,
                   const wchar_t* method = L"GET",
                   HINTERNET session = nullptr);
void CloseRequest(RequestHandles& handles);
void CloseConnectionPool();
std::wstring QueryHeader(HINTERNET request, DWORD query);
std::wstring QueryHeaderValue(HINTERNET request, const wchar_t* name);
std::wstring ResolveRedirectUrl(const std::wstring& base,
                                const std::wstring& location);

// ---------------------------------------------------------------------------
// html.cpp — charset detection + Lexbor HTML5 parsing
// ---------------------------------------------------------------------------

std::string ConvertHtmlToUtf8(const std::string& html,
                              const std::wstring& contentType);
std::string DetectHtmlCharset(const std::string& html,
                              const std::wstring& contentType,
                              size_t& bomLength);
std::string NormalizeDisplayText(const std::string& input);
// Reader mode: strips boilerplate short lines, keeps content lines.
std::string FilterReaderText(const std::string& input);
ParsedPage ParseHtml5(const std::string& html, const std::wstring& url);

// feeds.cpp — RSS/Atom detection and reader rendering.
bool LooksLikeFeed(const std::string& html);
struct FeedItem { std::string title, link, date, desc; };
std::vector<FeedItem> ParseFeedItems(const std::string& xml);
FetchOutcome BuildFeedPage(const std::wstring& url, const std::string& xml);

// downloads.cpp — downloads page builder (used by the load pipeline).
FetchOutcome BuildDownloadsPage();

// ---------------------------------------------------------------------------
// app.cpp — history, bookmarks, navigation, search, page loading, downloads
// ---------------------------------------------------------------------------

void AppendHistory(const std::wstring& url);
void LoadHistory();
// storage.cpp internals shared with the internal-pages builder.
fs::path HistoryFilePath();
fs::path BookmarksFilePath();
std::vector<HistoryEntry> ReadHistoryEntries(const fs::path& path);
std::string FormatVisitTime(uint64_t timestamp);
std::vector<std::wstring> LoadBookmarks();
void ShowBookmarks();
void ShowHistory();
void ClearHistory();
void ClearBookmarks();
void AddBookmark();
std::vector<std::wstring> LoadInputHistory();
void SaveInputHistory(const std::vector<std::wstring>& items);

// Shared WinHTTP session lifecycle (used by both console and GUI entry points).
bool InitNetworkSession();
void ShutdownNetworkSession();

FetchOutcome FetchPage(const std::wstring& url, CacheMode mode,
                       std::atomic_bool* cancel = nullptr,
                       const std::string* postBody = nullptr,
                       std::atomic_uint64_t* progressBytes = nullptr);

// Synchronous page load used by the fallback text mode.
bool LoadPage(const std::wstring& url, CacheMode cacheMode,
              bool addToNavigation, bool recordVisit);
bool PostPage(const std::wstring& url, const std::string& body);
void NavigateBack();
void NavigateForward();
void ReloadPage();

// Per-tab view state, stored inside each tab and applied by the UI.
struct TabViewState {
    size_t scrollOffset = 0;
    int selectedItem = -1;      // 0..links-1 = link, links.. = field
    int hoveredItem = -1;
    bool findMode = false;
    std::wstring findQuery;
    size_t findPos = 0;
    int editingField = -1;      // >= 0: omnibox edits this field
};

// Async navigation used by the console UI.
void UiOpenUrl(const std::wstring& url, CacheMode mode = CacheMode::Use);
void UiOpenUrlEdge(const std::wstring& url);
bool EdgePage(const std::wstring& url);
void UiSync(const std::wstring& action);
void UiSearch(const std::string& query);
void UiBack();
void UiForward();
void UiReload();
void UiBookmark();
void UiShowHistoryPage();
void UiShowHistorySearch(const std::wstring& query);
void UiSelfCheckIntegrity();
void UiShowBookmarksPage();
void UiClearHistory();
void UiClearBookmarks();
void UiClearCache();
void UiCancelLoad();

// Browser tabs (console UI). Each operation receives the current view
// state, saves it into the active tab, performs the switch and returns
// the view state of the tab that became active.
TabViewState UiNewTab(const TabViewState& view);
TabViewState UiCloseTab(const TabViewState& view);          // closes active
TabViewState UiCloseTabIndex(size_t index, const TabViewState& view);
TabViewState UiCloseOtherTabs(const TabViewState& view);
TabViewState UiCycleTab(int delta, const TabViewState& view);
TabViewState UiSwitchTabTo(size_t index, const TabViewState& view);
TabViewState UiOpenInNewTab(const std::wstring& url, const TabViewState& view);
TabViewState UiReopenClosedTab(const TabViewState& view);
TabViewState UiCloseTabsOnSide(bool before, const TabViewState& view);
TabViewState UiMoveTab(int delta, const TabViewState& view);
void UiSetTabLabel(const std::wstring& label);
void UiToggleTabPin();
bool UiTabPinned(size_t index);
bool UiCopyToClipboard(const std::wstring& text);
std::wstring UiGetClipboardText();
size_t UiTabCount();
size_t UiActiveTabIndex();
std::wstring UiTabTitle(size_t index);
std::wstring UiTabUrl(size_t index);
bool UiNavCanBack();
bool UiNavCanForward();

// Form submission from the console UI (field index into g_currentFields).
void UiSubmitForm(int fieldIndex);
// Submit the form that contains the given field (Ctrl+Enter on any field).
void UiSubmitFormForField(int fieldIndex);

// Save the current page source/text to a file (Ctrl+S). Returns the path.
std::wstring UiSavePage();
std::wstring UiSavePageMarkdown();
void UiShowDownloadsPage();
void UiOpenInternalPage(const std::wstring& kind);
std::string HelpPageText(); // browser://help page body
std::string StatsPageText(); // browser://stats page body
std::wstring UiGetSetting(const std::wstring& key);
void UiSetSetting(const std::wstring& key, const std::wstring& value);
std::wstring UiHomepage();
void UiShellOpen(const std::wstring& url);
void UiAddNote(const std::wstring& text);
void UiOpenDownloadedFile(const std::wstring& name);
void UiVerifyDownloadedFile(const std::wstring& name);
void UiOpenExternalBrowser();
void UiOpenDownloadsFolder();
void ClearHistoryOlderThan(uint64_t days);
void ExportBookmarksHtml();
size_t ImportBookmarksHtml(const std::wstring& path);
std::wstring DownloadQueueSummary();

// App version + GitHub release check (runs once a day in background).
inline constexpr const char* kAppVersion = "1.0.0";
void UiScheduleUpdateCheck();
std::wstring UiTakeUpdateNotice();
size_t PendingDownloadCount();
uint64_t TotalDownloadProgress();
size_t ActiveDownloadCount();
void CancelAllDownloads();
void UiCancelDownloadByName(const std::wstring& name);

// ai.cpp — built-in AI via Pollinations.ai (free, no API key).
FetchOutcome AiAsk(const std::string& promptUtf8, bool withPage);
void UiAi(const std::wstring& args);

// console_ui helpers — in-memory pages for source/info + status commands.
void UiViewSourcePage();
void UiPageInfoPage();
void UiShowStats();
void UiShowTabs();
void UiReaderMode();
TabViewState UiOpenInBackgroundTab(const std::wstring& url,
                                   const TabViewState& view);

// Feature pack helpers (app.cpp).
bool UiOfflineEnabled();
void UiSetOffline(bool on);
void UiApplyOfflineSetting();
TabViewState UiSearchInBackgroundTab(const std::string& query,
                                     const TabViewState& view);
void UiHardReload();
void UiViewSourceUrl(const std::wstring& rawUrl);
void UiImagesPage();
void UiLinksPage();
void UiNoteDelete(size_t index1);
void UiNotesClear();
void UiLucky();
void UiSessionsPage();
void UiRestoreSession(const std::wstring& name);
bool UiImagesEnabled();
void UiSetImagesEnabled(bool on);
void UiApplyImagesSetting();
bool UiStatusBarOn();
void UiSetStatusBar(bool on);
void UiApplyStatusBarSetting();
bool UiPrefetchEnabled();
void UiSetPrefetch(bool on);
void UiApplyPrefetchSetting();
int UiZoomPercent();
void UiSetZoomPercent(int percent);
void UiApplyZoomSetting();

// Hover prefetch: fetch a single link into the cache on mouse hover.
void UiPrefetchUrl(const std::wstring& url);
// True while the tab has a running or queued page load (tab-strip pulse).
bool UiTabBusy(size_t index);
// --safe-mode: skip session/tab/history restore and background checks.
extern bool g_safeMode;
// Auto-update: download the latest release exe and swap it in.
// Returns an empty string on success-start, or a status message.
std::wstring UiApplyUpdate();

// RSS subscriptions (feeds.cpp).
std::vector<std::wstring> LoadFeedSubscriptions();
bool FeedSubscribe(const std::wstring& url);
bool FeedUnsubscribe(size_t index1);
FetchOutcome BuildFeedsPage();

// ---------------------------------------------------------------------------
// extra.cpp — ecosystem pack: reading queue, macros, rules, wiki offline,
// feeds digest, export-md, OS integration, toasts, netlog/headers/cookies.
// ---------------------------------------------------------------------------

// Reading queue (data/queue.txt).
void UiQueueCommand(const std::wstring& args, std::wstring& statusOut);
bool UiQueueAdd(const std::wstring& url);
bool UiQueueRemove(size_t index1);
void UiQueueClear();
bool UiQueuePop(std::wstring& urlOut);
bool UiQueueDone(size_t index1);
size_t LoadReadingQueueCount();
std::wstring SafeSlug(const std::wstring& name);
uint64_t FeedDateToUnix(const std::string& d);
FetchOutcome BuildQueuePage();

// Macros (data/macros.txt): "name = cmd1 ; cmd2 ; ...".
std::vector<std::wstring> LoadMacroSteps(const std::wstring& name);
std::vector<std::wstring> ListMacroNames();

// Declarative content rules (data/rules/*.txt):
//   <url-substring> \t hide <text> | replace <from> => <to> | theme <name>
void ApplyContentRules(const std::wstring& url, std::string& text);
std::wstring SiteThemeFor(const std::wstring& url); // "" = no override
FetchOutcome BuildRulesPage();
FetchOutcome BuildGalleryPage();

// Offline Wikipedia (wiki/<slug>.txt under DataDir).
void UiWikiSave(const std::wstring& article);
FetchOutcome BuildWikiPage(const std::wstring& name); // "" = index

// RSS digest across all subscriptions (browser://digest).
FetchOutcome BuildDigestPage();

// Batch markdown export of recent history → saved/articles/ (bg thread).
void UiExportMarkdown(int days);

// Sync extras (sync.cpp provides the gist plumbing).
FetchOutcome BuildSyncPage();
std::string MergeSyncLines(const std::string& local,
                           const std::string& remote,
                           bool dedupeByUrl);

// DevTools-lite.
FetchOutcome BuildHeadersPage();
FetchOutcome BuildCookiesPage();
FetchOutcome BuildNetlogPage();
void NetLogClear();
void CookiesClear();
std::wstring NetLogExportHar(); // → saved/netlog-<date>.har, "" on failure

// Network-layer hooks (called from network.cpp).
void NetLogRecord(const std::wstring& url, const wchar_t* method,
                  DWORD status, uint64_t bytes, uint64_t ms);
void RecordResponseHeaders(const std::wstring& url,
                           const std::wstring& rawHeaders);
std::wstring CookieHeaderFor(const std::wstring& url);
std::vector<std::wstring> HeadersForUrl(const std::wstring& url);
void CookiesClearAll();

// OS integration.
std::wstring UiOsCommand(const std::wstring& args); // register-protocol etc.
void UiDownloadToast(const std::wstring& fileName);

// Extra internal pages dispatcher — returns ok=false for unknown kinds.
FetchOutcome BuildExtraInternalPage(const std::wstring& url);

// URL handed off via argv (browser:// protocol, .html file association).
// Consumed once by the console UI after session restore.
extern std::wstring g_pendingLaunchUrl;

// Startup maintenance: daily data backup + cache size eviction.
void MaybeDailyBackup();
void EvictCacheOverflow();
// Exports tabs+history+bookmarks into saved/session-<ts>.txt.
std::wstring UiExportSession();

// Address-bar suggestions from history + bookmarks (cached by file mtime).
std::vector<std::wstring> GetUrlSuggestions(const std::wstring& prefix,
                                            size_t maxCount);
std::vector<std::wstring> GetBookmarksCached();

// Persist open tab URLs and restore them on startup (console UI).
void SaveTabs();
void RestoreTabs();

// Called from the UI loop; commits finished loads, starts queued ones.
// Returns true when visible state changed and a redraw is needed.
bool PollAsyncLoad();

// Joins the async load thread (call before process exit).
void ShutdownAsyncLoads();
void SearchWeb(const std::string& query);

// Synthetic in-memory pages (history / bookmarks).
bool IsInternalPageUrl(const std::wstring& url);

void PrefetchLinks();
void StartDownload(const std::wstring& url, std::string filename);
void StopDownload();
void UiToggleDownloadPause();
bool DownloadsPaused();
void ShowDownloads();
void ClearCache();
void ShowStats();
void ShowHelp();
void FindText(const std::string& query);
bool RenderImageInConsole(const PageImage& image);
bool DecodeImagePixels(const std::wstring& url,
                       uint32_t maxPixelWidth, uint32_t maxPixelHeight,
                       std::vector<std::array<uint8_t, 3>>& rgb,
                       uint32_t& outWidth, uint32_t& outHeight);

// Exposed for tests.
bool IsChallengePage(const FetchOutcome& outcome);
std::wstring UnwrapSearchUrl(const std::wstring& url);

namespace console_ui {
    bool Run();
}
