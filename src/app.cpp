#include "browser.h"

#include <array>
#include <cstdio>
#include <ctime>
#include <cwchar>
#include <deque>
#include <map>
#include <algorithm>
#include <psapi.h>
#include <random>
#include <regex>
#include <shellapi.h>
#include <unordered_map>
#include <unordered_set>

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------

HINTERNET g_session = nullptr;

// Shared WinHTTP session setup: long-lived handle for keep-alive reuse,
// gzip/deflate decompression and pooled background connections.
bool InitNetworkSession() {
    g_session = WinHttpOpen(
        USER_AGENT,
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!g_session)
        return false;

    WinHttpSetTimeouts(g_session, 5000, 5000, 15000, 30000);

    DWORD decompression =
        WINHTTP_DECOMPRESSION_FLAG_GZIP |
        WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
    WinHttpSetOption(g_session, WINHTTP_OPTION_DECOMPRESSION,
                     &decompression, sizeof(decompression));

#ifdef WINHTTP_OPTION_BACKGROUND_CONNECTIONS
    DWORD backgroundConnections = 4;
    WinHttpSetOption(g_session, WINHTTP_OPTION_BACKGROUND_CONNECTIONS,
                     &backgroundConnections, sizeof(backgroundConnections));
#endif
    return true;
}

void ShutdownNetworkSession() {
    if (g_session) {
        CloseConnectionPool();
        WinHttpCloseHandle(g_session);
        g_session = nullptr;
    }
}
std::mutex g_outputMutex;
std::mutex g_cacheMutex;
std::atomic_bool g_downloadRunning{false};
std::atomic_bool g_downloadsPaused{false};
std::thread g_downloadThread;


std::atomic_uint64_t g_networkRequests{0};
std::atomic_uint64_t g_cacheHits{0};
std::atomic_uint64_t g_downloadedBytes{0};

std::wstring g_currentUrl;
std::string g_currentTitle;
std::string g_currentText;
bool g_consoleUiMode = false;
bool g_consolePageOpen = false;
int g_refreshSeconds = 0;
std::string g_refreshUrl;
bool g_isFeed = false;
FetchOutcome g_lastAiOutcome;
std::wstring g_consoleStatus;
bool g_offline = false;
bool g_userOffline = false;
bool g_showImages = true;
bool g_statusBar = true;
bool g_prefetch = true;
int g_zoomPercent = 100;
std::vector<Link> g_currentLinks;
std::vector<PageImage> g_currentImages;
std::vector<FormField> g_currentFields;
std::vector<FormDef> g_currentForms;
std::string g_currentHtml;
std::atomic_uint64_t g_loadDownloaded{0};
std::atomic_uint64_t g_loadTotal{0};
std::vector<std::wstring> g_navigation;
size_t g_navigationIndex = 0;
bool g_hasNavigation = false;

bool g_useAnsi = false;

std::atomic_bool g_loadRunning{false};
Clock::time_point g_loadStart{};
std::atomic_bool g_loadCancel{false};
std::mutex g_loadMutex;
bool g_safeMode = false;          // --safe-mode: no session restore
std::wstring g_pendingLaunchUrl;  // argv URL (protocol/file association)
std::atomic_long g_loadingTabIdx{-1}; // tab with the running load (-1 none)

// Internal pages defined later in this file.
std::string AboutPageText();
std::string NotesPageText();
std::string CachePageText();
std::string DefinePageText(const std::string& wordEnc);
std::string TrPageText(const std::string& textEnc);
std::string SettingsPageText();

// ---------------------------------------------------------------------------
// Page loading (network + parse)
// ---------------------------------------------------------------------------

static bool LooksLikeHtml(const std::string& body) {
    size_t i = 0;
    while (i < body.size() &&
           (body[i] == ' ' || body[i] == '\t' || body[i] == '\r' || body[i] == '\n' ||
            (static_cast<unsigned char>(body[i]) >= 0x80))) // BOM / high bytes
        ++i;
    if (i >= body.size()) return false;
    return body[i] == '<';
}

static bool LooksBinary(const std::string& body) {
    const size_t probe = std::min<size_t>(body.size(), 8192);
    size_t zeros = 0;
    for (size_t i = 0; i < probe; ++i)
        if (body[i] == '\0') ++zeros;
    return zeros > 0;
}

namespace {
std::wstring FindEdgePath();
FetchOutcome EdgeDomFetch(const std::wstring& url);
}

// Heuristic: page fetched fine but has almost no readable text while its
// HTML is script-heavy — classic JS-app shell (SPA, YouTube-like).
static bool LooksLikeJsHeavy(const FetchOutcome& outcome) {
    if (!outcome.ok || outcome.html.empty()) return false;
    if (outcome.text.size() >= 800) return false;
    const std::string low = LowerAscii(outcome.html);
    size_t scripts = 0, pos = 0;
    while ((pos = low.find("<script", pos)) != std::string::npos) {
        ++scripts; pos += 7;
    }
    return scripts >= 3 ||
           low.find("enable javascript") != std::string::npos ||
           low.find("noscript") != std::string::npos;
}

FetchOutcome FetchPage(const std::wstring& url, CacheMode mode,
                       std::atomic_bool* cancel,
                       const std::string* postBody,
                       std::atomic_uint64_t* progressBytes) {
    FetchOutcome outcome;
    outcome.finalUrl = url;

    std::wstring resolvedUrl;
    Response response = Request(url, mode, cancel, &resolvedUrl, postBody,
                                progressBytes);
    if (!resolvedUrl.empty())
        outcome.finalUrl = resolvedUrl;
    if (cancel && cancel->load()) {
        outcome.cancelled = true;
        outcome.error = L"Загрузка отменена.";
        return outcome;
    }
    if (!response.ok) {
        outcome.error = L"Не удалось открыть страницу. HTTP " +
            std::to_wstring(response.status);
        return outcome;
    }

    outcome.status = response.status;
    outcome.fromCache = response.fromCache;
    outcome.bodyBytes = response.body.size();

    const std::string raw(static_cast<const char*>(response.body.data()),
                          response.body.size());
    const std::string decoded = ConvertHtmlToUtf8(raw, response.contentType);
    // Keep the source for "save page" (Ctrl+S); cap to bound memory.
    if (!LooksBinary(raw) && decoded.size() <= 4ULL * 1024 * 1024)
        outcome.html = decoded;
    const std::string contentType = LowerAscii(ToUtf8(response.contentType));

    const bool htmlish = contentType.find("html") != std::string::npos ||
                         contentType.find("xhtml") != std::string::npos ||
                         contentType.find("xml") != std::string::npos ||
                         contentType.empty();

    if (htmlish || LooksLikeHtml(decoded)) {
        // RSS/Atom/RDF feeds render as a readable item list — do it here
        // so both the async LoadJob path and the synchronous fallback
        // LoadPage path get the feed view.
        if (LooksLikeFeed(decoded))
            return BuildFeedPage(url, decoded);
        ParsedPage page = ParseHtml5(decoded, url);
        outcome.title = std::move(page.title);
        outcome.text = std::move(page.text);
        // Declarative site rules (data/rules/*.txt): hide/replace lines.
        ApplyContentRules(url, outcome.text);
        outcome.links = std::move(page.links);
        outcome.images = std::move(page.images);
        outcome.fields = std::move(page.fields);
        outcome.forms = std::move(page.forms);
        outcome.refreshSeconds = page.refreshSeconds;
        outcome.refreshUrl = page.refreshUrl;
        outcome.ok = true;
        return outcome;
    }

    // Text-ish non-HTML content (plain text, JSON, CSS, JS): show as text.
    const bool textual = contentType.rfind("text/", 0) == 0 ||
                         contentType.find("json") != std::string::npos ||
                         contentType.find("javascript") != std::string::npos ||
                         contentType.find("xml") != std::string::npos;
    if (textual && !LooksBinary(decoded)) {
        outcome.text = NormalizeDisplayText(decoded);
        if (outcome.text.size() > MAX_DISPLAY_CHARS)
            outcome.text.resize(MAX_DISPLAY_CHARS);
        outcome.title = "Text document";
        outcome.ok = true;
        return outcome;
    }

    // Binary or unknown content: show a notice instead of garbage.
    outcome.title = "Unsupported content";
    outcome.text = "Этот ресурс нельзя отобразить в консоли "
                   "(тип: " + (contentType.empty() ? std::string("неизвестно")
                                                   : contentType) +
                   ", " + std::to_string(response.body.size()) + " байт). "
                   "Используйте команду download для сохранения файла.";
    outcome.ok = true;
    return outcome;
}

static void RenderPageImages(const std::vector<PageImage>& images);
static void PrintLoadedPage(const FetchOutcome& outcome);

// Commit a FetchOutcome into the global page state.
static void CommitOutcome(const FetchOutcome& outcome) {
    g_currentUrl = outcome.finalUrl;
    g_currentTitle = outcome.title;
    g_currentText = outcome.text;
    g_currentHtml = outcome.html;
    g_currentLinks = outcome.links;
    g_currentImages = outcome.images;
    g_currentFields = outcome.fields;
    g_currentForms = outcome.forms;
    g_refreshSeconds = outcome.refreshSeconds;
    g_refreshUrl = outcome.refreshUrl;
    g_isFeed = outcome.isFeed;
    g_consolePageOpen = true;
}

static void CommitNavigation(bool addToNavigation, bool recordVisit,
                             const std::wstring& url) {
    if (addToNavigation) {
        if (g_hasNavigation && g_navigationIndex + 1 < g_navigation.size())
            g_navigation.erase(g_navigation.begin() +
                               static_cast<long>(g_navigationIndex) + 1,
                               g_navigation.end());
        if (g_navigation.empty() || g_navigation.back() != url)
            g_navigation.push_back(url);
        g_hasNavigation = true;
        g_navigationIndex = g_navigation.size() - 1;
    }
    if (recordVisit)
        AppendHistory(url);
}

// ---------------------------------------------------------------------------
// Browser tabs (console UI)
// ---------------------------------------------------------------------------

struct TabState {
    std::wstring url;
    std::string title;
    std::string text;
    std::string html;           // raw source for Ctrl+S
    std::vector<Link> links;
    std::vector<PageImage> images;
    std::vector<FormField> fields;
    std::vector<FormDef> forms;
    bool pageOpen = false;      // false = "new tab" search page
    bool pendingLoad = false;   // restored tab: load URL on activation
    TabViewState view;
    // Per-tab back/forward stack (console UI only).
    std::vector<std::wstring> nav;
    size_t navIndex = 0;
    std::wstring label;         // custom tab name (tabname command)
    bool pinned = false;        // pinned tabs resist accidental closing
};

static std::vector<TabState> g_tabs;
static std::vector<TabState> g_closedTabs; // Ctrl+Shift+T stack (max 8)
static size_t g_activeTab = 0;
static constexpr size_t MAX_TABS = 12;
static constexpr size_t MAX_CLOSED_TABS = 8;

static void EnsureTabs() {
    if (g_tabs.empty())
        g_tabs.emplace_back();
    if (g_activeTab >= g_tabs.size())
        g_activeTab = g_tabs.size() - 1;
}

static TabState& ActiveTab() {
    EnsureTabs();
    return g_tabs[g_activeTab];
}

// Copy the live globals (the active tab's working copy) into its slot.
static void SnapshotActiveTab() {
    EnsureTabs();
    TabState& tab = g_tabs[g_activeTab];
    tab.url = g_currentUrl;
    tab.title = g_currentTitle;
    tab.text = g_currentText;
    tab.html = g_currentHtml;
    tab.links = g_currentLinks;
    tab.images = g_currentImages;
    tab.fields = g_currentFields;
    tab.forms = g_currentForms;
    tab.pageOpen = g_consolePageOpen;
}

static void LoadTabToGlobals(const TabState& tab) {
    g_currentUrl = tab.url;
    g_currentTitle = tab.title;
    g_currentText = tab.text;
    g_currentHtml = tab.html;
    g_currentLinks = tab.links;
    g_currentImages = tab.images;
    g_currentFields = tab.fields;
    g_currentForms = tab.forms;
    // Restore the feed flag cheaply (once per switch, not per frame).
    const std::string& h = g_currentHtml;
    g_isFeed = h.size() > 5 && h[0] == '<' &&
        (h.find("<rss") != std::string::npos ||
         h.find("<feed") != std::string::npos ||
         h.find("rdf:RDF") != std::string::npos);
    g_consolePageOpen = tab.pageOpen;
}

// Commit navigation into the ACTIVE tab's own history stack.
static void CommitTabNavigation(bool addToNavigation, bool recordVisit,
                                const std::wstring& url) {
    TabState& tab = ActiveTab();
    if (addToNavigation) {
        if (tab.navIndex + 1 < tab.nav.size())
            tab.nav.erase(tab.nav.begin() + static_cast<long>(tab.navIndex) + 1,
                          tab.nav.end());
        if (tab.nav.empty() || tab.nav.back() != url)
            tab.nav.push_back(url);
        tab.navIndex = tab.nav.size() - 1;
    }
    if (recordVisit)
        AppendHistory(url);
}

static bool TabNavCanBack() {
    const TabState& tab = ActiveTab();
    return tab.navIndex > 0;
}

static bool TabNavCanForward() {
    const TabState& tab = ActiveTab();
    return tab.navIndex + 1 < tab.nav.size();
}

// Synchronous load used by the fallback text mode.
bool LoadPage(const std::wstring& url,
              CacheMode cacheMode,
              bool addToNavigation,
              bool recordVisit) {
    if (g_consoleUiMode)
        return false; // console UI uses the async path

    FetchOutcome outcome = FetchPage(url, cacheMode, nullptr);
    if (!outcome.ok) {
        std::cout << "Request failed. "
                  << ToUtf8(outcome.error) << '\n';
        return false;
    }

    CommitOutcome(outcome);
    CommitNavigation(addToNavigation, recordVisit, outcome.finalUrl);
    PrintLoadedPage(outcome);
    return true;
}

static void PrintLoadedPage(const FetchOutcome& outcome);

// Synchronous POST used by the fallback text mode (`post` command) and
// useful for debugging form submissions without the console UI.
bool PostPage(const std::wstring& url, const std::string& body) {
    if (g_consoleUiMode)
        return false; // console UI uses the async path

    FetchOutcome outcome = FetchPage(url, CacheMode::Bypass, nullptr, &body);
    if (!outcome.ok) {
        std::cout << "Request failed. "
                  << ToUtf8(outcome.error) << '\n';
        return false;
    }

    CommitOutcome(outcome);
    CommitNavigation(true, true, outcome.finalUrl);
    PrintLoadedPage(outcome);
    return true;
}

// Synchronous headless-Edge render for the fallback text mode (`edge`
// command). Returns false in console-UI mode or when Edge is unavailable.
bool EdgePage(const std::wstring& rawUrl) {
    if (g_consoleUiMode)
        return false;
    const std::wstring url = NormalizeUrl(rawUrl);
    if (url.empty()) {
        std::cout << "Invalid URL.\n";
        return false;
    }
    FetchOutcome outcome = EdgeDomFetch(url);
    if (!outcome.ok) {
        std::cout << "Edge render failed. "
                  << ToUtf8(outcome.error) << '\n';
        return false;
    }
    CommitOutcome(outcome);
    CommitNavigation(true, true, outcome.finalUrl);
    PrintLoadedPage(outcome);
    return true;
}

// Strip internal display markers (\x01 verbatim/blank-line, \x02 inline
// image) so plain-text output (fallback mode, saved .txt) stays readable.
static std::string StripDisplayMarkers(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c == '\x01') {
            if (i + 1 < in.size() && in[i + 1] == '\n') {
                out += '\n';
                ++i;
            }
            continue;
        }
        if (c == '\x02') {
            out += "[IMG] ";
            continue;
        }
        out += c;
    }
    return out;
}

static void PrintLoadedPage(const FetchOutcome& outcome) {
    ui::PageHeader(g_currentTitle, outcome.finalUrl, outcome.status,
                   outcome.bodyBytes, outcome.fromCache);

    ui::Section("PAGE CONTENT");
    if (g_currentText.empty()) {
        ui::Warn("No readable text found. This page may require JavaScript.");
    } else {
        if (g_currentText.size() > MAX_DISPLAY_CHARS)
            g_currentText.resize(MAX_DISPLAY_CHARS);
        std::cout << "\n" << StripDisplayMarkers(g_currentText) << "\n";
    }

    RenderPageImages(g_currentImages);

    ui::Section("LINKS  ·  type open NUMBER");
    if (g_currentLinks.empty()) {
        std::cout << "  " << ui::color(ui::muted, "No links found on this page.") << "\n";
    } else {
        for (size_t i = 0; i < g_currentLinks.size(); ++i) {
            std::cout << "  " << ui::color(ui::cyan, "[" + std::to_string(i + 1) + "]")
                      << " " << ui::color(ui::white,
                             g_currentLinks[i].label.substr(0, 100))
                      << "\n     " << ui::color(ui::muted, ToUtf8(g_currentLinks[i].url))
                      << '\n';
        }
    }
    ui::Footer();
}

// Back/forward navigation. `forward` selects direction; synchronous version
// used by the fallback REPL. The console UI uses UiBack/UiForward.
static bool NavigateHistory(int direction) {
    if (!g_hasNavigation) {
        if (g_consoleUiMode) g_consoleStatus = direction < 0
            ? L"Предыдущей страницы нет." : L"Следующей страницы нет.";
        else std::cout << (direction < 0 ? "No previous page.\n"
                                         : "No next page.\n");
        return false;
    }
    if (direction < 0 && g_navigationIndex == 0) {
        if (g_consoleUiMode) g_consoleStatus = L"Предыдущей страницы нет.";
        else std::cout << "No previous page.\n";
        return false;
    }
    if (direction > 0 && g_navigationIndex + 1 >= g_navigation.size()) {
        if (g_consoleUiMode) g_consoleStatus = L"Следующей страницы нет.";
        else std::cout << "No next page.\n";
        return false;
    }

    const size_t target = g_navigationIndex + static_cast<size_t>(direction);
    const std::wstring url = g_navigation[target];
    if (!LoadPage(url, CacheMode::Use, false, false))
        return false;
    g_navigationIndex = target;
    return true;
}

