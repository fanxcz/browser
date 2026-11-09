#include "browser.h"

#include <array>
#include <cwchar>
#include <unordered_map>

// ---------------------------------------------------------------------------
// Cache storage
// ---------------------------------------------------------------------------

std::string CacheBodyFile(const std::wstring& url) {
    return (CacheDirPath() / (HashUrl(url) + ".body")).string();
}

std::string CacheMetaFile(const std::wstring& url) {
    return (CacheDirPath() / (HashUrl(url) + ".meta")).string();
}

bool LoadCache(const std::wstring& url,
               Response& response,
               CacheMeta& meta) {
    std::lock_guard<std::mutex> lock(g_cacheMutex);

    std::ifstream metaFile(CacheMetaFile(url), std::ios::binary);
    std::ifstream bodyFile(CacheBodyFile(url), std::ios::binary);
    if (!metaFile || !bodyFile)
        return false;

    std::string etagUtf8;
    std::string modifiedUtf8;
    std::string contentTypeUtf8;
    if (!(metaFile >> meta.savedAt >> meta.maxAge
                   >> std::quoted(etagUtf8)
                   >> std::quoted(modifiedUtf8)))
        return false;

    // The last field was added in Browser 2.1.2. Older cache entries remain readable.
    metaFile >> std::quoted(contentTypeUtf8);
    meta.etag = ToWide(etagUtf8);
    meta.lastModified = ToWide(modifiedUtf8);
    meta.contentType = ToWide(contentTypeUtf8);

    response.body.assign(std::istreambuf_iterator<char>(bodyFile),
                         std::istreambuf_iterator<char>());
    response.status = 200;
    response.ok = true;
    response.fromCache = true;
    response.contentType = meta.contentType;
    return true;
}

void SaveCache(const std::wstring& url,
               const Response& response,
               const CacheMeta& meta) {
    if (!response.ok || response.status != 200)
        return;

    std::lock_guard<std::mutex> lock(g_cacheMutex);

    const std::string bodyPath = CacheBodyFile(url);
    const std::string metaPath = CacheMetaFile(url);
    const std::string bodyTemp = bodyPath + ".tmp";
    const std::string metaTemp = metaPath + ".tmp";

    {
        std::ofstream body(bodyTemp, std::ios::binary | std::ios::trunc);
        if (!body) return;
        if (!response.body.empty()) {
            body.write(response.body.data(),
                       static_cast<std::streamsize>(response.body.size()));
        }
        if (!body) {
            fs::remove(bodyTemp);
            return;
        }
    }

    {
        std::ofstream metaFile(metaTemp, std::ios::binary | std::ios::trunc);
        if (!metaFile) {
            fs::remove(bodyTemp);
            return;
        }
        metaFile << meta.savedAt << ' ' << meta.maxAge << '\n'
                 << std::quoted(ToUtf8(meta.etag)) << '\n'
                 << std::quoted(ToUtf8(meta.lastModified)) << '\n'
                 << std::quoted(ToUtf8(meta.contentType)) << '\n';
        if (!metaFile) {
            fs::remove(bodyTemp);
            fs::remove(metaTemp);
            return;
        }
    }

    std::error_code ec;
    fs::remove(bodyPath, ec);
    fs::remove(metaPath, ec);
    fs::rename(bodyTemp, bodyPath, ec);
    if (ec) {
        fs::remove(bodyTemp);
        fs::remove(metaTemp);
        return;
    }
    fs::rename(metaTemp, metaPath, ec);
    if (ec) {
        fs::remove(metaTemp);
        fs::remove(bodyPath);
    }
}

void RemoveCacheEntry(const std::wstring& url) {
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    std::error_code ec;
    fs::remove(CacheBodyFile(url), ec);
    fs::remove(CacheMetaFile(url), ec);
}

