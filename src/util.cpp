#include "browser.h"

#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

#include <lexbor/punycode/punycode.h>

#include <array>
#include <cwchar>

// ---------------------------------------------------------------------------
// Encoding helpers
// ---------------------------------------------------------------------------

// All user data (cache, history, bookmarks, tabs, downloads, saved pages)
// lives next to the executable, not in the process working directory —
// otherwise files land wherever the shortcut/shell happened to point.
static fs::path ExeDir() {
    static const fs::path dir = [] {
        wchar_t buffer[MAX_PATH * 2]{};
        const DWORD n = GetModuleFileNameW(nullptr, buffer,
                                            static_cast<DWORD>(std::size(buffer)));
        fs::path p = n > 0 ? fs::path(buffer) : fs::current_path();
        return p.has_parent_path() ? p.parent_path() : fs::current_path();
    }();
    return dir;
}

fs::path DataDirPath() {
    static const fs::path dir = [] {
        const fs::path d = ExeDir() / L"data";
        std::error_code ec;
        fs::create_directories(d, ec);
        return d;
    }();
    return dir;
}

fs::path DownloadsDirPath() {
    static const fs::path dir = [] {
        fs::path d = ExeDir() / L"downloads";
        // Optional override in data/settings.txt: "downloads=<path>"
        std::wifstream in(DataDirPath() / L"settings.txt");
        std::wstring line;
        while (std::getline(in, line)) {
            if (line.rfind(L"downloads=", 0) == 0) {
                const std::wstring v = TrimWide(line.substr(10));
                if (!v.empty()) { d = v; break; }
            }
        }
        std::error_code ec;
        fs::create_directories(d, ec);
        return d;
    }();
    return dir;
}

fs::path CacheDirPath() {
    static const fs::path dir = [] {
        const fs::path d = ExeDir() / L"cache";
        std::error_code ec;
        fs::create_directories(d, ec);
        return d;
    }();
    return dir;
}

fs::path SavedDirPath() {
    static const fs::path dir = [] {
        const fs::path d = ExeDir() / L"saved";
        std::error_code ec;
        fs::create_directories(d, ec);
        return d;
    }();
    return dir;
}

fs::path TabsFilePath() { return DataDirPath() / L"tabs.txt"; }

std::wstring ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int needed = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.c_str(), -1, nullptr, 0);
    if (needed <= 1) return {};

    std::wstring result(static_cast<size_t>(needed), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                             value.c_str(), -1, result.data(), needed))
        return {};
    result.pop_back();
    return result;
}

std::string ToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int needed = WideCharToMultiByte(
        CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return {};

    std::string result(static_cast<size_t>(needed), '\0');
    if (!WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1,
                             result.data(), needed, nullptr, nullptr))
        return {};
    result.pop_back();
    return result;
}

std::string HashUrl(const std::wstring& url) {
    // FNV-1a is used only to create compact cache filenames, not for security.
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char ch : ToUtf8(url)) {
        hash ^= ch;
        hash *= 1099511628211ULL;
    }
    std::ostringstream out;
    out << std::hex << hash;
    return out.str();
}

uint64_t UnixNow() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

void PrintLine(const std::string& text) {
    std::lock_guard<std::mutex> lock(g_outputMutex);
    std::cout << text << '\n';
}

std::wstring LowerWide(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return value;
}

std::string LowerAscii(std::string value) {
    for (char& ch : value) {
        const unsigned char u = static_cast<unsigned char>(ch);
        if (u >= 'A' && u <= 'Z')
            ch = static_cast<char>(u - 'A' + 'a');
    }
    return value;
}