void NavigateBack() { NavigateHistory(-1); }
void NavigateForward() { NavigateHistory(+1); }

void ReloadPage() {
    if (g_currentUrl.empty()) {
        if (g_consoleUiMode) g_consoleStatus = L"Нет страницы для обновления.";
        else std::cout << "No page to reload.\n";
        return;
    }
    if (IsInternalPageUrl(g_currentUrl)) {
        if (g_consoleUiMode) {
            if (g_currentUrl.find(L"history") != std::wstring::npos)
                UiShowHistoryPage();
            else
                UiShowBookmarksPage();
        }
        return;
    }
    LoadPage(g_currentUrl, CacheMode::Revalidate, false, false);
}

// ---------------------------------------------------------------------------
// Search providers
// ---------------------------------------------------------------------------

struct SearchProvider {
    const wchar_t* name;
    const wchar_t* urlTemplate; // contains {q}
};

static const SearchProvider kProviders[] = {
    {L"DuckDuckGo", L"https://html.duckduckgo.com/html/?kl=ru-ru&q={q}"},
    {L"Bing",       L"https://www.bing.com/search?setlang=ru-RU&cc=ru&q={q}"},
    {L"Google",     L"https://www.google.com/search?hl=ru&gbv=1&nojs=1&q={q}"}
};

static std::wstring BuildSearchUrl(const SearchProvider& provider,
                                   const std::wstring& encodedQuery) {
    std::wstring url = provider.urlTemplate;
    const size_t pos = url.find(L"{q}");
    if (pos != std::wstring::npos)
        url.replace(pos, 3, encodedQuery);
    return url;
}

bool IsChallengePage(const FetchOutcome& outcome) {
    const std::string urlAndTitle = LowerAscii(
        ToUtf8(outcome.finalUrl) + " " + outcome.title);
    const std::string text = LowerAscii(outcome.text);

    if (urlAndTitle.find("httpservice/retry/enablejs") != std::string::npos ||
        urlAndTitle.find("enable javascript") != std::string::npos ||
        urlAndTitle.find("javascript required") != std::string::npos)
        return true;

    if (text.find("чтобы использовать поиск, включите javascript") != std::string::npos ||
        text.find("возникли проблемы с доступом к google поиску") != std::string::npos ||
        text.find("проблемы с доступом к google") != std::string::npos ||
        text.find("if you have trouble accessing google search") != std::string::npos ||
        text.find("if you are having trouble accessing google search") != std::string::npos ||
        text.find("unusual traffic") != std::string::npos ||
        text.find("automated queries") != std::string::npos ||
        text.find("automated traffic") != std::string::npos ||
        text.find("verify that you are human") != std::string::npos ||
        text.find("unfortunately, bots use duckduckgo too") != std::string::npos ||
        text.find("complete the following challenge") != std::string::npos ||
        text.find("captcha") != std::string::npos)
        return true;

    // A "results" page with neither text nor links is effectively empty.
    return outcome.text.empty() && outcome.links.empty();
}

static int Base64Value(char ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+' || ch == '-') return 62;
    if (ch == '/' || ch == '_') return 63;
    return -1;
}

static std::string Base64UrlDecode(const std::string& in) {
    std::string out;
    out.reserve(in.size() * 3 / 4);
    int val = 0, bits = -8;
    for (char ch : in) {
        if (ch == '=' || ch == '&') break;
        const int v = Base64Value(ch);
        if (v < 0) continue;
        val = (val << 6) + v;
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<char>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

// Search engines wrap result URLs in redirect endpoints. Unwrap them so the
// user sees and opens the real destination.
std::wstring UnwrapSearchUrl(const std::wstring& url) {
    const std::string utf8 = ToUtf8(url);
    const std::string lower = LowerAscii(utf8);

    // DuckDuckGo: https://duckduckgo.com/l/?uddg=<urlencoded>&rut=...
    const size_t uddg = lower.find("uddg=");
    if (uddg != std::string::npos) {
        size_t begin = uddg + 5;
        size_t end = utf8.find('&', begin);
        if (end == std::string::npos) end = utf8.size();
        const std::wstring decoded = ToWide(PercentDecode(utf8.substr(begin, end - begin)));
        const std::wstring normalized = NormalizeUrl(decoded);
        if (!normalized.empty())
            return normalized;
    }

    // Google classic: /url?q=<urlencoded>&sa=...
    if (lower.find("/url?") != std::string::npos) {
        const size_t q = lower.find("q=", lower.find("/url?"));
        if (q != std::string::npos) {
            size_t begin = q + 2;
            size_t end = utf8.find('&', begin);
            if (end == std::string::npos) end = utf8.size();
            const std::wstring decoded = ToWide(PercentDecode(utf8.substr(begin, end - begin)));
            const std::wstring normalized = NormalizeUrl(decoded);
            if (!normalized.empty())
                return normalized;
        }
    }

    // Bing: /ck/a?...&u=a1<base64url-encoded destination>
    if (lower.find("/ck/a") != std::string::npos) {
        const size_t u = lower.find("u=a1");
        if (u != std::string::npos) {
            const size_t begin = u + 4;
            size_t end = utf8.find('&', begin);
            if (end == std::string::npos) end = utf8.size();
            const std::string decoded = Base64UrlDecode(utf8.substr(begin, end - begin));
            const std::wstring normalized = NormalizeUrl(ToWide(decoded));
            if (!normalized.empty())
                return normalized;
        }
    }

    return url;
}

static std::wstring UrlHost(const std::wstring& url) {
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || !parts.lpszHostName)
        return {};
    return std::wstring(parts.lpszHostName, parts.dwHostNameLength);
}

static bool HostEndsWith(const std::wstring& host, const wchar_t* suffix) {
    const std::wstring h = LowerWide(host);
    const std::wstring s = LowerWide(suffix);
    return h.size() >= s.size() &&
           h.compare(h.size() - s.size(), s.size(), s) == 0;
}

static bool IsSearchEngineChrome(const std::wstring& url) {
    const std::wstring host = UrlHost(url);
    if (host.empty()) return false;
    const std::string u = LowerAscii(ToUtf8(url));

    // DuckDuckGo organic results always arrive via /l/?uddg= and are unwrapped
    // before this check; anything still on a duckduckgo.com host is UI chrome.
    if (HostEndsWith(host, L"duckduckgo.com"))
        return true;

    // Google/Bing: drop links back into the SERP itself (search form,
    // preferences, feedback) but keep links to real sites — including
    // microsoft.com pages that are genuine results.
    if (HostEndsWith(host, L"google.com") || HostEndsWith(host, L"bing.com")) {
        return u.find("/search") != std::string::npos ||
               u.find("/ck/a") != std::string::npos ||
               u.find("/preferences") != std::string::npos ||
               u.find("accounts.") != std::string::npos ||
               u.find("feedback") != std::string::npos ||
               u.find("/settings") != std::string::npos;
    }
    return false;
}

static void UnwrapProviderLinks(std::vector<Link>& links) {
    std::vector<Link> result;
    result.reserve(links.size());
    for (auto& link : links) {
        const std::wstring unwrapped = UnwrapSearchUrl(link.url);
        if (unwrapped.empty())
            continue;
        link.url = unwrapped;
        if (IsSearchEngineChrome(unwrapped))
            continue;
        result.push_back(std::move(link));
    }
    links = std::move(result);
}

// Try each provider; returns the winning outcome or an error.
static FetchOutcome SearchProviders(const std::string& query,
                                    std::wstring& providerName,
                                    std::atomic_bool* cancel,
                                    std::atomic_uint64_t* progress = nullptr) {
    const std::wstring encoded = ToWide(UrlEncode(query));
    FetchOutcome last;
    last.error = L"Поисковая выдача недоступна.";

    // Preferred provider from settings (`engine` command) goes first;
    // the rest keep their built-in fallback order.
    int order[3] = {0, 1, 2};
    const std::wstring pref = UiGetSetting(L"engine");
    if (!pref.empty()) {
        for (size_t i = 0; i < 3; ++i) {
            if (LowerWide(pref) == LowerWide(kProviders[i].name) ||
                pref.find(kProviders[i].name) != std::wstring::npos) {
                std::swap(order[0], order[i]);
                break;
            }
        }
    }

    for (int oi = 0; oi < 3; ++oi) {
        const auto& provider = kProviders[order[oi]];
        if (cancel && cancel->load()) {
            last.cancelled = true;
            last.error = L"Поиск отменён.";
            return last;
        }

        const std::wstring url = BuildSearchUrl(provider, encoded);
        FetchOutcome outcome = FetchPage(url, CacheMode::Revalidate, cancel,
                                         nullptr, progress);
        if (!outcome.ok || outcome.cancelled)
            continue;
        if (IsChallengePage(outcome))
            continue;

        UnwrapProviderLinks(outcome.links);
        providerName = provider.name;
        return outcome;
    }

    last.error = L"Поиск временно недоступен: поисковые сайты не отдали "
                 L"HTML-результаты. Попробуйте позже или откройте сайт напрямую.";
    return last;
}

// Synchronous search for the fallback REPL.
void SearchWeb(const std::string& query) {
    if (query.empty()) {
        if (g_consoleUiMode) g_consoleStatus = L"Введите поисковый запрос.";
        else std::cout << "Usage: search <words>\n";
        return;
    }

    std::wstring providerName;
    FetchOutcome outcome = SearchProviders(query, providerName, nullptr);
    if (!outcome.ok) {
        if (g_consoleUiMode) g_consoleStatus = outcome.error;
        else std::cout << ToUtf8(outcome.error) << '\n';
        return;
    }

    CommitOutcome(outcome);
    CommitNavigation(true, true, outcome.finalUrl);
    if (g_consoleUiMode) {
        g_consoleStatus = L"Результаты: " + providerName;
    } else {
        std::cout << "Results via " << ToUtf8(providerName) << "\n";
        PrintLoadedPage(outcome);
    }
}

void FindText(const std::string& query) {
    if (query.empty()) {
        std::cout << "Usage: find <text>\n";
        return;
    }
    if (g_currentText.empty()) {
        std::cout << "Open a page first.\n";
        return;
    }

    // Case-insensitive search that also folds Cyrillic via towlower.
    const std::wstring haystack = LowerWide(ToWide(g_currentText));
    const std::wstring needle = LowerWide(ToWide(query));
    if (needle.empty()) {
        std::cout << "Text not found.\n";
        return;
    }

    const size_t pos = haystack.find(needle);
    if (pos == std::wstring::npos) {
        std::cout << "Text not found.\n";
        return;
    }

    const size_t start = pos > 90 ? pos - 90 : 0;
    const size_t length = std::min<size_t>(60, g_currentText.size() - start);
    std::cout << "Found at char " << pos << ":\n"
              << g_currentText.substr(start, length) << '\n';
}

// ---------------------------------------------------------------------------
// Console image rendering (fallback mode)
// ---------------------------------------------------------------------------

// Fetch + decode an image to a small RGB pixel buffer via WIC.
// `maxPixelWidth/Height` bound the output size; alpha is blended on white.
bool DecodeImagePixels(const std::wstring& url,
                       uint32_t maxPixelWidth, uint32_t maxPixelHeight,
                       std::vector<std::array<uint8_t, 3>>& rgb,
                       uint32_t& outWidth, uint32_t& outHeight) {
    using Microsoft::WRL::ComPtr;

    Response response = Request(url, CacheMode::Use, nullptr);
    if (!response.ok || response.body.empty() ||
        response.body.size() > 12 * 1024 * 1024)
        return false;

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory)
        return false;

    ComPtr<IWICStream> stream;
    hr = factory->CreateStream(&stream);
    if (FAILED(hr) || !stream)
        return false;

    hr = stream->InitializeFromMemory(
        reinterpret_cast<BYTE*>(response.body.data()),
        static_cast<DWORD>(response.body.size()));
    if (FAILED(hr))
        return false;

    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromStream(
        stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr) || !decoder)
        return false;

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr) || !frame)
        return false;

    UINT sourceWidth = 0;
    UINT sourceHeight = 0;
    if (FAILED(frame->GetSize(&sourceWidth, &sourceHeight)) ||
        sourceWidth == 0 || sourceHeight == 0)
        return false;

    const double scale = std::min(
        1.0,
        std::min(
            static_cast<double>(maxPixelWidth) / sourceWidth,
            static_cast<double>(maxPixelHeight) / sourceHeight));

    const UINT targetWidth = std::max<UINT>(
        1, static_cast<UINT>(sourceWidth * scale));
    const UINT targetHeight = std::max<UINT>(
        1, static_cast<UINT>(sourceHeight * scale));

    ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr) || !converter)
        return false;

    hr = converter->Initialize(
        frame.Get(),
        GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone,
        nullptr,
        0.0,
        WICBitmapPaletteTypeCustom);
    if (FAILED(hr))
        return false;

    ComPtr<IWICBitmapScaler> scaler;
    hr = factory->CreateBitmapScaler(&scaler);
    if (FAILED(hr) || !scaler)
        return false;

    hr = scaler->Initialize(
        converter.Get(),
        targetWidth,
        targetHeight,
        WICBitmapInterpolationModeFant);
    if (FAILED(hr))
        return false;

    const UINT stride = targetWidth * 4;
    std::vector<BYTE> pixels(
        static_cast<size_t>(stride) * targetHeight);

    hr = scaler->CopyPixels(
        nullptr,
        stride,
        static_cast<UINT>(pixels.size()),
        pixels.data());
    if (FAILED(hr))
        return false;

    rgb.resize(static_cast<size_t>(targetWidth) * targetHeight);
    for (UINT y = 0; y < targetHeight; ++y) {
        for (UINT x = 0; x < targetWidth; ++x) {
            const size_t offset = static_cast<size_t>(y) * stride + x * 4;
            const BYTE b = pixels[offset + 0];
            const BYTE g = pixels[offset + 1];
            const BYTE r = pixels[offset + 2];
            const BYTE a = pixels[offset + 3];
            auto blend = [a](BYTE channel) -> uint8_t {
                return static_cast<uint8_t>(
                    (static_cast<int>(channel) * a +
                     255 * (255 - a)) / 255);
            };
            rgb[static_cast<size_t>(y) * targetWidth + x] = {
                blend(r), blend(g), blend(b)};
        }
    }
    outWidth = targetWidth;
    outHeight = targetHeight;
    return true;
}

bool RenderImageInConsole(const PageImage& image) {
    std::vector<std::array<uint8_t, 3>> rgb;
    uint32_t w = 0, h = 0;
    if (!DecodeImagePixels(image.url, 48, 32, rgb, w, h))
        return false;

    auto pixel = [&](uint32_t x, uint32_t y) -> const std::array<uint8_t, 3>& {
        return rgb[static_cast<size_t>(y) * w + x];
    };

    const std::string caption = image.alt.empty()
        ? ToUtf8(image.url)
        : image.alt;

    std::cout << "\n  [IMAGE] " << caption.substr(0, 100)
              << " (" << w << "x" << h << ")\n";

    for (uint32_t y = 0; y < h; y += 2) {
        std::string line;
        line.reserve(static_cast<size_t>(w) * 40);

        for (uint32_t x = 0; x < w; ++x) {
            const auto& top = pixel(x, y);
            const auto& bottom = (y + 1 < h)
                ? pixel(x, y + 1)
                : std::array<uint8_t, 3>{255, 255, 255};

            char sequence[96]{};
            std::snprintf(
                sequence, sizeof(sequence),
                "\x1b[38;2;%d;%d;%dm\x1b[48;2;%d;%d;%dm▀",
                top[0], top[1], top[2],
                bottom[0], bottom[1], bottom[2]);
            line += sequence;
        }

        line += "\x1b[0m\n";
        std::cout << line;
    }

    std::cout << "\x1b[0m\n";
    return true;
}

static void RenderPageImages(const std::vector<PageImage>& images) {
    if (images.empty())
        return;

    ui::Section("IMAGES");

    if (!g_useAnsi) {
        ui::Warn("Image pixels need Windows Terminal with ANSI true color enabled.");
        for (size_t i = 0; i < std::min<size_t>(images.size(), 3); ++i) {
            std::cout << "  " << ToUtf8(images[i].url) << '\n';
        }
        return;
    }

    size_t rendered = 0;
    const size_t limit = std::min<size_t>(images.size(), 3);
    for (size_t i = 0; i < limit; ++i) {
        if (RenderImageInConsole(images[i]))
            ++rendered;
    }

    if (rendered == 0)
        ui::Warn("No supported inline images could be decoded from this page.");
    else if (images.size() > limit)
        std::cout << "  " << ui::color(ui::muted,
            "Showing " + std::to_string(limit) + " of " +
            std::to_string(images.size()) + " page images.") << '\n';
}

// ---------------------------------------------------------------------------
// Multi-threaded link prefetch
// ---------------------------------------------------------------------------