uint64_t ReadMaxAge(const std::wstring& cacheControl) {
    const std::wstring lower = LowerWide(cacheControl);

    if (lower.find(L"no-store") != std::wstring::npos)
        return 0;
    if (lower.find(L"no-cache") != std::wstring::npos)
        return 0;

    const size_t marker = lower.find(L"max-age=");
    if (marker == std::wstring::npos)
        return 60; // Small heuristic TTL when the server gives no freshness lifetime.

    size_t pos = marker + 8;
    uint64_t value = 0;
    bool foundDigit = false;

    while (pos < lower.size() && lower[pos] >= L'0' && lower[pos] <= L'9') {
        foundDigit = true;
        const unsigned digit = static_cast<unsigned>(lower[pos] - L'0');
        if (value > (UINT64_MAX - digit) / 10)
            return 0;
        value = value * 10 + digit;
        ++pos;
    }

    return foundDigit ? std::min<uint64_t>(value, 30ULL * 24 * 60 * 60) : 60;
}

bool CacheIsFresh(const CacheMeta& meta) {
    return meta.maxAge > 0 && UnixNow() < meta.savedAt + meta.maxAge;
}

// ---------------------------------------------------------------------------
// WinHTTP plumbing
// ---------------------------------------------------------------------------

// Keep-alive connection pool: WinHTTP connections are keyed by
// scheme+host+port and reused across requests, avoiding a fresh TCP+TLS
// handshake for every page (big win when browsing one site).
namespace {
std::mutex g_poolMutex;
std::unordered_map<std::wstring, std::vector<HINTERNET>> g_connPool;
constexpr size_t MAX_POOLED_PER_HOST = 4;

std::wstring PoolKeyFor(const std::wstring& url) {
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || !parts.lpszHostName)
        return {};
    const std::wstring scheme = parts.nScheme == INTERNET_SCHEME_HTTP
                                    ? L"http" : L"https";
    return scheme + L"|" +
           std::wstring(parts.lpszHostName, parts.dwHostNameLength) +
           L"|" + std::to_wstring(parts.nPort);
}

HINTERNET BorrowConnection(const std::wstring& key) {
    if (key.empty()) return nullptr;
    std::lock_guard<std::mutex> lock(g_poolMutex);
    auto it = g_connPool.find(key);
    if (it == g_connPool.end() || it->second.empty()) return nullptr;
    HINTERNET handle = it->second.back();
    it->second.pop_back();
    return handle;
}

void ReturnConnection(const std::wstring& key, HINTERNET handle) {
    if (key.empty() || !handle) return;
    std::lock_guard<std::mutex> lock(g_poolMutex);
    auto& vec = g_connPool[key];
    if (vec.size() < MAX_POOLED_PER_HOST)
        vec.push_back(handle);
    else
        WinHttpCloseHandle(handle);
}
} // namespace

void CloseConnectionPool() {
    std::lock_guard<std::mutex> lock(g_poolMutex);
    for (auto& [key, vec] : g_connPool)
        for (HINTERNET handle : vec)
            WinHttpCloseHandle(handle);
    g_connPool.clear();
}

// WinHTTP error codes that mean "this connection is dead": drop it
// instead of returning it to the pool.
static bool IsDeadConnectionError(DWORD error) {
    switch (error) {
    case ERROR_WINHTTP_TIMEOUT:               // 12002
    case ERROR_WINHTTP_CANNOT_CONNECT:        // 12029
    case ERROR_WINHTTP_CONNECTION_ERROR:      // 12030
    case ERROR_WINHTTP_SECURE_FAILURE:        // 12045
        return true;
    default:
        return false;
    }
}

void CloseRequest(RequestHandles& handles) {
    if (handles.request) {
        WinHttpCloseHandle(handles.request);
        handles.request = nullptr;
    }
    if (handles.connection) {
        if (handles.pooledConnection && !handles.discardConnection)
            ReturnConnection(handles.poolKey, handles.connection);
        else
            WinHttpCloseHandle(handles.connection);
        handles.connection = nullptr;
    }
}

