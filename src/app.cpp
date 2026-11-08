#include "browser.h"

#include <array>
#include <cstdio>
#include <ctime>
#include <cwchar>
#include <deque>
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
std::wstring g_consoleStatus;
std::vector<Link> g_currentLinks;
std::vector<PageImage> g_currentImages;
std::vector<FormField> g_currentFields;
std::vector<FormDef> g_currentForms;
std::string g_currentHtml;
std::atomic_uint64_t g_loadDownloaded{0};
std::vector<std::wstring> g_navigation;
size_t g_navigationIndex = 0;
bool g_hasNavigation = false;

bool g_useAnsi = false;

std::atomic_bool g_loadRunning{false};
std::atomic_bool g_loadCancel{false};
std::mutex g_loadMutex;


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

    for (const auto& provider : kProviders) {
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
        << "  " << ui::color(ui::cyan, "wiki ...") << "         Offline Wikipedia library (ru/en)\n"
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
std::optional<LoadJob> g_queuedJob;
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
        } else {
            result.ok = false;
            result.statusText = L"Неизвестная внутренняя страница.";
        }
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
    }
}

void StartJobNow(const LoadJob& job) {
    if (g_loadThread.joinable())
        g_loadThread.join();

    g_loadCancel.store(false);
    g_loadDownloaded.store(0);
    g_loadRunning.store(true);
    g_loadThread = std::thread(LoadWorker, job);
}

void QueueOrStart(const LoadJob& job) {
    if (g_loadRunning.load()) {
        {
            std::lock_guard<std::mutex> lock(g_loadMutex);
            g_queuedJob = job;
        }
        g_consoleStatus = L"Загрузка… (текущий запрос продолжится, новый — в очереди)";
        return;
    }
    StartJobNow(job);
    g_consoleStatus = L"Загрузка…";
}

void ApplyResult(const LoadResult& result) {
    const LoadJob& job = result.job;
    if (!result.ok) {
        g_consoleStatus = result.statusText;
        return;
    }

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
    g_consolePageOpen = true;
}

} // namespace

// ---------------------------------------------------------------------------
// Async navigation API used by the console UI
// ---------------------------------------------------------------------------

void UiOpenUrl(const std::wstring& rawUrl, CacheMode mode) {
    // Local Wikipedia library pages render inside the console UI.
    if (LowerWide(rawUrl).rfind(L"wiki://", 0) == 0) {
        WikiOpenConsole(rawUrl);
        return;
    }
    const std::wstring url = NormalizeUrl(rawUrl);
    if (url.empty()) {
        g_consoleStatus = L"Некорректный адрес сайта.";
        return;
    }
    EnsureTabs();
    LoadJob job;
    job.kind = LoadJob::Kind::Url;
    job.url = url;
    job.mode = mode;
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
    LoadJob job;
    job.kind = LoadJob::Kind::Url;
    job.url = tab.nav[static_cast<size_t>(target)];
    job.mode = CacheMode::Use;
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
    LoadJob job;
    job.kind = LoadJob::Kind::Url;
    job.url = tab.nav[static_cast<size_t>(target)];
    job.mode = CacheMode::Use;
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




// Opens an internal page (downloads/history/bookmarks) through the normal
// navigation pipeline, so it lives in a real tab with back/forward.
void UiOpenInternalPage(const std::wstring& kind) {
    LoadJob job;
    job.kind = LoadJob::Kind::Internal;
    job.url = std::wstring(kInternalPrefix) + kind;
    job.mode = CacheMode::Bypass;
    job.addToNavigation = true;
    job.recordVisit = false; // internal pages don't belong in history
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
    job.kind = IsInternalPageUrl(tab.url) ? LoadJob::Kind::Internal
                                        : LoadJob::Kind::Url;
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
    UiSaveViewState(view);
    g_tabs.emplace_back();
    g_activeTab = g_tabs.size() - 1;
    LoadTabToGlobals(g_tabs[g_activeTab]);

    LoadJob job;
    job.kind = LoadJob::Kind::Url;
    job.url = url;
    job.mode = CacheMode::Use;
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

size_t UiActiveTabIndex() {
    EnsureTabs();
    return g_activeTab;
}

std::wstring UiTabTitle(size_t index) {
    EnsureTabs();
    if (index >= g_tabs.size())
        return L"";
    const TabState& tab = g_tabs[index];
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
        // Internal pages: only downloads is restorable (worker supports it).
        if (IsInternalPageUrl(tab.url) &&
            tab.url.find(L"downloads") == std::wstring::npos)
            continue;
        file << ToUtf8(tab.url);
        if (tab.view.scrollOffset)
            file << '\t' << tab.view.scrollOffset;
        file << '\n';
    }
}

void RestoreTabs() {
    // Lines: "<url>" or "<url>\t<scrollOffset>".
    std::vector<std::pair<std::wstring, size_t>> entries;
    {
        std::ifstream in(TabsFilePath());
        std::string line;
        while (std::getline(in, line)) {
            const size_t tab = line.find('\t');
            std::wstring url = ToWide(
                tab == std::string::npos ? line : line.substr(0, tab));
            if (url.empty()) continue;
            size_t scroll = 0;
            if (tab != std::string::npos) {
                try { scroll = std::stoull(line.substr(tab + 1)); }
                catch (...) { scroll = 0; }
            }
            entries.emplace_back(std::move(url), scroll);
        }
    }
    if (entries.empty())
        return; // first run: keep the single fresh tab

    g_tabs.clear();
    for (auto& [url, scroll] : entries) {
        TabState tab;
        tab.url = url;
        tab.view.scrollOffset = scroll;
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
            if (g_queuedJob) {
                queued = std::move(*g_queuedJob);
                g_queuedJob.reset();
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