void PrefetchLinks() {
    if (g_consoleUiMode) {
        g_consoleStatus = L"Prefetch доступен только в текстовом режиме.";
        return;
    }
    if (g_currentLinks.empty()) {
        std::cout << "Open a page first.\n";
        return;
    }

    std::atomic_size_t next{0};
    std::atomic_size_t completed{0};
    const size_t workerCount =
        std::min(MAX_PREFETCH_THREADS, g_currentLinks.size());

    std::vector<std::thread> workers;
    workers.reserve(workerCount);

    std::cout << "Prefetching " << g_currentLinks.size()
              << " links with " << workerCount << " workers...\n";

    for (size_t i = 0; i < workerCount; ++i) {
        workers.emplace_back([&]() {
            for (;;) {
                const size_t index = next.fetch_add(1);
                if (index >= g_currentLinks.size())
                    break;

                Request(g_currentLinks[index].url, CacheMode::Use, nullptr);
                completed.fetch_add(1);
            }
        });
    }

    for (auto& worker : workers)
        worker.join();

    std::cout << "Prefetch complete: " << completed.load()
              << " links checked.\n";
}

void ClearCache() {
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    size_t removed = 0;

    if (fs::exists(CacheDirPath())) {
        for (const auto& entry : fs::directory_iterator(CacheDirPath())) {
            if (entry.is_regular_file()) {
                std::error_code ec;
                if (fs::remove(entry.path(), ec))
                    ++removed;
            }
        }
    }

    if (g_consoleUiMode)
        g_consoleStatus = L"Кэш очищен. Файлов: " + std::to_wstring(removed) + L".";
    else
        std::cout << "Cache cleared. Files removed: " << removed << '\n';
}

void ShowStats() {
    uint64_t cacheBytes = 0;
    size_t cacheFiles = 0;

    if (fs::exists(CacheDirPath())) {
        for (const auto& entry : fs::directory_iterator(CacheDirPath())) {
            if (entry.is_regular_file() &&
                entry.path().extension() == ".body") {
                cacheBytes += entry.file_size();
                ++cacheFiles;
            }
        }
    }

    ui::Section("BROWSER STATISTICS");
    std::cout
        << "  Network requests     " << g_networkRequests.load() << '\n'
        << "  Cache hits           " << g_cacheHits.load() << '\n'
        << "  Cached pages         " << cacheFiles << '\n'
        << "  Cache size           " << cacheBytes << " bytes\n"
        << "  Downloaded this run  " << g_downloadedBytes.load() << " bytes\n"
        << "  Prefetch workers     up to " << MAX_PREFETCH_THREADS << '\n'
        << "  Current page         "
        << (g_currentUrl.empty() ? "(none)" : ToUtf8(g_currentUrl)) << '\n';
}

void ShowHelp() {
    ui::Section("NAVIGATION");
    std::cout
        << "  " << ui::color(ui::cyan, "open <url|N>") << "   Open a site or link\n"
        << "  " << ui::color(ui::cyan, "search <words>") << "     Search the web (HTML providers)\n"
        << "  " << ui::color(ui::cyan, "back / forward") << "   Move through visited pages\n"
        << "  " << ui::color(ui::cyan, "reload") << "            Revalidate the current page\n";

    ui::Section("PAGE TOOLS");
    std::cout
        << "  " << ui::color(ui::cyan, "find <text>") << "      Find text on the current page\n"
        << "  " << ui::color(ui::cyan, "edge <url>") << "       Render via headless Edge (JS pages)\n"
        << "  " << ui::color(ui::cyan, "web [url]") << "        Open in the WebView2 window (real rendering)\n"
        << "  " << ui::color(ui::cyan, "save") << "              Save the current page to saved/\n"
        << "  " << ui::color(ui::cyan, "history") << "          Open visit history page\n"
        << "  " << ui::color(ui::cyan, "bookmarks") << "         Open bookmarks page\n"
        << "  " << ui::color(ui::cyan, "bookmark add") << "       Save the current page\n"
        << "  " << ui::color(ui::cyan, "images") << "          List page images\n"
        << "  " << ui::color(ui::cyan, "image <N>") << "        Render one image\n";

    ui::Section("STORAGE");
    std::cout
        << "  " << ui::color(ui::cyan, "clear-history") << "    Delete visit history\n"
        << "  " << ui::color(ui::cyan, "history-clear-older <дн>")
        << "  Prune entries older than N days\n"
        << "  " << ui::color(ui::cyan, "export") << "            History+bookmarks → HTML file\n"
        << "  " << ui::color(ui::cyan, "import <file>") << "     Import bookmarks/history HTML\n"
        << "  " << ui::color(ui::cyan, "bookmarks-clear") << "  Delete all bookmarks\n"
        << "  " << ui::color(ui::cyan, "cache-clear") << "       Clear the page cache\n";

    ui::Section("DOWNLOADS & STATS");
    std::cout
        << "  " << ui::color(ui::cyan, "download <url> [name]")
        << "  Stream a file to downloads/ (resumable)\n"
        << "  " << ui::color(ui::cyan, "downloads") << "           List downloaded files\n"
        << "  " << ui::color(ui::cyan, "stop") << "                Cancel ALL downloads\n"
        << "  " << ui::color(ui::cyan, "pause") << "               Pause/resume the queue\n"
        << "  " << ui::color(ui::cyan, "prefetch") << "            Fetch page links in parallel\n"
        << "  " << ui::color(ui::cyan, "stats") << "              Show network/cache statistics\n"
        << "  " << ui::color(ui::cyan, "help") << "               Show this help\n"
        << "  " << ui::color(ui::cyan, "exit") << "               Close Browser\n";

    ui::Section("SYNC (optional, needs gh CLI)");
    std::cout
        << "  " << ui::color(ui::cyan, "sync push") << "         Send data to a GitHub Gist\n"
        << "  " << ui::color(ui::cyan, "sync pull") << "         Fetch data from the Gist\n"
        << "  " << ui::color(ui::cyan, "sync") << "               Show sync status\n";
}

// ---------------------------------------------------------------------------
// Async page loading for the console UI
// ---------------------------------------------------------------------------

namespace {

struct LoadResult {
    LoadJob job;
    FetchOutcome outcome;   // valid when kind == Url or search succeeded partially
    bool ok = false;
    bool cancelled = false;
    std::wstring statusText;
    std::wstring providerName;
};

std::optional<LoadResult> g_finishedLoad;
std::deque<LoadJob> g_queuedJobs; // FIFO — rapid commands no longer drop each other
std::thread g_loadThread;

// ---------------------------------------------------------------------------
// Optional headless-Edge DOM rendering for JS-heavy pages.
// Uses "msedge --headless --dump-dom"; graceful no-op when Edge is absent.
// ---------------------------------------------------------------------------

std::wstring FindEdgePath() {
    static const wchar_t* kVars[] = {
        L"ProgramFiles(x86)", L"ProgramFiles", L"LocalAppData"};
    for (const wchar_t* var : kVars) {
        wchar_t base[MAX_PATH]{};
        if (!GetEnvironmentVariableW(var, base, MAX_PATH)) continue;
        const std::wstring p = std::wstring(base) +
            L"\\Microsoft\\Edge\\Application\\msedge.exe";
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES)
            return p;
    }
    return {};
}

FetchOutcome EdgeDomFetch(const std::wstring& url) {
    FetchOutcome out;
    const std::wstring edge = FindEdgePath();
    if (edge.empty()) {
        out.error = L"Microsoft Edge не найден — headless-рендер недоступен.";
        return out;
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        out.error = L"Не удалось создать pipe для Edge.";
        return out;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, 0, nullptr);
    if (nul == INVALID_HANDLE_VALUE) nul = GetStdHandle(STD_ERROR_HANDLE);

    const std::wstring profile = (DataDirPath() / L"edge-profile").wstring();
    std::wstring cmd = L"\"" + edge + L"\" --headless --disable-gpu "
        L"--no-first-run --no-default-browser-check "
        L"--user-data-dir=\"" + profile + L"\" "
        L"--virtual-time-budget=8000 --dump-dom \"" + url + L"\"";
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = nul;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(rd); CloseHandle(wr);
        if (nul != GetStdHandle(STD_ERROR_HANDLE)) CloseHandle(nul);
        out.error = L"Не удалось запустить Edge.";
        return out;
    }
    CloseHandle(wr);
    if (nul != GetStdHandle(STD_ERROR_HANDLE)) CloseHandle(nul);

    if (WaitForSingleObject(pi.hProcess, 30000) == WAIT_TIMEOUT)
        TerminateProcess(pi.hProcess, 1);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);

    std::string dom;
    char buf[65536];
    DWORD got = 0;
    while (dom.size() < 8ULL * 1024 * 1024 &&
           ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got != 0)
        dom.append(buf, got);
    CloseHandle(rd);
    CloseHandle(pi.hProcess);

    if (dom.empty()) {
        out.error = L"Edge не вернул DOM (JS-рендер не удался).";
        return out;
    }

    ParsedPage page = ParseHtml5(dom, url);
    out.ok = true;
    out.status = 200;
    out.bodyBytes = dom.size();
    out.finalUrl = url;
    out.title = std::move(page.title);
    out.text = std::move(page.text);
    out.links = std::move(page.links);
    out.images = std::move(page.images);
    out.fields = std::move(page.fields);
    out.forms = std::move(page.forms);
    out.refreshSeconds = page.refreshSeconds;
    out.refreshUrl = page.refreshUrl;
    if (dom.size() <= 4ULL * 1024 * 1024) out.html = std::move(dom);
    return out;
}

void LoadWorker(LoadJob job) {
    LoadResult result;
    result.job = job;

    if (job.kind == LoadJob::Kind::Internal) {
        // Synchronous in-memory page (downloads list etc.) — no network.
        if (job.url.find(L"downloads") != std::wstring::npos) {
            result.outcome = BuildDownloadsPage();
            result.ok = true;
            result.statusText =
                L"Enter — открыть, v — sha256, x — отменить, e — папка, "
                L"r — обновить.";
            const std::wstring queued = DownloadQueueSummary();
            if (!queued.empty())
                result.statusText += L"  |  " + queued;
        } else if (job.url == std::wstring(kInternalPrefix) + L"ai") {
            // AI answer page — replay the last response (prompt isn't
            // stored in nav history, so we can't regenerate it).
            if (g_lastAiOutcome.ok) {
                result.outcome = g_lastAiOutcome;
                result.ok = true;
                result.statusText = L"Ответ ИИ.";
            } else {
                result.ok = false;
                result.statusText = L"Ответ ИИ больше не доступен.";
            }
        } else if (job.url == std::wstring(kInternalPrefix) + L"about") {
            result.outcome.ok = true;
            result.outcome.finalUrl = job.url;
            result.outcome.title = "О браузере";
            result.outcome.text = AboutPageText();
            result.outcome.links = {
                {"Сайт проекта", L"https://fanxcz.github.io/browser/"},
                {"Исходники на GitHub", L"https://github.com/fanxcz/browser"},
                {"Релизы", L"https://github.com/fanxcz/browser/releases"},
                {"Issues / обратная связь",
                 L"https://github.com/fanxcz/browser/issues"},
            };
            result.ok = true;
            result.statusText = L"О браузере.";
        } else if (job.url == std::wstring(kInternalPrefix) + L"notes") {
            result.outcome.ok = true;
            result.outcome.finalUrl = job.url;
            result.outcome.title = "Заметки";
            result.outcome.text = NotesPageText();
            result.ok = true;
            result.statusText =
                L"Заметки — note <текст> добавляет строку.";
        } else if (job.url == std::wstring(kInternalPrefix) + L"cache") {
            result.outcome.ok = true;
            result.outcome.finalUrl = job.url;
            result.outcome.title = "Кэш";
            result.outcome.text = CachePageText();
            result.ok = true;
            result.statusText = L"Содержимое кэша.";
        } else if (job.url == std::wstring(kInternalPrefix) + L"ip") {
            Response ip = Request(L"https://api.ipify.org", CacheMode::Bypass);
            result.outcome.ok = ip.ok;
            result.outcome.finalUrl = job.url;
            result.outcome.title = "Мой IP";
            result.outcome.text = ip.ok
                ? "Внешний IP-адрес:\n\n" +
                  std::string(ip.body.begin(), ip.body.end()) + "\n"
                : "Не удалось получить IP — сервис недоступен.\n";
            result.ok = true;
            result.statusText = L"Внешний IP-адрес.";
        } else if (job.url.rfind(
                       std::wstring(kInternalPrefix) + L"define/", 0) == 0) {
            const std::string word = ToUtf8(job.url.substr(
                std::wstring(kInternalPrefix).size() + 7));
            result.outcome.ok = true;
            result.outcome.finalUrl = job.url;
            result.outcome.title = "Словарь";
            result.outcome.text = DefinePageText(word);
            result.ok = true;
            result.statusText = L"Словарь — dictionaryapi.dev.";
        } else if (job.url.rfind(
                       std::wstring(kInternalPrefix) + L"tr/", 0) == 0) {
            const std::string text = ToUtf8(job.url.substr(
                std::wstring(kInternalPrefix).size() + 3));
            result.outcome.ok = true;
            result.outcome.finalUrl = job.url;
            result.outcome.title = "Перевод";
            result.outcome.text = TrPageText(text);
            result.ok = true;
            result.statusText = L"Перевод — Google gtx.";
        } else if (job.url == std::wstring(kInternalPrefix) + L"stats") {
            result.outcome.ok = true;
            result.outcome.finalUrl = job.url;
            result.outcome.title = "Статистика";
            result.outcome.text = StatsPageText();
            result.ok = true;
            result.statusText = L"Статистика сети, кэша и сессии.";
        } else if (job.url == std::wstring(kInternalPrefix) + L"help") {
            result.outcome.ok = true;
            result.outcome.finalUrl = job.url;
            result.outcome.title = "Справка";
            result.outcome.text = HelpPageText();
            result.ok = true;
            result.statusText = L"Справка по командам и клавишам.";
        } else if (job.url == std::wstring(kInternalPrefix) + L"settings") {
            result.outcome.ok = true;
            result.outcome.finalUrl = job.url;
            result.outcome.title = "Настройки";
            result.outcome.text = SettingsPageText();
            result.ok = true;
            result.statusText = L"Текущие настройки браузера.";
        } else if (job.url == std::wstring(kInternalPrefix) + L"feeds") {
            result.outcome = BuildFeedsPage();
            result.ok = true;
            result.statusText = L"RSS-ленты — feed add <url>.";
        } else {
            // Ecosystem pages: queue, digest, sync, headers, cookies,
            // netlog, rules, wiki[/name].
            result.outcome = BuildExtraInternalPage(job.url);
            result.ok = result.outcome.ok;
            result.statusText = result.ok
                ? L"Страница браузера."
                : L"Неизвестная внутренняя страница.";
        }
    } else if (job.kind == LoadJob::Kind::Ai) {
        result.outcome = AiAsk(job.query, job.aiContext);
        result.ok = result.outcome.ok;
        result.statusText = result.ok
            ? L"Ответ ИИ готов."
            : (result.outcome.error.empty()
                   ? L"ИИ не ответил."
                   : result.outcome.error);
        if (result.ok) g_lastAiOutcome = result.outcome;
    } else if (job.kind == LoadJob::Kind::Url) {
        result.outcome = FetchPage(job.url, job.mode, &g_loadCancel,
            job.postBody.empty() ? nullptr : &job.postBody,
            &g_loadDownloaded);
        result.ok = result.outcome.ok;
        result.cancelled = result.outcome.cancelled;
        // JS-heavy shell with almost no text → retry via headless Edge.
        if (result.ok && LooksLikeJsHeavy(result.outcome) &&
            !FindEdgePath().empty()) {
            FetchOutcome edge = EdgeDomFetch(job.url);
            if (edge.ok) {
                edge.fromCache = result.outcome.fromCache;
                result.outcome = std::move(edge);
            }
        }
        if (result.ok) {
            result.statusText = result.outcome.fromCache
                ? L"Страница загружена из кэша."
                : L"Готово.";
        } else {
            result.statusText = result.outcome.error.empty()
                ? L"Не удалось загрузить страницу."
                : result.outcome.error;
        }
    } else if (job.kind == LoadJob::Kind::EdgeDom) {
        result.outcome = EdgeDomFetch(job.url);
        result.ok = result.outcome.ok;
        result.statusText = result.ok
            ? L"Страница отрисована headless Edge (JS)."
            : (result.outcome.error.empty()
                   ? L"Headless Edge: ошибка."
                   : result.outcome.error);
    } else {
        result.outcome = SearchProviders(job.query, result.providerName,
                                         &g_loadCancel, &g_loadDownloaded);
        result.ok = result.outcome.ok;
        result.cancelled = result.outcome.cancelled;
        if (result.ok) {
            result.statusText = L"Результаты: " + result.providerName;
        } else {
            result.statusText = result.outcome.error.empty()
                ? L"Поиск недоступен."
                : result.outcome.error;
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_loadMutex);
        g_finishedLoad = std::move(result);
        g_loadRunning.store(false);
        g_loadingTabIdx.store(-1);
    }
}

void StartJobNow(const LoadJob& job) {
    if (g_loadThread.joinable())
        g_loadThread.join();

    g_loadCancel.store(false);
    g_loadDownloaded.store(0);
    g_loadTotal.store(0);
    g_loadStart = Clock::now();
    g_loadRunning.store(true);
    g_loadingTabIdx.store(static_cast<long>(job.tabIndex));
    g_loadThread = std::thread(LoadWorker, job);
}

void QueueOrStart(const LoadJob& job) {
    if (g_loadRunning.load()) {
        {
            std::lock_guard<std::mutex> lock(g_loadMutex);
            if (g_queuedJobs.size() < 16) // bounded: don't grow forever
                g_queuedJobs.push_back(job);
        }
        g_consoleStatus = L"Загрузка… (в очереди)";
        return;
    }
    StartJobNow(job);
    g_consoleStatus = L"Загрузка…";
}

static void MaybeAutoPrefetch(const LoadJob& job,
                              const FetchOutcome& outcome);