bool CreateRequest(const std::wstring& url, RequestHandles& handles,
                   const wchar_t* method, HINTERNET session) {
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts))
        return false;

    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path;
    if (parts.dwUrlPathLength)
        path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength)
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (path.empty())
        path = L"/";

    // Reuse a pooled keep-alive connection for this host when available.
    // A custom session (e.g. the no-decompression download session used for
    // Range resumes) bypasses the pool entirely: pooled handles belong to
    // g_session and its decompression flags don't apply there.
    const HINTERNET ses = session ? session : g_session;
    handles.poolKey = session ? std::wstring() : PoolKeyFor(url);
    handles.connection = handles.poolKey.empty()
                             ? nullptr
                             : BorrowConnection(handles.poolKey);
    handles.pooledConnection = handles.connection != nullptr;
    handles.borrowedConnection = handles.connection != nullptr;
    if (!handles.connection) {
        handles.connection = WinHttpConnect(ses, host.c_str(),
                                            parts.nPort, 0);
        if (!handles.connection) {
            handles.poolKey.clear();
            handles.pooledConnection = false;
            return false;
        }
        // Fresh connections on the main session are pooled too.
        handles.pooledConnection = !session;
    }

    const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS
                            ? WINHTTP_FLAG_SECURE : 0;

    handles.request = WinHttpOpenRequest(
        handles.connection, method, path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);

    if (!handles.request) {
        CloseRequest(handles);
        return false;
    }

    WinHttpAddRequestHeaders(
        handles.request,
        L"Accept: text/html,application/xhtml+xml,application/json,"
        L"text/plain,*/*\r\n"
        L"Accept-Language: ru-RU,ru;q=0.9,en-US;q=0.8,en;q=0.7\r\n",
        static_cast<DWORD>(-1),
        WINHTTP_ADDREQ_FLAG_ADD);

    // Disable WinHTTP's built-in redirect following: Request() implements
    // its own loop so the final URL, cache keys and cancel checks stay
    // under our control. The download worker follows redirects itself.
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(handles.request, WINHTTP_OPTION_REDIRECT_POLICY,
                     &redirectPolicy, sizeof(redirectPolicy));

    return true;
}

std::wstring QueryHeader(HINTERNET request, DWORD query) {
    DWORD bytes = 0;
    WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX,
                        nullptr, &bytes, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes < sizeof(wchar_t))
        return {};

    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    if (!WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX,
                             buffer.data(), &bytes, WINHTTP_NO_HEADER_INDEX))
        return {};

    return std::wstring(buffer.data());
}

std::wstring QueryHeaderValue(HINTERNET request, const wchar_t* name) {
    DWORD bytes = 0;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, name,
                        nullptr, &bytes, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes < sizeof(wchar_t))
        return {};

    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, name,
                             buffer.data(), &bytes, WINHTTP_NO_HEADER_INDEX))
        return {};

    return std::wstring(buffer.data());
}

static bool ReadResponseBody(HINTERNET request, std::vector<char>& body,
                             std::atomic_bool* cancel,
                             std::atomic_uint64_t* progress) {
    constexpr size_t MAX_BODY_BYTES = 64ULL * 1024 * 1024; // 64 MiB per page
    for (;;) {
        if (cancel && cancel->load())
            return false;

        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available))
            return false;
        if (available == 0)
            return true;

        const size_t oldSize = body.size();
        if (available > MAX_BODY_BYTES || body.size() + available > MAX_BODY_BYTES)
            return false;

        body.resize(oldSize + available);
        DWORD received = 0;

        if (!WinHttpReadData(request, body.data() + oldSize,
                             available, &received)) {
            body.resize(oldSize);
            return false;
        }

        body.resize(oldSize + received);
        if (progress && received > 0)
            progress->fetch_add(received, std::memory_order_relaxed);
        if (received == 0)
            return true;
    }
}

static bool IsRedirectStatus(DWORD status) {
    return status == HTTP_STATUS_MOVED ||        // 301
           status == 302 ||                      // Found
           status == 303 ||                      // See Other
           status == 307 ||                      // Temporary Redirect
           status == 308 ||                      // Permanent Redirect
           (status >= 300 && status < 400 && status != HTTP_STATUS_NOT_MODIFIED);
}