std::string Trim(const std::string& value) {
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::wstring TrimWide(std::wstring value) {
    const size_t first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    const size_t last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}

// ---------------------------------------------------------------------------
// URL helpers
// ---------------------------------------------------------------------------

// Encode one IDN label (UTF-8) to punycode via Lexbor.
static std::string PunyEncodeLabel(const std::string& utf8label) {
    std::string out;
    const auto cb = [](const lxb_char_t* data, size_t len, void* ctx,
                       bool /*unchanged*/) -> lxb_status_t {
        auto* result = static_cast<std::string*>(ctx);
        result->append(reinterpret_cast<const char*>(data), len);
        return LXB_STATUS_OK;
    };
    if (lxb_punycode_encode(
            reinterpret_cast<const lxb_char_t*>(utf8label.data()),
            utf8label.size(), cb, &out) != LXB_STATUS_OK)
        return {};
    return out;
}

// Convert non-ASCII domain labels to their "xn--" punycode form so WinHTTP
// can resolve internationalized domain names (IDN).
static std::wstring EncodeIdnHost(const std::wstring& host) {
    std::wstring result;
    result.reserve(host.size() + 8);
    size_t start = 0;

    auto flushLabel = [&](size_t end) -> bool {
        const std::wstring label = host.substr(start, end - start);
        const bool ascii = std::all_of(label.begin(), label.end(),
            [](wchar_t ch) { return ch < 128; });
        if (ascii) {
            result += label;
        } else {
            const std::string puny = PunyEncodeLabel(ToUtf8(label));
            if (puny.empty())
                return false;
            result += L"xn--";
            result += ToWide(puny);
        }
        return true;
    };

    for (size_t i = 0; i <= host.size(); ++i) {
        if (i == host.size() || host[i] == L'.') {
            if (!flushLabel(i))
                return {};
            if (i < host.size())
                result += L'.';
            start = i + 1;
        }
    }
    return result;
}

std::wstring NormalizeUrl(std::wstring url) {
    url = TrimWide(url);
    while (!url.empty() && (url.front() == L' ' || url.front() == L'\t'))
        url.erase(url.begin());

    if (url.find(L"://") == std::wstring::npos) {
        // A scheme without "://" (mailto:, javascript:, tel:, data: ...) means
        // an unsupported protocol — reject it instead of treating the scheme
        // name as a hostname. "host:port" is allowed because the character
        // after ':' is a digit.
        const size_t colon = url.find(L':');
        if (colon != std::wstring::npos && colon > 0 && colon < 32) {
            bool schemeLike = std::iswalpha(url[0]) != 0;
            for (size_t i = 1; schemeLike && i < colon; ++i) {
                const wchar_t ch = url[i];
                if (!std::iswalnum(ch) && ch != L'+' && ch != L'-' && ch != L'.')
                    schemeLike = false;
            }
            if (schemeLike && colon + 1 < url.size() &&
                !std::iswdigit(url[colon + 1]))
                return {};
        }
        url = L"https://" + url;
    }

    // IDN: rewrite a non-ASCII host to punycode before validation.
    const size_t schemePos = url.find(L"://");
    if (schemePos != std::wstring::npos) {
        const size_t hostBegin = schemePos + 3;
        size_t hostEnd = url.find_first_of(L"/?#", hostBegin);
        if (hostEnd == std::wstring::npos) hostEnd = url.size();
        size_t hostLimit = hostEnd;
        const size_t colon = url.find(L':', hostBegin);
        if (colon != std::wstring::npos && colon < hostEnd)
            hostLimit = colon; // keep :port untouched
        const std::wstring host = url.substr(hostBegin, hostLimit - hostBegin);
        const bool nonAscii = std::any_of(host.begin(), host.end(),
            [](wchar_t ch) { return ch >= 128; });
        if (nonAscii) {
            const std::wstring encoded = EncodeIdnHost(host);
            if (encoded.empty())
                return {};
            url.replace(hostBegin, hostLimit - hostBegin, encoded);
        }
    }

    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts))
        return {};

    if (parts.nScheme != INTERNET_SCHEME_HTTP &&
        parts.nScheme != INTERNET_SCHEME_HTTPS)
        return {};

    return url;
}

bool IsUrlLike(const std::wstring& value) {
    if (value.find(L' ') != std::wstring::npos ||
        value.find(L'\t') != std::wstring::npos)
        return false;
    return value.rfind(L"http://", 0) == 0 ||
           value.rfind(L"https://", 0) == 0 ||
           value.rfind(L"www.", 0) == 0 ||
           value.find(L'.') != std::wstring::npos ||
           value.rfind(L"localhost", 0) == 0;
}

std::string UrlEncode(const std::string& text) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() + 8);

    for (unsigned char ch : text) {
        if ((ch >= 'a' && ch <= 'z') ||
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            out.push_back(static_cast<char>(ch));
        } else if (ch == ' ') {
            out.push_back('+');
        } else {
            out.push_back('%');
            out.push_back(hex[ch >> 4]);
            out.push_back(hex[ch & 0x0F]);
        }
    }
    return out;
}

static int HexDigit(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

std::string PercentDecode(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() &&
            HexDigit(text[i + 1]) >= 0 && HexDigit(text[i + 2]) >= 0) {
            out.push_back(static_cast<char>(
                HexDigit(text[i + 1]) * 16 + HexDigit(text[i + 2])));
            i += 2;
        } else if (text[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// SHA-256 (BCrypt) — used by download verification
// ---------------------------------------------------------------------------

bool Sha256File(const fs::path& path, std::array<BYTE, 32>& digest) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    bool ok = false;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM,
                                    nullptr, 0) == 0) {
        DWORD objLen = 0, cb = 0;
        BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&objLen),
                          sizeof(objLen), &cb, 0);
        std::vector<BYTE> obj(static_cast<size_t>(objLen));
        if (BCryptCreateHash(alg, &hash, obj.data(), objLen,
                             nullptr, 0, 0) == 0) {
            std::array<BYTE, 65536> buf{};
            DWORD read = 0;
            bool feedOk = true;
            while (ReadFile(file, buf.data(),
                            static_cast<DWORD>(buf.size()), &read, nullptr) &&
                   read > 0) {
                if (BCryptHashData(hash, buf.data(), read, 0) != 0) {
                    feedOk = false;
                    break;
                }
            }
            ok = feedOk &&
                 BCryptFinishHash(hash, digest.data(),
                                  static_cast<ULONG>(digest.size()), 0) == 0;
        }
        if (hash) BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    CloseHandle(file);
    return ok;
}