void ApplyResult(const LoadResult& result) {
    const LoadJob& job = result.job;
    if (!result.ok) {
        // No HTTP status at all → the request never reached the server.
        g_offline = result.outcome.status == 0;
        g_consoleStatus = result.statusText;
        return;
    }
    g_offline = false;

    EnsureTabs();
    const size_t ti = job.tabIndex < g_tabs.size()
        ? job.tabIndex : g_activeTab;

    // The load was started for a different (background) tab: commit the
    // outcome straight into that tab's slot without touching the UI.
    if (ti != g_activeTab) {
        TabState& tab = g_tabs[ti];
        tab.url = result.outcome.finalUrl;
        tab.title = result.outcome.title;
        tab.text = result.outcome.text;
        tab.html = result.outcome.html;
        tab.links = result.outcome.links;
        tab.images = result.outcome.images;
        tab.fields = result.outcome.fields;
        tab.forms = result.outcome.forms;
        tab.pageOpen = true;
        if (job.addToNavigation) {
            if (tab.navIndex + 1 < tab.nav.size())
                tab.nav.erase(tab.nav.begin() +
                              static_cast<long>(tab.navIndex) + 1,
                              tab.nav.end());
            if (tab.nav.empty() || tab.nav.back() != tab.url)
                tab.nav.push_back(tab.url);
            tab.navIndex = tab.nav.size() - 1;
        }
        if (job.recordVisit)
            AppendHistory(tab.url);
        else if (job.navIndexTarget >= 0 &&
                 static_cast<size_t>(job.navIndexTarget) < tab.nav.size())
            tab.navIndex = static_cast<size_t>(job.navIndexTarget);
        g_consoleStatus = L"Страница загружена во вкладке " +
            std::to_wstring(ti + 1) + L".";
        // Audible ping for background-tab loads — like a browser's
        // "page loaded" notification.
        if (ti != g_activeTab) Beep(1600, 60);
        return;
    }

    CommitOutcome(result.outcome);

    if (job.addToNavigation) {
        CommitTabNavigation(true, job.recordVisit, result.outcome.finalUrl);
    } else {
        if (job.recordVisit)
            AppendHistory(result.outcome.finalUrl);
        TabState& tab = ActiveTab();
        if (job.navIndexTarget >= 0 &&
            static_cast<size_t>(job.navIndexTarget) < tab.nav.size())
            tab.navIndex = static_cast<size_t>(job.navIndexTarget);
    }

    g_consoleStatus = result.statusText;
    if (result.outcome.title == "Unsupported content")
        g_consoleStatus = L"Этот файл нельзя показать в консоли — "
                          L"нажмите d, чтобы скачать его.";
    MaybeAutoPrefetch(job, result.outcome);
}

// Build an in-memory "page" for history / bookmarks.
void CommitInternalPage(const std::wstring& kind) {
    ParsedPage page;
    std::vector<HistoryEntry> entries;

    if (kind == L"history") {
        // Most recent first, duplicates removed, with visit times.
        auto all = ReadHistoryEntries(HistoryFilePath());
        std::vector<HistoryEntry> reversed(all.rbegin(), all.rend());
        std::unordered_set<std::wstring> seen;
        for (const auto& entry : reversed) {
            if (seen.insert(entry.url).second)
                entries.push_back(entry);
            if (entries.size() >= 200) break;
        }
        page.title = "История посещений";
    } else {
        for (const auto& url : LoadBookmarks())
            entries.push_back({url, 0});
        page.title = "Закладки";
    }

    std::string text;
    for (size_t i = 0; i < entries.size(); ++i) {
        std::string label = std::to_string(i + 1) + ". " + ToUtf8(entries[i].url);
        const std::string when = FormatVisitTime(entries[i].timestamp);
        if (!when.empty())
            label += "  —  " + when;
        text += label;
        text += '\n';
        page.links.push_back({label, entries[i].url});
    }
    if (entries.empty())
        text = kind == L"history"
            ? "История пуста. Выполните поиск или откройте сайт."
            : "Закладок нет. Нажмите Ctrl+D на любой странице, чтобы сохранить её.";
    page.text = NormalizeDisplayText(text);

    g_currentUrl = std::wstring(kInternalPrefix) + kind;
    g_currentTitle = page.title;
    g_currentText = std::move(page.text);
    g_currentHtml.clear();
    g_currentLinks = std::move(page.links);
    g_currentImages.clear();
    g_currentFields.clear();
    g_currentForms.clear();
    g_isFeed = false;
    g_consolePageOpen = true;
}

// Fire-and-forget prefetch of the first few links after a page load.
// Uses the same Request() path as the manual prefetch command.
static void AutoPrefetchWorker(std::vector<std::wstring> urls) {
    for (const auto& u : urls) {
        try { Request(u, CacheMode::Use, nullptr); }
        catch (...) {}
    }
}

static void MaybeAutoPrefetch(const LoadJob& job,
                              const FetchOutcome& outcome) {
    if (!g_consoleUiMode || !g_prefetch || !job.addToNavigation ||
        job.kind != LoadJob::Kind::Url || !outcome.ok)
        return;
    std::vector<std::wstring> urls;
    for (const auto& link : outcome.links) {
        const std::wstring lu = LowerWide(link.url);
        if (lu.rfind(L"http://", 0) == 0 ||
            lu.rfind(L"https://", 0) == 0)
            urls.push_back(link.url);
        if (urls.size() >= 4) break;
    }
    if (urls.empty()) return;
    std::thread(AutoPrefetchWorker, std::move(urls)).detach();
}

} // namespace

// Commit a fully-built in-memory page (view-source / page info). Like the
// history/bookmarks pages it does not create a nav entry — Back still
// returns to the previous page via the existing stack.
static void CommitCustomPage(const std::wstring& url,
                             const std::string& title,
                             const std::string& text,
                             std::vector<Link> links) {
    g_currentUrl = url;
    g_currentTitle = title;
    g_currentText = NormalizeDisplayText(text);
    g_currentHtml.clear();
    g_currentLinks = std::move(links);
    g_currentImages.clear();
    g_currentFields.clear();
    g_currentForms.clear();
    g_isFeed = false;
    g_consolePageOpen = true;
    g_consoleStatus.clear();
}

void UiViewSourcePage() {
    if (g_currentHtml.empty() || IsInternalPageUrl(g_currentUrl)) {
        g_consoleStatus = L"Исходный код недоступен для этой страницы.";
        return;
    }
    const std::string source = g_currentHtml;
    CommitCustomPage(L"view-source://" + g_currentUrl,
                     "View-source: " + g_currentTitle, source, {});
    g_currentHtml = source; // keep it so F5 can rebuild this page
    g_consoleStatus = L"Исходный код страницы.";
}

void UiPageInfoPage() {
    if (!g_consolePageOpen || g_currentUrl.empty()) {
        g_consoleStatus = L"Сначала откройте страницу.";
        return;
    }
    std::string text;
    auto line = [&](const std::string& s) { text += s; text += '\n'; };
    line("■ Информация о странице\n");
    line("▎ URL: " + ToUtf8(g_currentUrl));
    line("▎ Заголовок: " + (g_currentTitle.empty()
                            ? std::string("(без названия)") : g_currentTitle));
    line("▎ Текст: " + std::to_string(g_currentText.size()) + " символов");
    line("▎ Ссылок: " + std::to_string(g_currentLinks.size()) +
         " · Картинок: " + std::to_string(g_currentImages.size()) +
         " · Полей: " + std::to_string(g_currentFields.size()) +
         " · Форм: " + std::to_string(g_currentForms.size()));
    line("▎ Сеть: " + std::to_string(g_networkRequests.load()) +
         " запросов · из кэша " + std::to_string(g_cacheHits.load()));
    line("▎ Скачано за сеанс: " + std::to_string(g_downloadedBytes.load()) +
         " байт");
    CommitCustomPage(L"info://page", "Информация о странице", text, {});
}

static std::wstring FmtBytes(uint64_t bytes) {
    if (bytes >= 1024 * 1024)
        return std::to_wstring(bytes / (1024 * 1024)) + L" МБ";
    if (bytes >= 1024)
        return std::to_wstring(bytes / 1024) + L" КБ";
    return std::to_wstring(bytes) + L" Б";
}

// Session start timestamp for the uptime counter in browser://stats.
static const Clock::time_point g_sessionStart = Clock::now();

// browser://stats page body — network/cache/download counters.
std::string StatsPageText() {
    uint64_t cacheBytes = 0;
    size_t cacheFiles = 0;
    std::error_code ec;
    if (fs::exists(CacheDirPath(), ec)) {
        for (const auto& entry : fs::recursive_directory_iterator(
                 CacheDirPath(), fs::directory_options::skip_permission_denied,
                 ec)) {
            if (entry.is_regular_file(ec)) {
                cacheBytes += entry.file_size(ec);
                ++cacheFiles;
            }
        }
    }
    size_t dlFiles = 0;
    if (fs::exists(DownloadsDirPath(), ec)) {
        for (const auto& entry : fs::recursive_directory_iterator(
                 DownloadsDirPath(),
                 fs::directory_options::skip_permission_denied, ec)) {
            if (entry.is_regular_file(ec)) ++dlFiles;
        }
    }
    // Process RAM + session uptime.
    uint64_t ramMB = 0;
    PROCESS_MEMORY_COUNTERS pmc{};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc,
                                sizeof(pmc)))
        ramMB = pmc.WorkingSetSize / (1024 * 1024);
    const auto up = Clock::now() - g_sessionStart;
    const long upMin = std::chrono::duration_cast<
        std::chrono::minutes>(up).count();
    const std::wstring upStr = upMin >= 60
        ? std::to_wstring(upMin / 60) + L" ч " +
          std::to_wstring(upMin % 60) + L" мин"
        : std::to_wstring(upMin) + L" мин";
    const std::wstring text =
        L"Статистика браузера\n"
        L"\n"
        L"■ СЕТЬ\n"
        L"  HTTP-запросов: " + std::to_wstring(g_networkRequests.load()) +
        L"\n  Отдано из кэша: " + std::to_wstring(g_cacheHits.load()) +
        L"\n  Сейчас офлайн: " + std::wstring(g_offline ? L"да" : L"нет") +
        L"\n\n■ ХРАНИЛИЩЕ\n"
        L"  Файлов в кэше: " + std::to_wstring(cacheFiles) +
        L" (" + FmtBytes(cacheBytes) + L")\n"
        L"  Файлов в загрузках: " + std::to_wstring(dlFiles) +
        L"\n  Скачано всего: " + FmtBytes(g_downloadedBytes.load()) +
        L"\n\n■ СЕССИЯ\n"
        L"  Открытых вкладок: " + std::to_wstring(UiTabCount()) +
        L"\n  Активная вкладка: " +
        std::to_wstring(UiActiveTabIndex() + 1) +
        L"\n  RAM процесса: " + std::to_wstring(ramMB) + L" МБ\n"
        L"  Аптайм: " + upStr +
        L"\n  Элементов на странице: " +
        std::to_wstring(g_currentLinks.size() + g_currentImages.size() +
                        g_currentFields.size()) +
        L"\n  Движок: Browser Console " + ToWide(kAppVersion) + L"\n";

    // Top sites over the last 7 days (from history timestamps).
    std::wstring topSites = L"\n■ ТОП САЙТОВ (7 дней)\n";
    {
        const uint64_t cutoff = UnixNow() - 7ULL * 86400;
        std::map<std::wstring, size_t> counts;
        for (const auto& e : ReadHistoryEntries(HistoryFilePath())) {
            if (!e.timestamp || e.timestamp < cutoff) continue;
            if (IsInternalPageUrl(e.url) ||
                e.url.rfind(L"http", 0) != 0) continue;
            std::wstring h = e.url;
            const size_t sc = h.find(L"://");
            if (sc != std::wstring::npos) h = h.substr(sc + 3);
            const size_t sl = h.find_first_of(L"/?#");
            if (sl != std::wstring::npos) h = h.substr(0, sl);
            const size_t dot = h.rfind(L'.');
            if (dot != std::wstring::npos && dot + 1 < h.size())
                h = h.substr(0, dot); // strip TLD for grouping
            if (h.rfind(L"www.", 0) == 0) h = h.substr(4);
            if (!h.empty()) ++counts[LowerWide(h)];
        }
        std::vector<std::pair<std::wstring, size_t>> top(
            counts.begin(), counts.end());
        std::sort(top.begin(), top.end(),
                  [](const auto& a, const auto& b) {
                      return a.second > b.second;
                  });
        if (top.empty()) topSites += L"  пока пусто\n";
        for (size_t i = 0; i < top.size() && i < 10; ++i)
            topSites += L"  " + std::to_wstring(i + 1) + L". " +
                        top[i].first + L" — " +
                        std::to_wstring(top[i].second) + L"\n";
    }
    return ToUtf8(text + topSites);
}

// browser://settings page body — live view of every persisted toggle.
std::string SettingsPageText() {
    const auto yn = [](bool v) { return v ? L"вкл" : L"выкл"; };
    const std::wstring engine = UiGetSetting(L"engine");
    const std::wstring home = UiHomepage();
    const std::wstring text =
        L"Настройки браузера\n"
        L"Текущие значения и команды для переключения.\n\n"
        L"■ ВИД\n"
        L"  Тема: " + [&]{ std::wstring t = UiGetSetting(L"theme");
            return t.empty() ? L"тёмная (по умолчанию)" : t; }() +
        L"   (theme <имя> — список: theme)\n"
        L"  Масштаб текста: " + std::to_wstring(UiZoomPercent()) + L"%" +
        L"   (zoom in|out|<%>)\n"
        L"  Строка состояния: " + yn(UiStatusBarOn()) +
        L"   (statusbar on|off)\n"
        L"  Картинки: " + yn(UiImagesEnabled()) +
        L"   (images on|off, noimg)\n"
        L"  Автоподгрузка ссылок: " + yn(UiPrefetchEnabled()) +
        L"   (prefetch on|off)\n"
        L"\n\n"
        L"■ СЕТЬ\n"
        L"  Офлайн-режим (только кэш): " + yn(UiOfflineEnabled()) +
        L"   (offline on|off)\n"
        L"  Поисковик: " + (engine.empty() ? L"авто (DDG→Bing→Google)"
                                           : engine) +
        L"   (engine ddg|bing|google)\n"
        L"  Домашняя страница: " + (home.empty() ? L"стартовая" : home) +
        L"   (home <url>)\n\n"
        L"■ ДАННЫЕ\n"
        L"  Заметок: см. notes   ·   Закладок: см. bookmarks\n"
        L"  Сессии: sessions / restore <имя>   ·   Бэкап: export\n"
        L"  RSS-лент: feeds   ·   Кэш: cache   ·   Статистика: stats\n";
    return ToUtf8(text);
}

void UiShowStats() {
    uint64_t cacheBytes = 0;
    size_t cacheFiles = 0;
    std::error_code ec;
    if (fs::exists(CacheDirPath(), ec)) {
        for (const auto& entry : fs::recursive_directory_iterator(
                 CacheDirPath(), fs::directory_options::skip_permission_denied,
                 ec)) {
            if (entry.is_regular_file(ec)) {
                cacheBytes += entry.file_size(ec);
                ++cacheFiles;
            }
        }
    }
    size_t dlFiles = 0;
    if (fs::exists(DownloadsDirPath(), ec)) {
        for (const auto& entry : fs::recursive_directory_iterator(
                 DownloadsDirPath(),
                 fs::directory_options::skip_permission_denied, ec)) {
            if (entry.is_regular_file(ec)) ++dlFiles;
        }
    }
    g_consoleStatus =
        L"Запросов: " + std::to_wstring(g_networkRequests.load()) +
        L" · из кэша: " + std::to_wstring(g_cacheHits.load()) +
        L" · кэш: " + std::to_wstring(cacheFiles) + L" файлов, " +
        FmtBytes(cacheBytes) +
        L" · загрузок: " + std::to_wstring(dlFiles) +
        L" · вкладок: " + std::to_wstring(UiTabCount()) +
        L" · скачано: " + FmtBytes(g_downloadedBytes.load());
}

void UiReaderMode() {
    if (!g_consolePageOpen || g_currentText.empty() ||
        IsInternalPageUrl(g_currentUrl)) {
        g_consoleStatus = L"Режим чтения доступен только для обычных страниц.";
        return;
    }
    const std::string filtered = FilterReaderText(g_currentText);
    if (filtered.empty() || filtered.size() > g_currentText.size() * 9 / 10) {
        g_consoleStatus = L"На странице почти нет лишнего — фильтровать нечего.";
        return;
    }
    const std::string source = g_currentHtml;
    const std::vector<Link> links = g_currentLinks;
    g_currentTitle = "Чтение — " + g_currentTitle;
    g_currentText = NormalizeDisplayText(filtered);
    g_currentHtml = source; // keep source for view-source (u)
    g_currentLinks = links;
    g_consoleStatus = L"Режим чтения: показан основной текст.";
}

TabViewState UiOpenInBackgroundTab(const std::wstring& rawUrl,
                                   const TabViewState& view) {
    const std::wstring url = NormalizeUrl(rawUrl);
    if (url.empty()) {
        g_consoleStatus = L"Некорректный адрес сайта.";
        return view;
    }
    EnsureTabs();
    if (g_tabs.size() >= MAX_TABS) {
        g_consoleStatus = L"Слишком много вкладок (максимум " +
            std::to_wstring(MAX_TABS) + L").";
        return view;
    }
    // New tab at the end; the current tab/view stays untouched and the
    // load commits into the background tab's slot on completion.
    g_tabs.emplace_back();
    const size_t newIndex = g_tabs.size() - 1;
    LoadJob job;
    job.kind = LoadJob::Kind::Url;
    job.url = url;
    job.mode = CacheMode::Use;
    job.addToNavigation = true;
    job.recordVisit = true;
    job.tabIndex = newIndex;
    QueueOrStart(job);
    g_consoleStatus = L"Открыто в фоновой вкладке " +
        std::to_wstring(newIndex + 1) + L".";
    return view;
}