// Single request/response pair for one URL (no redirect following).
// `postBody` non-null switches the request to POST and disables caching —
// POST responses are dynamic and must never be served from the page cache.
static Response RequestOnce(const std::wstring& url, CacheMode mode,
                            std::atomic_bool* cancel,
                            std::wstring& locationOut,
                            const std::string* postBody,
                            std::atomic_uint64_t* progress) {
    Response response;
    CacheMeta cachedMeta;
    Response cachedResponse;
    const bool cacheable = postBody == nullptr;
    const bool haveCached = cacheable && mode != CacheMode::Bypass &&
                            LoadCache(url, cachedResponse, cachedMeta);

    if (haveCached && mode == CacheMode::Use && CacheIsFresh(cachedMeta)) {
        g_cacheHits.fetch_add(1);
        return cachedResponse;
    }

    // User-enabled offline mode: serve whatever is cached, never touch
    // the network. Stale cache beats no page.
    if (g_userOffline)
        return haveCached ? cachedResponse : Response{};

    // A pooled keep-alive connection can be half-dead (server closed it
    // while idle): the first send then fails. Retry once on a fresh
    // connection instead of surfacing a spurious load error.
    for (int attempt = 0; attempt < 2; ++attempt) {
    response = Response{};
    locationOut.clear();
    RequestHandles handles;
    if (!CreateRequest(url, handles, postBody ? L"POST" : L"GET")) {
        if (haveCached) return cachedResponse;
        if (attempt == 0) continue;
        return response;
    }

    if (postBody) {
        WinHttpAddRequestHeaders(
            handles.request,
            L"Content-Type: application/x-www-form-urlencoded\r\n",
            static_cast<DWORD>(-1),
            WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }

    std::wstring conditionalHeaders;
    if (haveCached) {
        if (!cachedMeta.etag.empty())
            conditionalHeaders += L"If-None-Match: " + cachedMeta.etag + L"\r\n";
        if (!cachedMeta.lastModified.empty())
            conditionalHeaders += L"If-Modified-Since: " + cachedMeta.lastModified + L"\r\n";
    }

    if (!conditionalHeaders.empty()) {
        WinHttpAddRequestHeaders(handles.request, conditionalHeaders.c_str(),
                                 static_cast<DWORD>(-1),
                                 WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }

    g_networkRequests.fetch_add(1);

    void* bodyData = postBody && !postBody->empty()
        ? const_cast<void*>(static_cast<const void*>(postBody->data()))
        : nullptr;
    const DWORD bodySize = postBody
        ? static_cast<DWORD>(postBody->size()) : 0;

    BOOL sent = WinHttpSendRequest(
        handles.request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        bodyData, bodySize, bodySize, 0);
    if (!sent && IsDeadConnectionError(GetLastError()))
        handles.discardConnection = true;

    if (sent)
        sent = WinHttpReceiveResponse(handles.request, nullptr);
    if (!sent && IsDeadConnectionError(GetLastError()))
        handles.discardConnection = true;

    if (sent) {
        DWORD status = 0;
        DWORD size = sizeof(status);
        if (WinHttpQueryHeaders(
                handles.request,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX,
                &status, &size, WINHTTP_NO_HEADER_INDEX)) {
            response.status = status;
        }

        response.contentType = QueryHeader(handles.request, WINHTTP_QUERY_CONTENT_TYPE);
        {
            const std::wstring len =
                QueryHeader(handles.request, WINHTTP_QUERY_CONTENT_LENGTH);
            uint64_t value = 0;
            for (wchar_t ch : len) {
                if (ch < L'0' || ch > L'9') break;
                value = value * 10 + static_cast<unsigned>(ch - L'0');
            }
            response.contentLength = value;
            // Expose to the UI for the "loaded/total" progress readout.
            if (value > 0) g_loadTotal.store(value);
        }

        if (response.status == HTTP_STATUS_NOT_MODIFIED && haveCached) {
            cachedMeta.savedAt = UnixNow();
            SaveCache(url, cachedResponse, cachedMeta);
            response = std::move(cachedResponse);
            response.status = HTTP_STATUS_OK; // treat as fresh 200 for callers
            response.fromCache = true;
            response.ok = true;
            g_cacheHits.fetch_add(1);
        } else if (IsRedirectStatus(response.status)) {
            locationOut = QueryHeaderValue(handles.request, L"Location");
            // Redirect bodies are irrelevant; do not read them.
            response.ok = false;
        } else {
            const bool bodyOk = ReadResponseBody(handles.request, response.body,
                                                 cancel, progress);
            response.ok = bodyOk && response.status >= 200 && response.status < 300;
            // Some servers return 2xx only; keep 3xx non-redirect (e.g. 304 handled above)
            // out of the ok set, matching previous behavior loosely.
            if (response.status >= 300 && response.status < 400)
                response.ok = false;

            if (cacheable && response.status == HTTP_STATUS_OK &&
                response.ok) {
                const std::wstring cacheControl =
                    QueryHeader(handles.request, WINHTTP_QUERY_CACHE_CONTROL);

                CacheMeta newMeta;
                newMeta.savedAt = UnixNow();
                newMeta.maxAge = ReadMaxAge(cacheControl);
                newMeta.etag = QueryHeader(handles.request, WINHTTP_QUERY_ETAG);
                newMeta.lastModified =
                    QueryHeader(handles.request, WINHTTP_QUERY_LAST_MODIFIED);
                newMeta.contentType = response.contentType;

                if (LowerWide(cacheControl).find(L"no-store") != std::wstring::npos) {
                    RemoveCacheEntry(url);
                } else {
                    SaveCache(url, response, newMeta);
                }
            }
        }
    }

    CloseRequest(handles);

    const bool canRetry = attempt == 0 && handles.borrowedConnection &&
                          !response.ok && locationOut.empty() &&
                          !(cancel && cancel->load());
    if (!canRetry)
        break;
    }

    if (cancel && cancel->load()) {
        Response cancelled;
        cancelled.status = response.status;
        return cancelled;
    }

    if (!response.ok && haveCached && locationOut.empty() &&
        !IsRedirectStatus(response.status)) {
        cachedResponse.fromCache = true;
        return cachedResponse; // Stale-if-error fallback.
    }

    return response;
}

// Resolve a (possibly relative) Location header against the request URL.
std::wstring ResolveRedirectUrl(const std::wstring& base,
                                const std::wstring& location) {
    std::wstring target = TrimWide(location);
    if (target.empty())
        return {};

    // Protocol-relative URL: "//host/path".
    if (target.rfind(L"//", 0) == 0) {
        // Determine the scheme of the base URL.
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        parts.dwSchemeLength = static_cast<DWORD>(-1);
        parts.dwHostNameLength = static_cast<DWORD>(-1);
        parts.dwUrlPathLength = static_cast<DWORD>(-1);
        parts.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (WinHttpCrackUrl(base.c_str(), 0, 0, &parts)) {
            const wchar_t* scheme = parts.nScheme == INTERNET_SCHEME_HTTP
                                        ? L"http" : L"https";
            target = std::wstring(scheme) + L":" + target;
        }
    }

    wchar_t absolute[8192]{};
    DWORD absoluteLength = static_cast<DWORD>(std::size(absolute));
    const HRESULT hr = UrlCombineW(base.c_str(), target.c_str(),
                                   absolute, &absoluteLength, 0);
    if (FAILED(hr))
        return {};

    return NormalizeUrl(absolute);
}

Response Request(const std::wstring& url, CacheMode mode,
                 std::atomic_bool* cancel, std::wstring* resolvedUrl,
                 const std::string* postBody,
                 std::atomic_uint64_t* progressBytes) {
    std::wstring current = url;
    Response last;
    // POST bodies survive only 307/308 redirects; 301/302/303 degrade to GET,
    // matching browser behavior.
    std::string pendingPost = postBody ? *postBody : std::string();
    const std::string* activePost = postBody ? &pendingPost : nullptr;

    for (size_t hop = 0; hop < MAX_REDIRECTS; ++hop) {
        if (cancel && cancel->load()) {
            Response cancelled;
            cancelled.status = last.status;
            return cancelled;
        }

        std::wstring location;
        Response response = RequestOnce(current, mode, cancel, location,
                                        activePost, progressBytes);
        last = response;

        if (cancel && cancel->load())
            return Response{};

        if (response.ok || location.empty() || !IsRedirectStatus(response.status)) {
            if (resolvedUrl) *resolvedUrl = current;
            return response;
        }

        if (activePost && response.status != 307 && response.status != 308) {
            pendingPost.clear();
            activePost = nullptr; // degrade to GET on the next hop
        }

        const std::wstring next = ResolveRedirectUrl(current, location);
        if (next.empty()) {
            if (resolvedUrl) *resolvedUrl = current;
            return response; // broken redirect: return what we have
        }
        current = next;
        mode = CacheMode::Use; // revalidate only the first hop
    }

    // Too many redirects.
    Response failed;
    failed.status = last.status;
    if (resolvedUrl) *resolvedUrl = current;
    return failed;
}