std::string Sha256Hex(const std::array<BYTE, 32>& digest) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (BYTE b : digest) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0xF]);
    }
    return out;
}

// ---------------------------------------------------------------------------
// ANSI print helpers
// ---------------------------------------------------------------------------

namespace ui {
    const char* reset   = "\x1b[0m";
    const char* cyan    = "\x1b[96m";
    const char* blue    = "\x1b[94m";
    const char* green   = "\x1b[92m";
    const char* yellow  = "\x1b[93m";
    const char* red     = "\x1b[91m";
    const char* muted   = "\x1b[90m";
    const char* white   = "\x1b[97m";
    const char* bold    = "\x1b[1m";
    const char* magenta = "\x1b[95m";

    std::string color(const char* code, const std::string& value) {
        if (!g_useAnsi) return value;
        return std::string(code) + value + reset;
    }

    std::string color(const char* first, const char* second,
                      const std::string& value) {
        if (!g_useAnsi) return value;
        return std::string(first) + second + value + reset;
    }

    bool Enable() {
        HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (output == INVALID_HANDLE_VALUE || !GetConsoleMode(output, &mode))
            return false;
        return SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
    }

    void Banner() {
        std::cout
            << "\n"
            << color(cyan, "  ╭──────────────────────────────────────────────────────────────────────╮") << "\n"
            << color(cyan, "  │") << "  " << color(bold, white, "BROWSER") << "  "
            << color(muted, "C++20  ·  WINDOWS  ·  HTML5  ·  HTTPS")
            << "                 " << color(green, "v4.0") << "  "
            << color(cyan, "│") << "\n"
            << color(cyan, "  ├──────────────────────────────────────────────────────────────────────┤") << "\n"
            << color(cyan, "  │") << "  " << color(green, "READY")
            << "  " << color(muted, "Search  |  Mouse  |  Keyboard  |  History")
            << "  " << color(cyan, "│") << "\n"
            << color(cyan, "  ╰──────────────────────────────────────────────────────────────────────╯") << "\n"
            << "  " << color(blue, "open") << " <url>   "
            << color(blue, "search") << " <words>   "
            << color(blue, "back") << "   "
            << color(blue, "forward") << "   "
            << color(blue, "help") << "\n\n";
    }

    void Prompt() {
        std::cout << color(cyan, "browser") << " "
                  << color(green, "›") << " " << std::flush;
    }

    void Section(const std::string& title) {
        std::cout << "\n" << color(blue, "  ── ")
                  << color(bold, white, title)
                  << color(blue, " ─────────────────────────────────────────────")
                  << "\n";
    }

    void Notice(const std::string& label, const std::string& value,
                bool success) {
        std::cout << "  " << color(success ? green : yellow, label)
                  << "  " << value << "\n";
    }

    void PageHeader(const std::string& title, const std::wstring& url,
                    DWORD status, size_t bytes, bool fromCache) {
        std::cout << "\n"
                  << color(cyan, "  ╭─ PAGE ──────────────────────────────────────────────────────────────╮")
                  << "\n";
        const std::string displayTitle =
            title.empty() ? std::string("(untitled page)") : title;
        std::cout << color(cyan, "  │ ") << color(bold, white, displayTitle) << "\n";
        std::cout << color(cyan, "  │ ") << color(muted, ToUtf8(url)) << "\n";
        std::cout << color(cyan, "  │ ") << color(green, "HTTP " + std::to_string(status))
                  << "  " << color(muted, std::to_string(bytes) + " bytes")
                  << "  " << color(fromCache ? green : yellow,
                                      fromCache ? "CACHE" : "NETWORK")
                  << "\n";
        std::cout << color(cyan, "  ╰────────────────────────────────────────────────────────────────────╯")
                  << "\n";
    }

    void Footer() {
        std::cout << "\n"
                  << color(muted, "  Shortcuts: open N  |  back  |  forward  |  reload  |  find <text>  |  help")
                  << "\n";
    }

    void Good(const std::string& text) {
        std::cout << "  " << color(green, "✓") << " " << text << "\n";
    }

    void Warn(const std::string& text) {
        std::cout << "  " << color(yellow, "!") << " " << text << "\n";
    }

    void Error(const std::string& text) {
        std::cout << "  " << color(red, "ERROR") << " " << text << "\n";
    }
}