// Copy the persistent data files into data/backups/<date>/ once a day.
void MaybeDailyBackup() {
    static const char* kFiles[] = {
        "history.txt", "bookmarks.txt", "tabs.txt", "settings.txt" };
    const fs::path dir = DataDirPath();
    std::error_code ec;
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    wchar_t date[16];
    swprintf_s(date, L"%04d-%02d-%02d",
               tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    const fs::path marker = dir / L"backups" / L"last.txt";
    {
        std::wifstream in(marker);
        std::wstring last;
        std::getline(in, last);
        if (last == date) return; // already backed up today
    }
    const fs::path dest = dir / L"backups" / date;
    fs::create_directories(dest, ec);
    for (const char* name : kFiles) {
        const fs::path src = dir / ToWide(name);
        if (fs::exists(src, ec))
            fs::copy_file(src, dest / ToWide(name),
                          fs::copy_options::overwrite_existing, ec);
    }
    std::wofstream out(marker, std::ios::trunc);
    out << date << L'\n';
}

// Bound the cache: delete oldest files until under ~500 MB.
void EvictCacheOverflow() {
    const uint64_t kMaxBytes = 500ULL * 1024 * 1024;
    const fs::path root = CacheDirPath();
    std::error_code ec;
    if (!fs::exists(root, ec)) return;
    struct F { fs::path p; std::filesystem::file_time_type t; uint64_t sz; };
    std::vector<F> files;
    uint64_t total = 0;
    for (const auto& e : fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied, ec)) {
        if (!e.is_regular_file(ec)) continue;
        const uint64_t sz = e.file_size(ec);
        files.push_back({e.path(), e.last_write_time(ec), sz});
        total += sz;
    }
    if (total <= kMaxBytes) return;
    std::sort(files.begin(), files.end(),
              [](const F& a, const F& b) { return a.t < b.t; });
    for (const auto& f : files) {
        if (total <= kMaxBytes) break;
        fs::remove(f.p, ec);
        total -= f.sz;
    }
}

// Full session export: tabs + history + bookmarks in one file.
// Format is line-based and re-importable by hand or by sync tooling.
std::wstring UiExportSession() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    wchar_t stamp[32];
    swprintf_s(stamp, L"%04d%02d%02d-%02d%02d%02d",
               tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
               tm.tm_hour, tm.tm_min, tm.tm_sec);
    const fs::path path = SavedDirPath() /
        (L"session-" + std::wstring(stamp) + L".txt");
    std::wofstream out(path, std::ios::trunc);
    if (!out) {
        g_consoleStatus = L"Не удалось создать файл сессии.";
        return {};
    }
    out << L"# Browser session export\n";
    out << L"# sections: TABS / HISTORY / BOOKMARKS\n\n";
    out << L"[TABS]\n";
    EnsureTabs();
    for (const auto& tab : g_tabs)
        if (!tab.url.empty()) out << tab.url << L'\n';
    out << L"\n[HISTORY]\n";
    {
        std::wifstream in(HistoryFilePath());
        std::wstring line;
        while (std::getline(in, line)) out << line << L'\n';
    }
    out << L"\n[BOOKMARKS]\n";
    {
        std::wifstream in(BookmarksFilePath());
        std::wstring line;
        while (std::getline(in, line)) out << line << L'\n';
    }
    g_consoleStatus = L"Сессия сохранена: " + path.filename().wstring();
    return path;
}

void UiShowTabs() {
    EnsureTabs();
    const size_t count = UiTabCount();
    const size_t active = UiActiveTabIndex();
    std::wstring s = L"Вкладки (" + std::to_wstring(count) + L"): ";
    for (size_t i = 0; i < count && i < 12; ++i) {
        if (i) s += L" | ";
        std::wstring title = UiTabTitle(i);
        if (title.size() > 24) title = title.substr(0, 21) + L"...";
        s += std::to_wstring(i + 1) + L":" + title;
        if (i == active) s += L"*";
    }
    if (count > 12) s += L" | …";
    g_consoleStatus = s;
}


// ---------------------------------------------------------------------------
// Async navigation API used by the console UI
// ---------------------------------------------------------------------------

void UiOpenUrl(const std::wstring& rawUrl, CacheMode mode) {
    EnsureTabs();
    const std::wstring url = NormalizeUrl(rawUrl);
    if (url.empty()) {
        g_consoleStatus = L"Некорректный адрес сайта.";
        return;
    }
    LoadJob job;
    job.kind = LoadJob::Kind::Url;
    job.url = url;
    job.mode = mode;
    job.addToNavigation = true;
    job.recordVisit = true;
    job.tabIndex = g_activeTab;
    QueueOrStart(job);
}

// ai <вопрос> | ai @ <вопрос> | ai sum — ask the built-in AI
// (Pollinations.ai, free, no key). '@'/'page' adds the current page as
// context; 'sum' summarizes it.
void UiAi(const std::wstring& args) {
    std::wstring a = TrimWide(args);
    if (a.empty()) {
        g_consoleStatus =
            L"Формат: ai <вопрос> · ai @ <вопрос> (контекст страницы) · "
            L"ai sum (суть страницы). ИИ: Pollinations.ai, без ключа.";
        return;
    }
    bool withPage = false;
    std::wstring lower = LowerWide(a);
    if (lower == L"sum" || lower == L"summary" || lower == L"суть" ||
        lower == L"перескажи") {
        withPage = true;
        a = L"Кратко перескажи главное из этого текста.";
    } else if (lower.rfind(L"@ ", 0) == 0 ||
               lower.rfind(L"page ", 0) == 0) {
        withPage = true;
        a = TrimWide(a.substr(2));
        if (a.empty()) {
            g_consoleStatus = L"Формат: ai @ <вопрос>";
            return;
        }
    }
    if (withPage && g_currentText.empty()) {
        g_consoleStatus = L"Нет открытой страницы для контекста.";
        return;
    }
    EnsureTabs();
    LoadJob job;
    job.kind = LoadJob::Kind::Ai;
    job.query = ToUtf8(a);
    job.aiContext = withPage;
    job.addToNavigation = true;
    job.recordVisit = true;
    job.tabIndex = g_activeTab;
    QueueOrStart(job);
}

void UiOpenUrlEdge(const std::wstring& rawUrl) {
    const std::wstring url = NormalizeUrl(rawUrl);
    if (url.empty()) {
        g_consoleStatus = L"Некорректный адрес сайта.";
        return;
    }
    EnsureTabs();
    LoadJob job;
    job.kind = LoadJob::Kind::EdgeDom;
    job.url = url;
    job.addToNavigation = true;
    job.recordVisit = true;
    job.tabIndex = g_activeTab;
    QueueOrStart(job);
}

void UiSearch(const std::string& query) {
    if (query.empty()) {
        g_consoleStatus = L"Введите поисковый запрос.";
        return;
    }
    EnsureTabs();
    LoadJob job;
    job.kind = LoadJob::Kind::Search;
    job.query = query;
    job.addToNavigation = true;
    job.recordVisit = true;
    job.tabIndex = g_activeTab;
    QueueOrStart(job);
}

void UiBack() {
    const TabState& tab = ActiveTab();
    if (tab.navIndex == 0) {
        g_consoleStatus = L"Предыдущей страницы нет.";
        return;
    }
    const int target = static_cast<int>(tab.navIndex) - 1;
    EnsureTabs();
    const std::wstring targetUrl = tab.nav[static_cast<size_t>(target)];
    const bool internal = IsInternalPageUrl(targetUrl);
    LoadJob job;
    job.kind = internal ? LoadJob::Kind::Internal : LoadJob::Kind::Url;
    job.url = targetUrl;
    job.mode = internal ? CacheMode::Bypass : CacheMode::Use;
    job.addToNavigation = false;
    job.recordVisit = false;
    job.navIndexTarget = target;
    job.tabIndex = g_activeTab;
    QueueOrStart(job);
}

void UiForward() {
    const TabState& tab = ActiveTab();
    if (tab.navIndex + 1 >= tab.nav.size()) {
        g_consoleStatus = L"Следующей страницы нет.";
        return;
    }
    const int target = static_cast<int>(tab.navIndex) + 1;
    EnsureTabs();
    const std::wstring targetUrl = tab.nav[static_cast<size_t>(target)];
    const bool internal = IsInternalPageUrl(targetUrl);
    LoadJob job;
    job.kind = internal ? LoadJob::Kind::Internal : LoadJob::Kind::Url;
    job.url = targetUrl;
    job.mode = internal ? CacheMode::Bypass : CacheMode::Use;
    job.addToNavigation = false;
    job.recordVisit = false;
    job.navIndexTarget = target;
    job.tabIndex = g_activeTab;
    QueueOrStart(job);
}

void UiReload() {
    if (g_currentUrl.empty()) {
        g_consoleStatus = L"Нет страницы для обновления.";
        return;
    }
    if (g_currentUrl.rfind(L"view-source://", 0) == 0) {
        // Re-commit the stored source (globals still hold it).
        CommitCustomPage(g_currentUrl, g_currentTitle, g_currentHtml, {});
        g_consoleStatus = L"Исходный код страницы.";
        return;
    }
    if (g_currentUrl.rfind(L"info://", 0) == 0) {
        g_consoleStatus = L"Информация о странице не обновляется — "
                          L"откройте её заново (клавиша i).";
        return;
    }
    if (IsInternalPageUrl(g_currentUrl)) {
        if (g_currentUrl.find(L"history") != std::wstring::npos)
            UiShowHistoryPage();
        else if (g_currentUrl.find(L"downloads") != std::wstring::npos)
            UiShowDownloadsPage();
        else
            UiShowBookmarksPage();
        return;
    }
    EnsureTabs();
    LoadJob job;
    job.kind = LoadJob::Kind::Url;
    job.url = g_currentUrl;
    job.mode = CacheMode::Revalidate;
    job.addToNavigation = false;
    job.recordVisit = false;
    job.tabIndex = g_activeTab;
    QueueOrStart(job);
}

void UiBookmark() { AddBookmark(); }

bool UiCopyToClipboard(const std::wstring& text) {
    if (text.empty() || !OpenClipboard(nullptr))
        return false;
    bool ok = false;
    if (EmptyClipboard()) {
        const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (mem) {
            void* dst = GlobalLock(mem);
            if (dst) {
                std::memcpy(dst, text.c_str(), bytes);
                GlobalUnlock(mem);
                ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
            }
            if (!ok) GlobalFree(mem);
        }
    }
    CloseClipboard();
    return ok;
}

std::wstring UiGetClipboardText() {
    if (!OpenClipboard(nullptr))
        return {};
    std::wstring out;
    if (const HANDLE hd = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* p = static_cast<const wchar_t*>(
                GlobalLock(hd))) {
            out = p;
            GlobalUnlock(hd);
        }
    }
    CloseClipboard();
    return out;
}

void UiCancelLoad() {
    {
        std::lock_guard<std::mutex> lock(g_loadMutex);
        g_queuedJobs.clear();
    }
    if (!g_loadRunning.load()) return;
    g_loadCancel.store(true);
    g_consoleStatus = L"Отмена загрузки…";
}

void UiShowHistoryPage() {
    CommitInternalPage(L"history");
    g_consoleStatus = L"История посещений. Клик по строке открывает страницу.";
}

void UiShowBookmarksPage() {
    CommitInternalPage(L"bookmarks");
    g_consoleStatus = L"Закладки. Ctrl+D на странице добавляет её сюда.";
}




// browser://help page body — commands and keys of the console UI.
std::string HelpPageText() {
    return
        "Справка — команды адресной строки и клавиши страницы.\n"
        "\n"
        "■ НАВИГАЦИЯ\n"
        "  back / forward / reload / home — история и перезагрузка\n"
        "  reload <сек> — автообновление страницы\n"
        "  history / bookmarks / downloads — внутренние страницы\n"
        "  download <url> [имя] — скачать файл\n"
        "  edge <url> — страница через headless Edge (JS)\n"
        "  web [url] — открыть в WebView2-окне\n"
        "  stop — отменить загрузку\n"
        "\n"
        "■ ПОИСК И ТЕКСТ\n"
        "  Любое слово без http → поиск в DuckDuckGo.\n"
        "  / или Ctrl+F — найти на странице, F3 — следующее.\n"
        "  ai <вопрос> — ИИ (Pollinations, без ключа).\n"
        "  ai @ <вопрос> — ИИ с учётом текста страницы.\n"
        "  ai sum — краткий пересказ страницы.\n"
        "\n"
        "■ ВЫДЕЛЕНИЕ И БУФЕР\n"
        "  Мышью: тянуть по любому тексту → Ctrl+C копирует.\n"
        "  V — выделение клавиатурой (j/k, g/G, y — копировать).\n"
        "  z / Ctrl+Shift+C — весь текст страницы.\n"
        "  c — ссылка под курсором, y — адрес страницы.\n"
        "  Ctrl+A/C/X/V/Z — выделить/копировать/вырезать/вставить/undo.\n"
        "\n"
        "■ ВКЛАДКИ И ФАЙЛЫ\n"
        "  Ctrl+T/W — новая/закрыть, Ctrl+Tab — следующая,\n"
        "  Ctrl+1..9 — вкладка по номеру, Ctrl+Shift+T — вернуть.\n"
        "  dup / reopen — дублировать/переоткрыть вкладку.\n"
        "  tab <n> — перейти, tabname <имя> — переименовать,\n"
        "  pin/unpin — закрепить, close-left/right — закрыть соседние,\n"
        "  tabmove left|right — переместить, new/close — алиасы.\n"
        "  s — сохранить HTML, Shift+S — Markdown.\n"
        "  Ctrl+D — закладка, Ctrl+S — сохранить, Ctrl+Y — история.\n"
        "\n"
        "■ ЕЩЁ КОМАНДЫ\n"
        "  Ctrl+Enter в адресной строке — открыть во вкладке фоном.\n"
        "  dl <url> / download <url> — скачать, up — на уровень выше.\n"
        "  hard-reload — обновить мимо кэша, offline on|off — только кэш.\n"
        "  engine <ddg|bing|google> — поисковик, source <url> — исходник.\n"
        "  images / linkpage — страницы картинок и ссылок, saveimg —\n"
        "  скачать картинку, imglinks — скопировать URL картинок.\n"
        "  copy url|domain|title|text|links|images|selection — буфер.\n"
        "  find <текст> — найти на странице, ' — регистр, Shift+F3 — назад.\n"
        "  theme <имя> — тема, theme save <имя> — сохранить свою,\n"
        "  zoom in|out|<%> — масштаб, F11 — полный\n"
        "  экран, statusbar/images/prefetch on|off — переключатели.\n"
        "  Ctrl+K / palette — командная палитра (команды, история,\n"
        "  закладки, вкладки). Боковые кнопки мыши — назад/вперёд.\n"
        "  feed add <url> / feed del <n> / feeds — RSS-подписки.\n"
        "  settings — страница настроек, update apply — обновиться.\n"
        "  note del <n> / notes clear — заметки, lucky — случайная страница,\n"
        "  snapshot — снимок экрана, sessions / restore <имя> — сессии.\n"
        "\n"
        "■ СИСТЕМА\n"
        "  stats — статистика сети и кэша.\n"
        "  backup / session — бэкап и экспорт сессии.\n"
        "  clear-history, clear-history-older <дн>, cache-clear.\n"
        "\n"
        "■ УТИЛИТЫ\n"
        "  about — о браузере, ip — внешний IP-адрес.\n"
        "  notes — заметки, note <текст> — добавить заметку.\n"
        "  cache — содержимое кэша.\n"
        "  define <слово> / dict — словарь (dictionaryapi.dev).\n"
        "  tr <текст> / translate — перевод (Google gtx).\n"
        "  w <запрос> / wiki — поиск по Википедии.\n"
        "  title / links / mdlink — заголовок/ссылки/MD-ссылка.\n"
        "  scroll <1-20> — строк за щелчок колеса.\n"
        "  home <url> — своя главная страница.\n"
        "  feedback / repo / site — GitHub-ссылки проекта.\n"
        "  ver — версия браузера.\n"
        "\n"
        "■ ЭКОСИСТЕМА\n"
        "  queue / queue add|next|del|clear — очередь чтения.\n"
        "  macro <имя> — макросы из data/macros.txt.\n"
        "  rules — декларативные правила data/rules/*.txt.\n"
        "  digest — дайджест RSS-лент за сутки.\n"
        "  export-md <дн> — статьи из истории в .md (saved/articles/).\n"
        "  wsave <статья> / woffline <имя> / wiki — офлайн-википедия.\n"
        "  headers / cookies / netlog — devtools-lite (ответ, куки, лог).\n"
        "  syncpage / sync push|pull — синхронизация Gist с merge.\n"
        "  register-protocol / fileassoc — интеграция с Windows.\n"
        "  theme pull <имя> — тема из онлайн-галереи.\n"
        "\n"
        "■ КЛАВИШИ НА СТРАНИЦЕ\n"
        "  j/k, Enter — ссылка, o — адрес, ,/. — назад/вперёд,\n"
        "  b/f — история/вперёд по ссылкам, g/G — края,\n"
        "  u — исходник, i — инфо, r — обновить, ? — эта справка.\n"
        "  Пробел/PgUp/PgDn — листать, Home/End — края.\n"
        "  Ctrl+Home/End — в начало/конец, Ctrl+PgUp/PgDn — блоки.\n"
        "  Колесо мыши — прокрутка, Esc — сброс выделения/отмена.\n"
        "  Ctrl+Q — выход.\n";
}

// --- Settings key=value store (data/settings.txt) ----------------------
std::wstring UiGetSetting(const std::wstring& key) {
    std::wifstream in(DataDirPath() / L"settings.txt");
    std::wstring line;
    while (std::getline(in, line)) {
        const size_t eq = line.find(L'=');
        if (eq != std::wstring::npos &&
            TrimWide(line.substr(0, eq)) == key)
            return TrimWide(line.substr(eq + 1));
    }
    return {};
}

void UiSetSetting(const std::wstring& key, const std::wstring& value) {
    const fs::path path = DataDirPath() / L"settings.txt";
    std::vector<std::wstring> lines;
    {
        std::wifstream in(path);
        std::wstring line;
        while (std::getline(in, line)) {
            const size_t eq = line.find(L'=');
            if (eq != std::wstring::npos &&
                TrimWide(line.substr(0, eq)) == key)
                continue; // drop old value for this key
            if (!TrimWide(line).empty()) lines.push_back(line);
        }
    }
    if (!value.empty()) lines.push_back(key + L"=" + value);
    std::wofstream out(path, std::ios::trunc);
    for (const auto& l : lines) out << l << '\n';
}

// Homepage override: "home <url>" in the omnibox persists here; empty
// means the built-in start page.
std::wstring UiHomepage() { return UiGetSetting(L"home"); }

// Open an arbitrary URL in the system browser (project links etc.).
void UiShellOpen(const std::wstring& url) {
    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr,
                  SW_SHOWNORMAL);
}

// --- browser://about ---------------------------------------------------
std::string AboutPageText() {
    return ToUtf8(
        L"Browser Console " + ToWide(kAppVersion) +
        L" — консольный браузер для Windows\n"
        L"\n"
        L"■ ЧТО ЭТО\n"
        L"  Полноценный браузер в терминале: HTTP/HTTPS, кэш, формы,\n"
        L"  вкладки, история, закладки, загрузки, RSS, ИИ, терминал.\n"
        L"\n"
        L"■ УПРАВЛЕНИЕ\n"
        L"  Мышь: выделение текста, клики по ссылкам и кнопкам.\n"
        L"  Клавиатура: j/k — элементы, Enter — открыть, Ctrl+L — адрес,\n"
        L"  / — найти, V — выделение, ? или команда help — справка.\n"
        L"\n"
        L"■ АВТОР\n"
        L"  fanxcz — github.com/fanxcz\n"
        L"\n"
        L"■ ССЫЛКИ (кликабельны ниже)\n"
        L"  Сайт проекта, исходники и актуальный релиз — на GitHub.\n");
}

// --- browser://notes (data/notes.txt) ----------------------------------
namespace {
fs::path NotesFilePath() { return DataDirPath() / L"notes.txt"; }
}

std::string NotesPageText() {
    std::ifstream in(NotesFilePath(), std::ios::binary);
    std::string all((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    if (all.empty())
        all =
            "Быстрые заметки.\n"
            "Команда `note <текст>` добавляет строку сюда — она хранится\n"
            "в data/notes.txt и доступна после перезапуска.\n"
            "`notes` — эта страница.\n";
    return all;
}

void UiAddNote(const std::wstring& text) {
    std::wofstream out(NotesFilePath(), std::ios::app);
    out << text << L'\n';
}

// --- browser://cache ----------------------------------------------------
std::string CachePageText() {
    uint64_t total = 0;
    std::vector<std::pair<std::wstring, uint64_t>> files;
    std::error_code ec;
    if (fs::exists(CacheDirPath(), ec)) {
        for (const auto& e : fs::recursive_directory_iterator(
                 CacheDirPath(),
                 fs::directory_options::skip_permission_denied, ec)) {
            if (!e.is_regular_file(ec)) continue;
            const uint64_t sz = e.file_size(ec);
            total += sz;
            files.push_back({e.path().filename().wstring(), sz});
        }
    }
    std::sort(files.begin(), files.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    std::wstring text =
        L"Кэш страниц — " + std::to_wstring(files.size()) +
        L" файлов, " + FmtBytes(total) + L"\n\n";
    size_t shown = 0;
    for (const auto& f : files) {
        if (shown++ >= 200) break;
        text += L"  " + f.first + L"  —  " + FmtBytes(f.second) + L"\n";
    }
    if (files.empty()) text += L"Кэш пуст.\n";
    return ToUtf8(text);
}

// --- browser://ip -------------------------------------------------------
// (worker fetches api.ipify.org — plain-text body)

// --- browser://define/<word> --------------------------------------------
// Free dictionary API (dictionaryapi.dev), no key required.
std::string DefinePageText(const std::string& wordEnc) {
    const std::string word = PercentDecode(wordEnc);
    if (word.empty()) return "Использование: define <слово>\n";
    Response r = Request(
        L"https://api.dictionaryapi.dev/api/v2/entries/en/" +
            ToWide(UrlEncode(word)),
        CacheMode::Bypass);
    if (!r.ok || r.status != 200)
        return "Слово не найдено или сервис недоступен: " + word + "\n";
    const std::string body(r.body.begin(), r.body.end());
    // Tolerant JSON scraping: pull fields in document order.
    struct Hit { size_t pos; char kind; std::string val; };
    std::vector<Hit> hits;
    static const std::regex re(
        "\\\"(partOfSpeech|phonetic|definition|example)\\\"\\s*:\\s*\\\"((?:[^\\\"\\\\]|\\\\.)*)\\\"");
    for (std::sregex_iterator it(body.begin(), body.end(), re), end; it != end; ++it) {
        const std::string kind = (*it)[1];
        hits.push_back({static_cast<size_t>(it->position()),
                        kind.empty() ? '?' : kind[0], (*it)[2]});
    }
    if (hits.empty()) return "Нет данных для слова: " + word + "\n";
    std::wstring out = L"Словарь: " + ToWide(word) + L"\n\n";
    size_t shown = 0;
    for (const auto& h : hits) {
        if (shown >= 14) break;
        ++shown;
        if (h.kind == 'p') out += L"■ " + ToWide(h.val) + L"\n";
        else if (h.kind == 'd') out += L"  • " + ToWide(h.val) + L"\n";
        else if (h.kind == 'e') out += L"    e.g. " + ToWide(h.val) + L"\n";
        else if (h.kind == 'h' && out.find(L'—') == std::wstring::npos)
            out += L"  " + ToWide(h.val) + L"\n";
    }
    return ToUtf8(out);
}

// --- browser://tr/<text> ------------------------------------------------
// Google Translate free endpoint (client=gtx), output language Russian.
std::string JsonUnescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            const char n = s[++i];
            if (n == 'n') out += '\n';
            else if (n == 't') out += '\t';
            else if (n == 'u' && i + 4 < s.size()) {
                unsigned cp = 0;
                for (int k = 0; k < 4; ++k) {
                    const char c = s[++i];
                    cp = cp * 16 + (c <= '9' ? c - '0'
                          : (c | 32) - 'a' + 10);
                }
                // UTF-8 encode the code point.
                if (cp < 0x80) out += static_cast<char>(cp);
                else if (cp < 0x800) {
                    out += static_cast<char>(0xC0 | (cp >> 6));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                } else {
                    out += static_cast<char>(0xE0 | (cp >> 12));
                    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                }
            } else out += n;
        } else out += s[i];
    }
    return out;
}

std::string TrPageText(const std::string& textEnc) {
    const std::string text = PercentDecode(textEnc);
    if (text.empty()) return "Использование: translate <текст>\n";
    Response r = Request(
        L"https://translate.googleapis.com/translate_a/single?client=gtx"
        L"&sl=auto&tl=ru&dt=t&q=" +
            ToWide(UrlEncode(text)),
        CacheMode::Bypass);
    if (!r.ok || r.status != 200)
        return "Перевод недоступен.\n";
    const std::string body(r.body.begin(), r.body.end());
    // Every inner segment is ["translated","source",...]: the first
    // quoted string of each [...] block is the translation.
    static const std::regex re("\\[\\s*\\\"((?:[^\\\"\\\\]|\\\\.)*)\\\"");
    std::wstring out;
    for (std::sregex_iterator it(body.begin(), body.end(), re), end;
         it != end; ++it) {
        out += ToWide(JsonUnescape((*it)[1]));
    }
    if (out.empty()) return "Перевод недоступен.\n";
    out = L"Перевод (auto → ru):\n\n" + out + L"\n";
    return ToUtf8(out);
}

// --- Update check: once a day ask GitHub for the latest release tag ---
namespace {
std::mutex g_updateMutex;
std::wstring g_updateNotice;

std::vector<int> VersionParts(const std::string& v) {
    std::vector<int> out;
    std::string cur;
    for (char c : v) {
        if (c >= '0' && c <= '9') cur += c;
        else if (!cur.empty()) {
            out.push_back(std::atoi(cur.c_str()));
            cur.clear();
        }
    }
    if (!cur.empty()) out.push_back(std::atoi(cur.c_str()));
    return out;
}

bool VersionNewer(const std::string& remote, const std::string& local) {
    const auto a = VersionParts(remote);
    const auto b = VersionParts(local);
    const size_t n = std::max(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const int x = i < a.size() ? a[i] : 0;
        const int y = i < b.size() ? b[i] : 0;
        if (x != y) return x > y;
    }
    return false;
}
} // namespace

void UiScheduleUpdateCheck() {
    std::thread([] {
        try {
            const fs::path stamp = DataDirPath() / L"update_check.txt";
            uint64_t last = 0;
            {
                std::wifstream f(stamp);
                f >> last;
            }
            const uint64_t now = UnixNow();
            if (last != 0 && now < last + 86400) return;
            {
                std::error_code ec;
                fs::create_directories(DataDirPath(), ec);
                std::wofstream f(stamp, std::ios::trunc);
                f << now;
            }
            Response r = Request(
                L"https://api.github.com/repos/fanxcz/browser/releases/latest",
                CacheMode::Bypass);
            if (!r.ok || r.status != 200) return;
            const std::string body(r.body.begin(), r.body.end());
            const size_t tp = body.find("\"tag_name\"");
            if (tp == std::string::npos) return;
            const size_t colon = body.find(':', tp);
            if (colon == std::string::npos) return;
            const size_t q1 = body.find('"', colon + 1);
            if (q1 == std::string::npos) return;
            const size_t q2 = body.find('"', q1 + 1);
            if (q2 == std::string::npos) return;
            std::string tag = body.substr(q1 + 1, q2 - q1 - 1);
            if (!tag.empty() && tag.front() == 'v') tag.erase(tag.begin());
            if (tag.empty() || !VersionNewer(tag, kAppVersion)) return;
            std::lock_guard<std::mutex> lock(g_updateMutex);
            g_updateNotice =
                L"Доступна новая версия Browser " + ToWide(tag) +
                L" — github.com/fanxcz/browser/releases";
        } catch (...) {}
    }).detach();
}

std::wstring UiTakeUpdateNotice() {
    std::lock_guard<std::mutex> lock(g_updateMutex);
    std::wstring out = std::move(g_updateNotice);
    g_updateNotice.clear();
    return out;
}

// update apply: fetch the newest GitHub release exe and swap it in.
// A running exe can be RENAMED on Windows (not deleted), so the swap is:
// Browser.exe -> Browser.exe.old, update-new.exe -> Browser.exe, relaunch.
std::wstring UiApplyUpdate() {
    Response r = Request(
        L"https://api.github.com/repos/fanxcz/browser/releases/latest",
        CacheMode::Bypass);
    if (!r.ok || r.status != 200)
        return L"Не удалось проверить обновления — нет сети.";
    const std::string body(r.body.begin(), r.body.end());

    auto jsonField = [&](const char* key) -> std::string {
        const std::string pat = std::string("\"") + key + "\"";
        const size_t p = body.find(pat);
        if (p == std::string::npos) return {};
        const size_t q1 = body.find('"', body.find(':', p) + 1);
        if (q1 == std::string::npos) return {};
        const size_t q2 = body.find('"', q1 + 1);
        return q2 == std::string::npos ? std::string()
                                     : body.substr(q1 + 1, q2 - q1 - 1);
    };
    std::string tag = jsonField("tag_name");
    if (!tag.empty() && tag.front() == 'v') tag.erase(tag.begin());
    if (tag.empty()) return L"В ответе GitHub нет tag_name.";
    if (!VersionNewer(tag, kAppVersion))
        return L"Уже установлена последняя версия " + ToWide(tag) + L".";

    const std::string dl = jsonField("browser_download_url");
    if (dl.empty()) return L"В релизе нет exe-ассета.";

    g_consoleStatus = L"Скачиваю Browser " + ToWide(tag) + L"…";
    Response asset = Request(ToWide(dl), CacheMode::Bypass);
    if (!asset.ok || asset.body.empty())
        return L"Не удалось скачать обновление.";

    const fs::path staged = DataDirPath() / L"update-new.exe";
    {
        std::ofstream out(staged, std::ios::binary | std::ios::trunc);
        out.write(asset.body.data(),
                  static_cast<std::streamsize>(asset.body.size()));
        if (!out) return L"Не удалось записать файл обновления.";
    }

    wchar_t cur[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, cur, MAX_PATH))
        return L"Не удалось определить путь к Browser.exe.";
    const std::wstring curPath = cur;
    const std::wstring oldPath = curPath + L".old";
    const std::wstring newPath = staged.wstring();

    std::error_code ec;
    fs::remove(oldPath, ec);
    if (!MoveFileW(curPath.c_str(), oldPath.c_str()))
        return L"Не удалось заменить Browser.exe — файл занят? "
                L"Закройте браузер и повторите.";
    if (!MoveFileW(newPath.c_str(), curPath.c_str())) {
        MoveFileW(oldPath.c_str(), curPath.c_str()); // rollback
        return L"Ошибка установки — прежний exe восстановлен.";
    }

    // Persist tabs so the fresh instance restores the session, then
    // launch it and get out of the way.
    SnapshotActiveTab();
    SaveTabs();
    std::wstring cmd = L"\"" + curPath + L"\"";
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                       0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    ExitProcess(0);
}

void UiOpenInternalPage(const std::wstring& kind) {
    LoadJob job;
    job.kind = LoadJob::Kind::Internal;
    job.url = std::wstring(kInternalPrefix) + kind;
    job.mode = CacheMode::Bypass;
    job.addToNavigation = true;
    job.recordVisit = false; // internal pages don't belong in history
    job.tabIndex = g_activeTab;
    QueueOrStart(job);
}


// Hands the current page URL to the system default browser.
void UiOpenExternalBrowser() {
    const std::wstring lower = LowerWide(g_currentUrl);
    if (lower.rfind(L"http://", 0) != 0 &&
        lower.rfind(L"https://", 0) != 0) {
        g_consoleStatus = L"Открыть во внешнем браузере можно только http(s)-страницу.";
        return;
    }
    const HINSTANCE rc = ShellExecuteW(nullptr, L"open", g_currentUrl.c_str(),
                                       nullptr, nullptr, SW_SHOWNORMAL);
    g_consoleStatus = rc > reinterpret_cast<HINSTANCE>(32)
        ? L"Открыто в системном браузере."
        : L"Не удалось открыть во внешнем браузере.";
}

void UiClearHistory() {
    ClearHistory();
    // Keep the in-memory navigation stack usable: rebuild it empty.
    g_navigation.clear();
    g_navigationIndex = 0;
    g_hasNavigation = false;
}

void UiClearBookmarks() { ClearBookmarks(); }
void UiClearCache() { ClearCache(); }

// ---------------------------------------------------------------------------
// Tab management
// ---------------------------------------------------------------------------

void UiSaveViewState(const TabViewState& view);

// Start the deferred load of a tab restored from data/tabs.txt.
static void MaybeStartPendingLoad() {
    EnsureTabs();
    TabState& tab = g_tabs[g_activeTab];
    if (!tab.pendingLoad || tab.url.empty())
        return;
    tab.pendingLoad = false;
    LoadJob job;
    job.kind = IsInternalPageUrl(tab.url)
        ? LoadJob::Kind::Internal : LoadJob::Kind::Url;
    job.url = tab.url;
    job.mode = CacheMode::Use;
    job.addToNavigation = true;
    job.recordVisit = false; // already in history from the previous session
    job.tabIndex = g_activeTab;
    QueueOrStart(job);
}

static TabViewState SwitchTo(size_t index, const TabViewState& view) {
    UiSaveViewState(view);
    g_activeTab = index;
    LoadTabToGlobals(g_tabs[g_activeTab]);
    MaybeStartPendingLoad();
    return g_tabs[g_activeTab].view;
}

TabViewState UiNewTab(const TabViewState& view) {
    EnsureTabs();
    if (g_tabs.size() >= MAX_TABS) {
        g_consoleStatus = L"Слишком много вкладок (максимум " +
            std::to_wstring(MAX_TABS) + L").";
        return view;
    }
    UiSaveViewState(view);
    g_tabs.emplace_back(); // fresh "new tab" page
    g_activeTab = g_tabs.size() - 1;
    LoadTabToGlobals(g_tabs[g_activeTab]);
    g_consoleStatus.clear();
    return {};
}

TabViewState UiOpenInNewTab(const std::wstring& rawUrl,
                            const TabViewState& view) {
    const bool internal = IsInternalPageUrl(rawUrl);
    const std::wstring url = internal ? rawUrl : NormalizeUrl(rawUrl);
    if (url.empty()) {
        g_consoleStatus = L"Некорректный адрес сайта.";
        return view;
    }
    EnsureTabs();
    if (g_tabs.size() >= MAX_TABS) {
        g_consoleStatus = L"Слишком много вкладок (максимум " +
            std::to_wstring(MAX_TABS) + L").";
        return view;
    }
    UiSaveViewState(view);
    g_tabs.emplace_back();
    g_activeTab = g_tabs.size() - 1;
    LoadTabToGlobals(g_tabs[g_activeTab]);

    LoadJob job;
    job.kind = internal ? LoadJob::Kind::Internal : LoadJob::Kind::Url;
    job.url = url;
    job.mode = internal ? CacheMode::Bypass : CacheMode::Use;
    job.addToNavigation = true;
    job.recordVisit = true;
    job.tabIndex = g_activeTab;
    QueueOrStart(job);
    g_consoleStatus.clear();
    return {};
}

TabViewState UiSwitchTabTo(size_t index, const TabViewState& view) {
    EnsureTabs();
    if (index >= g_tabs.size() || index == g_activeTab)
        return view;
    return SwitchTo(index, view);
}

TabViewState UiCycleTab(int delta, const TabViewState& view) {
    EnsureTabs();
    if (g_tabs.size() < 2)
        return view;
    const long long next =
        (static_cast<long long>(g_activeTab) + delta +
         static_cast<long long>(g_tabs.size())) %
        static_cast<long long>(g_tabs.size());
    return SwitchTo(static_cast<size_t>(next), view);
}

static TabViewState CloseTabAt(size_t index, const TabViewState& view,
                               bool wasActive) {
    EnsureTabs();
    // Remember the closed tab for Ctrl+Shift+T (full state, incl. content).
    if (!g_tabs[index].url.empty() || g_tabs[index].pageOpen) {
        g_closedTabs.push_back(g_tabs[index]);
        if (g_closedTabs.size() > MAX_CLOSED_TABS)
            g_closedTabs.erase(g_closedTabs.begin());
    }
    g_tabs.erase(g_tabs.begin() + static_cast<long>(index));
    if (g_tabs.empty())
        g_tabs.emplace_back();
    if (wasActive) {
        // Prefer the tab that slid into the same slot, else the previous one.
        g_activeTab = std::min(index, g_tabs.size() - 1);
        LoadTabToGlobals(g_tabs[g_activeTab]);
        MaybeStartPendingLoad();
        return g_tabs[g_activeTab].view;
    }
    if (index < g_activeTab) --g_activeTab;
    return view; // active tab untouched
}

TabViewState UiCloseTab(const TabViewState& view) {
    EnsureTabs();
    if (g_tabs[g_activeTab].pinned) {
        g_consoleStatus = L"Вкладка закреплена — сначала выполните unpin.";
        return view;
    }
    if (g_tabs.size() <= 1) {
        // Last tab: turn it into a fresh "new tab" page instead of closing.
        UiSaveViewState(view);
        g_closedTabs.push_back(g_tabs[g_activeTab]);
        if (g_closedTabs.size() > MAX_CLOSED_TABS)
            g_closedTabs.erase(g_closedTabs.begin());
        g_tabs[g_activeTab] = TabState{};
        LoadTabToGlobals(g_tabs[g_activeTab]);
        SaveTabs();
        g_consoleStatus = L"Последнюю вкладку нельзя закрыть — открыта новая.";
        return {};
    }
    const size_t index = g_activeTab;
    UiSaveViewState(view);
    return CloseTabAt(index, view, true);
}

TabViewState UiCloseTabIndex(size_t index, const TabViewState& view) {
    EnsureTabs();
    if (index >= g_tabs.size())
        return view;
    if (g_tabs[index].pinned) {
        g_consoleStatus = L"Вкладка закреплена — сначала выполните unpin.";
        return view;
    }
    if (g_tabs.size() <= 1) {
        if (index == g_activeTab)
            return UiCloseTab(view);
        return view;
    }
    const bool wasActive = index == g_activeTab;
    if (wasActive)
        UiSaveViewState(view);
    TabViewState result = CloseTabAt(index, view, wasActive);
    SaveTabs();
    return result;
}

TabViewState UiCloseOtherTabs(const TabViewState& view) {
    EnsureTabs();
    if (g_tabs.size() <= 1) {
        g_consoleStatus = L"Других вкладок нет.";
        return view;
    }
    UiSaveViewState(view);
    // Close from the back so indices stay valid; keep the active tab
    // and every pinned tab.
    size_t kept = 0;
    for (size_t i = g_tabs.size(); i-- > 0;) {
        if (i == g_activeTab || g_tabs[i].pinned) { ++kept; continue; }
        CloseTabAt(i, view, false);
    }
    SaveTabs();
    g_consoleStatus = kept > 1
        ? L"Закрыты все незакреплённые вкладки, кроме текущей."
        : L"Закрыты все вкладки, кроме текущей.";
    return g_tabs[g_activeTab].view;
}

// Close every tab before (before=true) or after the active one.
TabViewState UiCloseTabsOnSide(bool before, const TabViewState& view) {
    EnsureTabs();
    size_t closed = 0;
    UiSaveViewState(view);
    if (before) {
        // CloseTabAt shifts g_activeTab down for each removal, so always
        // pick the lowest unpinned tab left of the active one.
        for (;;) {
            size_t victim = g_tabs.size();
            for (size_t i = 0; i < g_activeTab && i < g_tabs.size(); ++i)
                if (!g_tabs[i].pinned) { victim = i; break; }
            if (victim == g_tabs.size()) break;
            CloseTabAt(victim, view, false);
            ++closed;
        }
    } else {
        for (size_t i = g_tabs.size(); i-- > g_activeTab + 1;) {
            if (g_tabs[i].pinned) continue;
            CloseTabAt(i, view, false);
            ++closed;
        }
    }
    SaveTabs();
    g_consoleStatus = closed
        ? L"Закрыто вкладок: " + std::to_wstring(closed) + L"."
        : L"Нечего закрывать (закреплённые вкладки пропущены).";
    return g_tabs[g_activeTab].view;
}

// Rename the active tab (empty label restores the automatic title).
void UiSetTabLabel(const std::wstring& label) {
    EnsureTabs();
    TabState& tab = g_tabs[g_activeTab];
    tab.label = label;
    // Strip characters that would break the tabs.txt line format.
    for (wchar_t& c : tab.label)
        if (c == L'\t' || c == L'\r' || c == L'\n') c = L' ';
    SaveTabs();
    g_consoleStatus = tab.label.empty()
        ? L"Имя вкладки сброшено."
        : L"Вкладка переименована: " + tab.label;
}

void UiToggleTabPin() {
    EnsureTabs();
    TabState& tab = g_tabs[g_activeTab];
    tab.pinned = !tab.pinned;
    SaveTabs();
    g_consoleStatus = tab.pinned
        ? L"Вкладка закреплена."
        : L"Вкладка откреплена.";
}

// Move the active tab one slot left (-1) or right (+1).
TabViewState UiMoveTab(int delta, const TabViewState& view) {
    EnsureTabs();
    if (g_tabs.size() < 2) {
        g_consoleStatus = L"Перемещать некуда — вкладка одна.";
        return view;
    }
    const long long target =
        static_cast<long long>(g_activeTab) + delta;
    if (target < 0 || target >= static_cast<long long>(g_tabs.size())) {
        g_consoleStatus = L"Вкладка уже на краю.";
        return view;
    }
    UiSaveViewState(view);
    std::swap(g_tabs[g_activeTab],
              g_tabs[static_cast<size_t>(target)]);
    g_activeTab = static_cast<size_t>(target);
    LoadTabToGlobals(g_tabs[g_activeTab]);
    SaveTabs();
    g_consoleStatus = L"Вкладка перемещена.";
    return g_tabs[g_activeTab].view;
}

TabViewState UiReopenClosedTab(const TabViewState& view) {
    EnsureTabs();
    if (g_closedTabs.empty()) {
        g_consoleStatus = L"Нет закрытых вкладок для восстановления.";
        return view;
    }
    if (g_tabs.size() >= MAX_TABS) {
        g_consoleStatus = L"Слишком много вкладок (максимум " +
            std::to_wstring(MAX_TABS) + L").";
        return view;
    }
    TabState tab = std::move(g_closedTabs.back());
    g_closedTabs.pop_back();
    UiSaveViewState(view);
    g_tabs.push_back(std::move(tab));
    g_activeTab = g_tabs.size() - 1;
    LoadTabToGlobals(g_tabs[g_activeTab]);
    MaybeStartPendingLoad();
    g_consoleStatus = L"Вкладка восстановлена.";
    return g_tabs[g_activeTab].view;
}

size_t UiTabCount() {
    EnsureTabs();
    return g_tabs.size();
}

bool UiTabPinned(size_t index) {
    EnsureTabs();
    return index < g_tabs.size() && g_tabs[index].pinned;
}

size_t UiActiveTabIndex() {
    EnsureTabs();
    return g_activeTab;
}

std::wstring UiTabTitle(size_t index) {
    EnsureTabs();
    if (index >= g_tabs.size())
        return L"";
    const TabState& tab = g_tabs[index];
    if (!tab.label.empty())
        return tab.label;
    if (!tab.pageOpen)
        return L"Новая вкладка";
    if (!tab.title.empty())
        return ToWide(tab.title);
    if (!tab.url.empty()) {
        // Show host + short path instead of the full URL when there is no title.
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        parts.dwHostNameLength = static_cast<DWORD>(-1);
        parts.dwUrlPathLength = static_cast<DWORD>(-1);
        parts.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (WinHttpCrackUrl(tab.url.c_str(), 0, 0, &parts) &&
            parts.lpszHostName) {
            std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
            std::wstring path = parts.dwUrlPathLength
                ? std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength)
                : L"/";
            if (path.size() > 18) path = path.substr(0, 15) + L"...";
            return host + path;
        }
        return tab.url;
    }
    return L"Вкладка";
}

bool UiNavCanBack() { return TabNavCanBack(); }
bool UiNavCanForward() { return TabNavCanForward(); }

void UiSaveViewState(const TabViewState& view) {
    EnsureTabs();
    SnapshotActiveTab();
    g_tabs[g_activeTab].view = view;
    SaveTabs();
}

// ---------------------------------------------------------------------------
// Tab session persistence (data/tabs.txt)
// ---------------------------------------------------------------------------

void SaveTabs() {
    EnsureTabs();
    std::ofstream file(TabsFilePath(), std::ios::trunc);
    if (!file) return;
    for (const TabState& tab : g_tabs) {
        if (!tab.pageOpen || tab.url.empty())
            continue;
        // Internal pages restorable through the load worker:
        // downloads, help, stats, about, notes, cache, ip, define/, tr/
        // (history/bookmarks build synchronously and stay session-only).
        if (IsInternalPageUrl(tab.url)) {
            static const wchar_t* kOk[] = {
                L"downloads", L"help", L"stats",
                L"about", L"notes", L"cache", L"ip", L"define/", L"tr/" };
            bool restorable = false;
            for (const wchar_t* k : kOk)
                if (tab.url.find(k) != std::wstring::npos) {
                    restorable = true; break;
                }
            if (!restorable) continue;
        }
        file << ToUtf8(tab.url);
        file << '\t' << tab.view.scrollOffset;
        if (tab.pinned || !tab.label.empty()) {
            file << '\t' << (tab.pinned ? 'p' : '-');
            if (!tab.label.empty()) file << '\t' << ToUtf8(tab.label);
        }
        file << '\n';
    }
}

void RestoreTabs() {
    // Lines: "<url>[\t<scroll>[\t<p|-> [\t<label>]]]".
    struct PendingTab { std::wstring url; size_t scroll; bool pinned; std::wstring label; };
    std::vector<PendingTab> entries;
    {
        std::ifstream in(TabsFilePath());
        std::string line;
        while (std::getline(in, line)) {
            std::vector<std::string> f;
            size_t pos = 0;
            while (true) {
                const size_t t = line.find('\t', pos);
                f.push_back(t == std::string::npos
                    ? line.substr(pos) : line.substr(pos, t - pos));
                if (t == std::string::npos) break;
                pos = t + 1;
            }
            if (f.empty() || f[0].empty()) continue;
            PendingTab e;
            e.url = ToWide(f[0]);
            try { e.scroll = f.size() > 1 ? std::stoull(f[1]) : 0; }
            catch (...) { e.scroll = 0; }
            e.pinned = f.size() > 2 && f[2] == "p";
            e.label = f.size() > 3 ? ToWide(f[3]) : L"";
            entries.push_back(std::move(e));
        }
    }
    if (entries.empty())
        return; // first run: keep the single fresh tab

    g_tabs.clear();
    for (auto& e : entries) {
        TabState tab;
        tab.url = e.url;
        tab.view.scrollOffset = e.scroll;
        tab.pinned = e.pinned;
        tab.label = e.label;
        tab.pageOpen = true;
        tab.pendingLoad = true; // loaded lazily on activation
        g_tabs.push_back(std::move(tab));
    }
    if (g_tabs.empty()) {
        g_tabs.emplace_back();
        return;
    }
    g_activeTab = 0;
    LoadTabToGlobals(g_tabs[0]);
    // Kick off the visible tab immediately; the rest load on switch.
    MaybeStartPendingLoad();
}


// ---------------------------------------------------------------------------
// Save page (Ctrl+S)
// ---------------------------------------------------------------------------

std::wstring UiSavePage() {
    if (g_currentUrl.empty() || IsInternalPageUrl(g_currentUrl)) {
        g_consoleStatus = L"Нечего сохранять — сначала откройте страницу.";
        return {};
    }

    // Filename: sanitized page title, falling back to host+hash.
    std::wstring base;
    for (wchar_t ch : ToWide(g_currentTitle)) {
        if (std::iswalnum(ch) || ch == L'-' || ch == L'_')
            base.push_back(ch);
        else if (ch == L' ' || ch == L'.')
            base.push_back(L'_');
        if (base.size() >= 80) break;
    }
    if (base.empty())
        base = L"page-" + ToWide(HashUrl(g_currentUrl));

    const bool haveHtml = !g_currentHtml.empty();
    const wchar_t* ext = haveHtml ? L".html" : L".txt";

    const fs::path savedDir = SavedDirPath();
    std::error_code ec;
    fs::path outPath;
    for (int i = 0; i < 100; ++i) {
        fs::path candidate = savedDir / base;
        if (i > 0) candidate += L"-" + std::to_wstring(i);
        candidate += ext;
        if (!fs::exists(candidate, ec)) {
            outPath = candidate;
            break;
        }
    }
    if (outPath.empty()) {
        g_consoleStatus = L"Не удалось подобрать имя файла.";
        return {};
    }

    FILE* file = nullptr;
    if (_wfopen_s(&file, outPath.c_str(), L"wb") != 0 || !file) {
        g_consoleStatus = L"Не удалось записать файл.";
        return {};
    }
    if (haveHtml) {
        std::fwrite(g_currentHtml.data(), 1, g_currentHtml.size(), file);
    } else {
        const std::string out = "TITLE: " + g_currentTitle +
            "\nURL: " + ToUtf8(g_currentUrl) + "\n\n" +
            StripDisplayMarkers(g_currentText);
        std::fwrite(out.data(), 1, out.size(), file);
    }
    std::fclose(file);

    g_consoleStatus = L"Сохранено: " + outPath.wstring();
    return outPath.wstring();
}

// Save the readable text view as Markdown (.md) — good for notes/reading.
std::wstring UiSavePageMarkdown() {
    if (g_currentUrl.empty() || IsInternalPageUrl(g_currentUrl)) {
        g_consoleStatus = L"Нечего сохранять — сначала откройте страницу.";
        return {};
    }
    std::wstring base;
    for (wchar_t ch : ToWide(g_currentTitle)) {
        if (std::iswalnum(ch) || ch == L'-' || ch == L'_')
            base.push_back(ch);
        else if (ch == L' ' || ch == L'.')
            base.push_back(L'_');
        if (base.size() >= 80) break;
    }
    if (base.empty())
        base = L"page-" + ToWide(HashUrl(g_currentUrl));

    const fs::path savedDir = SavedDirPath();
    std::error_code ec;
    fs::path outPath;
    for (int i = 0; i < 100; ++i) {
        fs::path candidate = savedDir / base;
        if (i > 0) candidate += L"-" + std::to_wstring(i);
        candidate += L".md";
        if (!fs::exists(candidate, ec)) {
            outPath = candidate;
            break;
        }
    }
    if (outPath.empty()) {
        g_consoleStatus = L"Не удалось подобрать имя файла.";
        return {};
    }
    const std::string md = "# " + g_currentTitle +
        "\n\n<" + ToUtf8(g_currentUrl) + ">\n\n" +
        StripDisplayMarkers(g_currentText) + "\n";
    FILE* file = nullptr;
    if (_wfopen_s(&file, outPath.c_str(), L"wb") != 0 || !file) {
        g_consoleStatus = L"Не удалось записать файл.";
        return {};
    }
    std::fwrite(md.data(), 1, md.size(), file);
    std::fclose(file);
    g_consoleStatus = L"Сохранено (Markdown): " + outPath.wstring();
    return outPath.wstring();
}

// ---------------------------------------------------------------------------
// Form submission
// ---------------------------------------------------------------------------

// `submitField` is the index (into g_currentFields) of the button that was
// activated; -1 means "no button" (Ctrl+Enter on a plain field). Only that
// submit button contributes its name/value, per HTML form semantics.
static std::string BuildFormBody(const FormDef& form, int submitField) {
    std::string body;
    bool first = true;
    auto appendPair = [&](const std::string& name, const std::string& value) {
        if (name.empty()) return;
        if (!first) body += '&';
        first = false;
        body += UrlEncode(name);
        body += '=';
        body += UrlEncode(value);
    };

    for (int fi : form.fields) {
        if (fi < 0 || static_cast<size_t>(fi) >= g_currentFields.size())
            continue;
        const FormField& field = g_currentFields[static_cast<size_t>(fi)];
        switch (field.kind) {
        case FieldKind::Text:
        case FieldKind::Password:
        case FieldKind::Hidden:
        case FieldKind::Textarea:
            appendPair(field.name, field.value);
            break;
        case FieldKind::Checkbox:
        case FieldKind::Radio:
            if (field.checked)
                appendPair(field.name, field.value.empty() ? "on" : field.value);
            break;
        case FieldKind::Select:
            if (field.selected < field.options.size()) {
                const std::string& submitted =
                    field.selected < field.optionValues.size()
                        ? field.optionValues[field.selected]
                        : field.options[field.selected];
                appendPair(field.name, submitted);
            }
            break;
        case FieldKind::Submit:
            // Only the activated submit button is successful.
            if (fi == submitField)
                appendPair(field.name, field.value);
            break;
        }
    }
    return body;
}

// Send the assembled body to the form's action via GET (query string) or
// POST (urlencoded body, never cached).
static void DispatchForm(const FormDef& form, const std::string& body) {
    const std::wstring action = form.action.empty()
        ? g_currentUrl : form.action;

    if (form.method == "post") {
        EnsureTabs();
        LoadJob job;
        job.kind = LoadJob::Kind::Url;
        job.url = action;
        job.mode = CacheMode::Bypass; // POST responses are never cached
        job.addToNavigation = true;
        job.recordVisit = true;
        job.tabIndex = g_activeTab;
        job.postBody = body;
        QueueOrStart(job);
        g_consoleStatus = L"Отправка формы (POST)…";
        return;
    }

    // GET: merge pairs into the query string.
    std::wstring url = action;
    if (!body.empty()) {
        url += (url.find(L'?') == std::wstring::npos) ? L'?' : L'&';
        url += ToWide(body);
    }
    UiOpenUrl(url);
}

void UiSubmitForm(int fieldIndex) {
    if (fieldIndex < 0 ||
        static_cast<size_t>(fieldIndex) >= g_currentFields.size()) {
        g_consoleStatus = L"Поле формы не найдено.";
        return;
    }
    const FormField& field = g_currentFields[static_cast<size_t>(fieldIndex)];
    if (field.formIndex < 0 ||
        static_cast<size_t>(field.formIndex) >= g_currentForms.size()) {
        g_consoleStatus = L"У поля нет associated-формы.";
        return;
    }
    const FormDef& form = g_currentForms[static_cast<size_t>(field.formIndex)];

    switch (field.kind) {
    case FieldKind::Submit:
        break; // fall through to submission below
    case FieldKind::Select:
        if (!field.options.empty()) {
            FormField& mutableField =
                g_currentFields[static_cast<size_t>(fieldIndex)];
            mutableField.selected =
                (mutableField.selected + 1) % mutableField.options.size();
            g_consoleStatus = ToWide(mutableField.options[mutableField.selected]);
        }
        return;
    case FieldKind::Checkbox:
    case FieldKind::Radio: {
        FormField& mutableField =
            g_currentFields[static_cast<size_t>(fieldIndex)];
        mutableField.checked = !mutableField.checked;
        if (mutableField.checked && field.kind == FieldKind::Radio) {
            // Radio groups share a name within the form: uncheck siblings.
            for (int fi : form.fields) {
                if (fi == fieldIndex || fi < 0 ||
                    static_cast<size_t>(fi) >= g_currentFields.size())
                    continue;
                FormField& sibling = g_currentFields[static_cast<size_t>(fi)];
                if (sibling.kind == FieldKind::Radio &&
                    sibling.name == field.name &&
                    sibling.formIndex == field.formIndex)
                    sibling.checked = false;
            }
        }
        g_consoleStatus = mutableField.checked ? L"Отмечено." : L"Снято.";
        return;
    }
    case FieldKind::Hidden:
        g_consoleStatus = L"Скрытое поле — редактирование не требуется.";
        return;
    case FieldKind::Text:
    case FieldKind::Password:
    case FieldKind::Textarea:
        // Editing is handled by the console UI (editingField state).
        return;
    }

    // Only submit buttons reach this point.
    DispatchForm(form, BuildFormBody(form, fieldIndex));
}

void UiSubmitFormForField(int fieldIndex) {
    if (fieldIndex < 0 ||
        static_cast<size_t>(fieldIndex) >= g_currentFields.size()) {
        g_consoleStatus = L"Поле формы не найдено.";
        return;
    }
    const FormField& field = g_currentFields[static_cast<size_t>(fieldIndex)];
    if (field.formIndex < 0 ||
        static_cast<size_t>(field.formIndex) >= g_currentForms.size()) {
        g_consoleStatus = L"У поля нет associated-формы.";
        return;
    }
    const FormDef& form = g_currentForms[static_cast<size_t>(field.formIndex)];
    DispatchForm(form, BuildFormBody(form, -1));
}

bool PollAsyncLoad() {
    std::optional<LoadResult> finished;
    {
        std::lock_guard<std::mutex> lock(g_loadMutex);
        if (g_finishedLoad) {
            finished = std::move(*g_finishedLoad);
            g_finishedLoad.reset();
        }
    }

    bool changed = false;
    if (finished) {
        ApplyResult(*finished);
        changed = true;
        // Persist the tab set (URLs may have changed) for next startup.
        SnapshotActiveTab();
        SaveTabs();

        std::optional<LoadJob> queued;
        {
            std::lock_guard<std::mutex> lock(g_loadMutex);
            if (!g_queuedJobs.empty()) {
                queued = std::move(g_queuedJobs.front());
                g_queuedJobs.pop_front();
            }
        }
        if (queued) {
            StartJobNow(*queued);
            g_consoleStatus = L"Загрузка…";
        }
    }
    return changed;
}

void ShutdownAsyncLoads() {
    g_loadCancel.store(true);
    if (g_loadThread.joinable())
        g_loadThread.join();
}



// ---------------------------------------------------------------------------
// Feature pack: offline mode, background-tab search, misc helpers
// ---------------------------------------------------------------------------

// User-controlled offline mode: requests are answered from cache only.
bool UiOfflineEnabled() { return g_userOffline; }

void UiSetOffline(bool on) {
    g_userOffline = on;
    UiSetSetting(L"offline", on ? L"1" : L"");
}

void UiApplyOfflineSetting() {
    g_userOffline = UiGetSetting(L"offline") == L"1";
}

// Search query opened in a new background tab (omnibox Ctrl+Enter).
TabViewState UiSearchInBackgroundTab(const std::string& query,
                                    const TabViewState& view) {
    if (query.empty()) {
        g_consoleStatus = L"Пустой поисковый запрос.";
        return view;
    }
    EnsureTabs();
    if (g_tabs.size() >= MAX_TABS) {
        g_consoleStatus = L"Слишком много вкладок (максимум " +
            std::to_wstring(MAX_TABS) + L").";
        return view;
    }
    g_tabs.emplace_back();
    const size_t newIndex = g_tabs.size() - 1;
    LoadJob job;
    job.kind = LoadJob::Kind::Search;
    job.query = query;
    job.addToNavigation = true;
    job.recordVisit = true;
    job.tabIndex = newIndex;
    QueueOrStart(job);
    g_consoleStatus = L"Поиск во вкладке " +
        std::to_wstring(newIndex + 1) + L".";
    return view;
}

// Reload bypassing the cache entirely (hard reload).
void UiHardReload() {
    if (g_currentUrl.empty()) {
        g_consoleStatus = L"Нет страницы для обновления.";
        return;
    }
    if (g_currentUrl.rfind(L"http", 0) != 0) {
        UiReload(); // internal/custom pages: normal reload path
        return;
    }
    EnsureTabs();
    LoadJob job;
    job.kind = LoadJob::Kind::Url;
    job.url = g_currentUrl;
    job.mode = CacheMode::Bypass;
    job.addToNavigation = false;
    job.recordVisit = false;
    job.tabIndex = g_activeTab;
    QueueOrStart(job);
    g_consoleStatus = L"Жёсткое обновление (мимо кэша)…";
}

// View-source of an arbitrary URL (synchronous — one fetch, UI waits).
void UiViewSourceUrl(const std::wstring& rawUrl) {
    const std::wstring url = NormalizeUrl(rawUrl);
    if (url.empty() || url.rfind(L"http", 0) != 0) {
        g_consoleStatus = L"Укажите http(s)-адрес: source <url>.";
        return;
    }
    g_consoleStatus = L"Загрузка исходного кода…";
    FetchOutcome o = FetchPage(url, CacheMode::Use, nullptr);
    if (!o.ok) {
        g_consoleStatus = o.error.empty()
            ? L"Не удалось загрузить страницу." : o.error;
        return;
    }
    const std::string src = o.html.empty() ? o.text : o.html;
    CommitCustomPage(L"view-source://" + o.finalUrl,
                     "View-source: " + o.title, src, {});
    g_currentHtml = src;
    g_consoleStatus = L"Исходный код: " + o.finalUrl;
}

// Dedicated images page — every page image as a clickable link.
void UiImagesPage() {
    if (g_currentImages.empty()) {
        g_consoleStatus = L"На странице нет изображений.";
        return;
    }
    std::string text;
    std::vector<Link> links;
    text += "Изображения страницы — " +
            std::to_string(g_currentImages.size()) + " шт.\n";
    text += " Enter/click — открыть, d — скачать.\n\n";
    for (size_t i = 0; i < g_currentImages.size(); ++i) {
        const PageImage& img = g_currentImages[i];
        std::wstring label = ToWide(img.alt);
        if (label.empty()) {
            const size_t slash = img.url.find_last_of(L"/");
            label = slash == std::wstring::npos
                ? img.url : img.url.substr(slash + 1);
            if (label.empty()) label = img.url;
        }
        text += std::to_string(i + 1) + ". " + ToUtf8(label) + "\n   " +
                ToUtf8(img.url) + "\n";
        links.push_back({ToUtf8(label), img.url});
    }
    CommitCustomPage(L"images://page", "Изображения страницы",
                     NormalizeDisplayText(text), std::move(links));
    g_consoleStatus = L"Изображений на странице: " +
        std::to_wstring(g_currentImages.size()) + L".";
}

// Dedicated links page — the full link list (the `links` command only
// copies URLs to the clipboard).
void UiLinksPage() {
    if (g_currentLinks.empty()) {
        g_consoleStatus = L"На странице нет ссылок.";
        return;
    }
    std::string text;
    std::vector<Link> links;
    text += "Ссылки страницы — " +
            std::to_string(g_currentLinks.size()) + " шт.\n\n";
    for (size_t i = 0; i < g_currentLinks.size(); ++i) {
        const Link& l = g_currentLinks[i];
        text += std::to_string(i + 1) + ". " + l.label + "\n   " +
                ToUtf8(l.url) + "\n";
        links.push_back({std::to_string(i + 1) + ". " + l.label, l.url});
    }
    CommitCustomPage(L"links://page", "Ссылки страницы",
                     NormalizeDisplayText(text), std::move(links));
    g_consoleStatus = L"Ссылок на странице: " +
        std::to_wstring(g_currentLinks.size()) + L".";
}

// --- notes management -----------------------------------------------------

void UiNoteDelete(size_t index1) {
    std::wifstream in(NotesFilePath());
    std::vector<std::wstring> lines;
    std::wstring line;
    while (std::getline(in, line)) lines.push_back(line);
    if (lines.empty()) {
        g_consoleStatus = L"Заметок нет.";
        return;
    }
    if (index1 < 1 || index1 > lines.size()) {
        g_consoleStatus = L"Нет заметки №" + std::to_wstring(index1) +
            L" (всего " + std::to_wstring(lines.size()) + L").";
        return;
    }
    lines.erase(lines.begin() + static_cast<long>(index1 - 1));
    std::wofstream out(NotesFilePath(), std::ios::trunc);
    for (const auto& l : lines) out << l << L'\n';
    g_consoleStatus = L"Заметка удалена (осталось " +
        std::to_wstring(lines.size()) + L").";
}

void UiNotesClear() {
    std::wofstream out(NotesFilePath(), std::ios::trunc);
    g_consoleStatus = L"Все заметки удалены.";
}

// --- lucky ----------------------------------------------------------------

void UiLucky() {
    // Half the time: random Wikipedia article; otherwise a random bookmark.
    const auto bookmarks = LoadBookmarks();
    if (!bookmarks.empty() && (std::rand() % 2 == 0)) {
        const auto& url =
            bookmarks[static_cast<size_t>(std::rand()) % bookmarks.size()];
        UiOpenUrl(url);
        g_consoleStatus = L"Мне повезёт! (закладка)";
    } else {
        UiOpenUrl(L"https://ru.wikipedia.org/wiki/Special:Random");
        g_consoleStatus = L"Мне повезёт! (Википедия)";
    }
}

// --- sessions ---------------------------------------------------------------

// List saved/session-*.txt files as a clickable internal page.
void UiSessionsPage() {
    std::vector<fs::path> files;
    std::error_code ec;
    const fs::path dir = SavedDirPath();
    if (fs::exists(dir, ec)) {
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            const std::wstring n = e.path().filename().wstring();
            if (n.rfind(L"session-", 0) == 0 &&
                e.path().extension() == L".txt")
                files.push_back(e.path());
        }
    }
    std::sort(files.begin(), files.end());
    std::string text;
    std::vector<Link> links;
    text += "Сохранённые сессии — " + std::to_string(files.size()) +
            " шт.\n";
    text += " restore <имя> — открыть вкладки из файла.\n\n";
    for (size_t i = 0; i < files.size(); ++i) {
        const std::wstring name = files[i].filename().wstring();
        uint64_t sz = 0;
        {
            std::error_code ec2;
            sz = fs::file_size(files[i], ec2);
        }
        text += std::to_string(i + 1) + ". " + ToUtf8(name) +
                "  —  " + std::to_string(sz) + " байт\n";
        links.push_back({std::to_string(i + 1) + ". " + ToUtf8(name),
                         L"restore://" + name});
    }
    if (files.empty())
        text += "Сессий нет — выполните session для экспорта.\n";
    CommitCustomPage(L"sessions://page", "Сессии", text, std::move(links));
    g_consoleStatus = L"Сохранённых сессий: " +
        std::to_wstring(files.size()) + L".";
}

// Restore tabs from a saved/session-*.txt export ([TABS] section).
void UiRestoreSession(const std::wstring& name) {
    std::wstring file = name;
    if (file.rfind(L"session-", 0) != 0) file = L"session-" + file;
    if (file.size() < 4 || file.substr(file.size() - 4) != L".txt")
        file += L".txt";
    const fs::path path = SavedDirPath() / file;
    std::wifstream in(path);
    if (!in) {
        g_consoleStatus = L"Файл сессии не найден: " + file;
        return;
    }
    std::vector<std::wstring> urls;
    bool inTabs = false;
    std::wstring line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.front() == L'[') {
            inTabs = line == L"[TABS]";
            continue;
        }
        if (inTabs && !line.empty() && line.front() != L'#')
            urls.push_back(line);
    }
    if (urls.empty()) {
        g_consoleStatus = L"В сессии нет сохранённых вкладок.";
        return;
    }
    // First URL replaces the current tab, the rest open in background.
    UiOpenUrl(urls.front());
    for (size_t i = 1; i < urls.size(); ++i)
        UiOpenInBackgroundTab(urls[i], TabViewState{});
    g_consoleStatus = L"Восстановлено вкладок: " +
        std::to_wstring(urls.size()) + L" (" + file + L").";
}

// --- display toggles (images section, status bar, prefetch) ---------------

bool UiImagesEnabled() { return g_showImages; }
void UiSetImagesEnabled(bool on) {
    g_showImages = on;
    UiSetSetting(L"images", on ? L"" : L"0");
}
void UiApplyImagesSetting() { g_showImages = UiGetSetting(L"images") != L"0"; }

bool UiStatusBarOn() { return g_statusBar; }
void UiSetStatusBar(bool on) {
    g_statusBar = on;
    UiSetSetting(L"statusbar", on ? L"" : L"0");
}
void UiApplyStatusBarSetting() { g_statusBar = UiGetSetting(L"statusbar") != L"0"; }

bool UiPrefetchEnabled() { return g_prefetch; }
void UiSetPrefetch(bool on) {
    g_prefetch = on;
    UiSetSetting(L"prefetch", on ? L"" : L"0");
}
void UiApplyPrefetchSetting() { g_prefetch = UiGetSetting(L"prefetch") != L"0"; }

// --- text zoom (wrap width) ------------------------------------------------

int UiZoomPercent() { return g_zoomPercent; }

void UiSetZoomPercent(int percent) {
    g_zoomPercent = std::clamp(percent, 50, 200);
    UiSetSetting(L"zoom", g_zoomPercent == 100
        ? L"" : std::to_wstring(g_zoomPercent));
}

void UiApplyZoomSetting() {
    const std::wstring v = UiGetSetting(L"zoom");
    if (v.empty()) { g_zoomPercent = 100; return; }
    try { UiSetZoomPercent(std::stoi(ToUtf8(v))); }
    catch (...) { g_zoomPercent = 100; }
}

// --- hover prefetch / tab busy / misc feature-pack helpers -----------------

void UiPrefetchUrl(const std::wstring& url) {
    if (!g_prefetch) return;
    const std::wstring lu = LowerWide(url);
    if (lu.rfind(L"http://", 0) != 0 && lu.rfind(L"https://", 0) != 0)
        return;
    // Dedup: each URL is prefetched at most once per session.
    static std::unordered_set<std::wstring> done;
    static std::mutex mu;
    {
        std::lock_guard<std::mutex> lock(mu);
        if (!done.insert(url).second) return;
        if (done.size() > 1000) done.clear();
    }
    std::thread([url] {
        try { Request(url, CacheMode::Use, nullptr); }
        catch (...) {}
    }).detach();
}

bool UiTabBusy(size_t index) {
    if (g_loadingTabIdx.load() == static_cast<long>(index)) return true;
    std::lock_guard<std::mutex> lock(g_loadMutex);
    for (const auto& j : g_queuedJobs)
        if (j.tabIndex == index) return true;
    return false;
}

std::wstring UiTabUrl(size_t index) {
    EnsureTabs();
    return index < g_tabs.size() ? g_tabs[index].url : std::wstring();
}
