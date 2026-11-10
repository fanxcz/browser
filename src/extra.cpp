// extra.cpp — ecosystem pack: reading queue, macros, declarative rules,
// per-site themes, offline wiki, feeds digest, batch markdown export,
// Windows integration (protocol/file assoc/toasts) and devtools-lite
// (netlog, response headers, cookie jar).

#include "browser.h"

#include <atomic>
#include <cstdio>
#include <ctime>
#include <set>
#include <unordered_map>

namespace {

fs::path QueueFilePath() { return DataDirPath() / L"queue.txt"; }
fs::path MacrosFilePath() { return DataDirPath() / L"macros.txt"; }
fs::path RulesDirPath() { return DataDirPath() / L"rules"; }
fs::path SitesDirPath() { return DataDirPath() / L"sites"; }
fs::path WikiDirPath() { return DataDirPath() / L"wiki"; }
fs::path NetLogPath() { return DataDirPath() / L"netlog.txt"; }
fs::path CookiesPath() { return DataDirPath() / L"cookies.txt"; }

std::vector<std::wstring> ReadLines(const fs::path& path) {
    std::vector<std::wstring> out;
    std::wifstream in(path);
    std::wstring line;
    while (std::getline(in, line)) {
        line = TrimWide(line);
        if (!line.empty() && line.front() != L'#') out.push_back(line);
    }
    return out;
}

void WriteLines(const fs::path& path,
                const std::vector<std::wstring>& lines) {
    std::wofstream out(path, std::ios::trunc);
    for (const auto& l : lines) out << l << L'\n';
}

// Split a wide string on '\t' (keeps empty fields).
std::vector<std::wstring> WSplitTabs(const std::wstring& s) {
    std::vector<std::wstring> out;
    size_t pos = 0;
    while (pos <= s.size()) {
        const size_t tab = s.find(L'\t', pos);
        out.push_back(s.substr(pos, tab == std::wstring::npos
                                   ? std::wstring::npos : tab - pos));
        if (tab == std::wstring::npos) break;
        pos = tab + 1;
    }
    return out;
}

// Host part of an http(s) URL (no scheme/credentials/port/path, no www.).
std::wstring HostOf(const std::wstring& url) {
    std::wstring h = url;
    const size_t scheme = h.find(L"://");
    if (scheme != std::wstring::npos) h = h.substr(scheme + 3);
    const size_t at = h.rfind(L'@');
    if (at != std::wstring::npos) h = h.substr(at + 1);
    const size_t slash = h.find_first_of(L"/?#");
    if (slash != std::wstring::npos) h = h.substr(0, slash);
    const size_t colon = h.rfind(L':');
    if (colon != std::wstring::npos && h.find(L']') == std::wstring::npos)
        h = h.substr(0, colon);
    if (h.rfind(L"www.", 0) == 0) h = h.substr(4);
    return LowerWide(h);
}

std::wstring Slugify(const std::wstring& name) {
    std::wstring out;
    for (wchar_t c : name) {
        // iswalnum is locale-dependent for Cyrillic — accept the whole
        // Cyrillic block explicitly so Russian article names slugify.
        const bool cyr = c >= 0x0400 && c <= 0x04FF;
        wchar_t lc = c;
        if (cyr && c >= 0x0410 && c <= 0x042F) lc = c + 0x20; // А-Я→а-я
        else if (!cyr) lc = static_cast<wchar_t>(std::towlower(c));
        if (std::iswalnum(c) || cyr || c == L'-' || c == L'_')
            out.push_back(lc);
        else if (!out.empty() && out.back() != L'-')
            out.push_back(L'-');
    }
    while (!out.empty() && out.back() == L'-') out.pop_back();
    return out;
}

// Run a hidden console helper (reg.exe, powershell) and wait for it.
std::wstring RunHidden(const std::wstring& commandLine,
                       DWORD timeoutMs = 15000) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return L"";
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    std::vector<wchar_t> cmd(commandLine.begin(), commandLine.end());
    cmd.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(rd); CloseHandle(wr);
        return L"Не удалось запустить: " + commandLine;
    }
    CloseHandle(wr);
    if (WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_TIMEOUT)
        TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    std::string out;
    char buf[4096]; DWORD got = 0;
    while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got)
        out.append(buf, got);
    CloseHandle(rd); CloseHandle(pi.hProcess);
    return ToWide(Trim(out));
}

std::wstring ExePath() {
    wchar_t buf[MAX_PATH * 2]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    return buf;
}

// ---------------------------------------------------------------------------
// Reading queue — data/queue.txt, one URL per line.
// ---------------------------------------------------------------------------

std::vector<std::wstring> LoadQueue() { return ReadLines(QueueFilePath()); }

// ---------------------------------------------------------------------------
// Declarative rules — data/rules/*.txt (+ data/rules.txt).
//   <url-substring>\t<action>
//   actions: hide <text> | replace <from> => <to> | theme <name>
// ---------------------------------------------------------------------------

struct SiteRule {
    std::wstring match;
    std::string action;   // UTF-8 tail after the match
};

std::vector<SiteRule> LoadSiteRules() {
    std::vector<SiteRule> rules;
    std::error_code ec;
    auto readFile = [&](const fs::path& p) {
        std::wifstream in(p);
        std::wstring line;
        while (std::getline(in, line)) {
            line = TrimWide(line);
            if (line.empty() || line.front() == L'#') continue;
            const size_t tab = line.find(L'\t');
            if (tab == std::wstring::npos) continue;
            SiteRule r;
            r.match = LowerWide(TrimWide(line.substr(0, tab)));
            r.action = ToUtf8(TrimWide(line.substr(tab + 1)));
            if (!r.match.empty() && !r.action.empty())
                rules.push_back(std::move(r));
        }
    };
    const fs::path single = DataDirPath() / L"rules.txt";
    if (fs::exists(single, ec)) readFile(single);
    const fs::path dir = RulesDirPath();
    if (fs::exists(dir, ec)) {
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (e.path().extension() == L".txt") readFile(e.path());
        }
    }
    return rules;
}

bool RuleMatches(const SiteRule& r, const std::wstring& urlLower) {
    return urlLower.find(r.match) != std::wstring::npos;
}

// Parse common feed date formats to unix time; 0 = unknown.
uint64_t ParseFeedDate(const std::string& d) {
    if (d.empty()) return 0;
    const std::string s = Trim(d);
    std::tm tm{};
    // ISO 8601: 2025-01-31T12:34:56Z / +03:00
    if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d",
                    &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                    &tm.tm_hour, &tm.tm_min, &tm.tm_sec) == 6) {
        tm.tm_year -= 1900; tm.tm_mon -= 1;
        return static_cast<uint64_t>(_mkgmtime(&tm));
    }
    // RFC 822: "Mon, 31 Jan 2025 12:34:56 +0300" / "31 Jan 2025 12:34:56 GMT"
    static const char* kMonths[] = {"Jan","Feb","Mar","Apr","May","Jun",
        "Jul","Aug","Sep","Oct","Nov","Dec"};
    int day = 0, year = 0, hh = 0, mm = 0, ss = 0, mon = -1;
    char monbuf[4]{};
    const char* p = s.c_str();
    if (std::sscanf(p, "%*3[^,], %d %3s %d %d:%d:%d",
                    &day, monbuf, &year, &hh, &mm, &ss) >= 6 ||
        std::sscanf(p, "%d %3s %d %d:%d:%d",
                    &day, monbuf, &year, &hh, &mm, &ss) >= 6) {
        for (int i = 0; i < 12; ++i)
            if (LowerAscii(monbuf) == LowerAscii(kMonths[i])) mon = i;
        if (mon >= 0) {
            tm = std::tm{};
            tm.tm_year = year - 1900; tm.tm_mon = mon;
            tm.tm_mday = day; tm.tm_hour = hh; tm.tm_min = mm;
            tm.tm_sec = ss;
            return static_cast<uint64_t>(std::mktime(&tm));
        }
    }
    return 0;
}

// Devtools-lite shared state.
struct HeaderRec { std::wstring url; std::wstring raw; };
std::mutex g_netMutex;
std::vector<HeaderRec> g_headerLog;   // last ~40 responses
std::wstring g_lastNetUrl;            // most recent non-internal URL

} // namespace

// Public wrappers for unit tests.
std::wstring SafeSlug(const std::wstring& name) { return Slugify(name); }
uint64_t FeedDateToUnix(const std::string& d) { return ParseFeedDate(d); }

size_t LoadReadingQueueCount() {
    size_t n = 0;
    std::wifstream in(QueueFilePath());
    std::wstring line;
    while (std::getline(in, line)) {
        line = TrimWide(line);
        if (!line.empty() && line.front() != L'#') ++n;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Reading queue — public API.
// ---------------------------------------------------------------------------

bool UiQueueAdd(const std::wstring& url) {
    const std::wstring u = NormalizeUrl(url);
    if (u.empty()) return false;
    auto q = LoadQueue();
    for (const auto& e : q)
        if (LowerWide(e) == LowerWide(u)) return false;
    q.push_back(u);
    WriteLines(QueueFilePath(), q);
    return true;
}

bool UiQueueRemove(size_t index1) {
    auto q = LoadQueue();
    if (index1 < 1 || index1 > q.size()) return false;
    q.erase(q.begin() + static_cast<long>(index1 - 1));
    WriteLines(QueueFilePath(), q);
    return true;
}

void UiQueueClear() {
    std::wofstream out(QueueFilePath(), std::ios::trunc);
}

bool UiQueuePop(std::wstring& urlOut) {
    auto q = LoadQueue();
    if (q.empty()) return false;
    urlOut = q.front();
    q.erase(q.begin());
    WriteLines(QueueFilePath(), q);
    return true;
}

// queue done <n>: mark item n as read — removes it and records the visit
// in history (so it won't be suggested as unread again).
bool UiQueueDone(size_t index1) {
    auto q = LoadQueue();
    if (index1 < 1 || index1 > q.size()) return false;
    const std::wstring url = q[index1 - 1];
    q.erase(q.begin() + static_cast<long>(index1 - 1));
    WriteLines(QueueFilePath(), q);
    AppendHistory(url);
    return true;
}

void UiQueueCommand(const std::wstring& args, std::wstring& statusOut) {
    const std::wstring a = TrimWide(args);
    const std::wstring lower = LowerWide(a);
    if (lower.empty() || lower == L"list") {
        statusOut = L"Очередь чтения: " +
            std::to_wstring(LoadQueue().size()) +
            L" — страница browser://queue";
        return;
    }
    if (lower.rfind(L"add ", 0) == 0) {
        statusOut = UiQueueAdd(a.substr(4))
            ? L"Добавлено в очередь чтения."
            : L"Не добавлено (пустой адрес или уже в очереди).";
        return;
    }
    if (lower.rfind(L"del ", 0) == 0) {
        try {
            const long n = std::stol(ToUtf8(TrimWide(a.substr(4))));
            statusOut = UiQueueRemove(static_cast<size_t>(n))
                ? L"Удалено из очереди."
                : L"Нет такого номера.";
        } catch (...) { statusOut = L"Формат: queue del <n>"; }
        return;
    }
    if (lower.rfind(L"done ", 0) == 0) {
        try {
            const long n = std::stol(ToUtf8(TrimWide(a.substr(5))));
            statusOut = UiQueueDone(static_cast<size_t>(n))
                ? L"Отмечено прочитанным (в истории)."
                : L"Нет такого номера.";
        } catch (...) { statusOut = L"Формат: queue done <n>"; }
        return;
    }
    if (lower == L"clear") {
        UiQueueClear();
        statusOut = L"Очередь чтения очищена.";
        return;
    }
    if (lower == L"next") {
        std::wstring u;
        if (UiQueuePop(u)) {
            statusOut = L"Очередь → " + u;
            UiOpenUrl(u);
        } else {
            statusOut = L"Очередь чтения пуста.";
        }
        return;
    }
    statusOut = L"queue add <url> · queue next · queue done|del <n> · "
                L"queue clear · queue list";
}

// ---------------------------------------------------------------------------
// Macros — data/macros.txt:  name = step ; step ; step
// ---------------------------------------------------------------------------

std::vector<std::wstring> ListMacroNames() {
    std::vector<std::wstring> names;
    std::wifstream in(MacrosFilePath());
    std::wstring line;
    while (std::getline(in, line)) {
        line = TrimWide(line);
        if (line.empty() || line.front() == L'#') continue;
        const size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        const std::wstring name = TrimWide(line.substr(0, eq));
        if (!name.empty()) names.push_back(name);
    }
    return names;
}

std::vector<std::wstring> LoadMacroSteps(const std::wstring& name) {
    std::vector<std::wstring> steps;
    const std::wstring want = LowerWide(TrimWide(name));
    std::wifstream in(MacrosFilePath());
    std::wstring line;
    while (std::getline(in, line)) {
        line = TrimWide(line);
        if (line.empty() || line.front() == L'#') continue;
        const size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        if (LowerWide(TrimWide(line.substr(0, eq))) != want) continue;
        const std::wstring rest = line.substr(eq + 1);
        size_t pos = 0;
        while (pos <= rest.size()) {
            const size_t semi = rest.find(L';', pos);
            const std::wstring step = TrimWide(rest.substr(
                pos, semi == std::wstring::npos
                         ? std::wstring::npos : semi - pos));
            if (!step.empty()) steps.push_back(step);
            if (semi == std::wstring::npos) break;
            pos = semi + 1;
        }
        break;
    }
    return steps;
}

// ---------------------------------------------------------------------------
// Declarative rules — public API.
// ---------------------------------------------------------------------------

void ApplyContentRules(const std::wstring& url, std::string& text) {
    if (text.empty()) return;
    const std::wstring urlLower = LowerWide(url);
    for (const auto& r : LoadSiteRules()) {
        if (!RuleMatches(r, urlLower)) continue;
        const std::string act = LowerAscii(r.action);
        if (act.rfind("hide ", 0) == 0) {
            const std::string needle = r.action.substr(5);
            if (needle.empty()) continue;
            const std::string lowNeedle = LowerAscii(needle);
            std::string out;
            size_t pos = 0;
            while (pos <= text.size()) {
                const size_t nl = text.find('\n', pos);
                const std::string line = text.substr(
                    pos, nl == std::string::npos
                             ? std::string::npos : nl - pos);
                if (LowerAscii(line).find(lowNeedle) ==
                    std::string::npos) {
                    out += line;
                    out += '\n';
                }
                if (nl == std::string::npos) break;
                pos = nl + 1;
            }
            text = std::move(out);
        } else if (act.rfind("replace ", 0) == 0) {
            // replace <from> => <to>   (also accepts ->)
            const std::string args = r.action.substr(8);
            const size_t arrow = args.find("=>");
            const size_t alt = args.find("->");
            size_t sep = std::string::npos;
            if (arrow != std::string::npos) sep = arrow;
            else if (alt != std::string::npos) sep = alt;
            if (sep == std::string::npos) continue;
            const std::string from = Trim(args.substr(0, sep));
            const std::string to = Trim(args.substr(sep + 2));
            if (from.empty()) continue;
            size_t p = 0;
            while ((p = text.find(from, p)) != std::string::npos) {
                text.replace(p, from.size(), to);
                p += to.size();
            }
        }
        // "theme <name>" is consumed by SiteThemeFor(), not here.
    }
}

// Per-site theme override: data/sites/<host>.theme wins, then a `theme`
// action in the rules files. Returns a theme name or "".
std::wstring SiteThemeFor(const std::wstring& url) {
    const std::wstring urlLower = LowerWide(url);
    const std::wstring host = HostOf(url);
    if (!host.empty()) {
        std::error_code ec;
        const fs::path f = SitesDirPath() / (host + L".theme");
        if (fs::exists(f, ec)) {
            std::wifstream in(f);
            std::wstring name;
            std::getline(in, name);
            name = TrimWide(name);
            if (!name.empty()) return name;
        }
    }
    for (const auto& r : LoadSiteRules()) {
        if (!RuleMatches(r, urlLower)) continue;
        const std::string act = LowerAscii(r.action);
        if (act.rfind("theme ", 0) == 0) {
            const std::wstring name = ToWide(Trim(r.action.substr(6)));
            if (!name.empty()) return name;
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// Offline wiki — wiki/<slug>.txt snapshots of Wikipedia articles.
// ---------------------------------------------------------------------------

void UiWikiSave(const std::wstring& article) {
    const std::wstring name = TrimWide(article);
    if (name.empty()) {
        g_consoleStatus = L"Формат: wsave <название статьи>";
        return;
    }
    const std::wstring slug = Slugify(name);
    if (slug.empty()) {
        g_consoleStatus = L"Не получилось имя файла.";
        return;
    }
    const std::wstring page =
        L"https://ru.wikipedia.org/wiki/" +
        ToWide(UrlEncode(ToUtf8(name)));
    g_consoleStatus = L"Сохраняю статью…";
    std::thread([page, slug, name]() {
        FetchOutcome out = FetchPage(page, CacheMode::Bypass);
        if (!out.ok || out.text.empty()) {
            g_consoleStatus = L"Не удалось скачать статью.";
            return;
        }
        const std::string filtered = FilterReaderText(out.text);
        std::error_code ec;
        fs::create_directories(WikiDirPath(), ec);
        const fs::path path = WikiDirPath() / (slug + L".txt");
        std::wofstream file(path, std::ios::trunc);
        file << L"# " << name << L"\n"
             << L"Источник: " << page << L"\n\n"
             << ToWide(filtered);
        g_consoleStatus = L"Статья сохранена офлайн: wiki/" + slug +
                          L".txt";
    }).detach();
}

FetchOutcome BuildWikiPage(const std::wstring& name) {
    FetchOutcome out;
    out.finalUrl = std::wstring(kInternalPrefix) + L"wiki";
    if (!name.empty()) {
        const fs::path f = WikiDirPath() / (Slugify(name) + L".txt");
        std::wifstream in(f);
        if (!in) {
            out.ok = false;
            out.error = L"Статья не сохранена: wiki/" + Slugify(name) +
                        L".txt";
            return out;
        }
        std::wstring content((std::istreambuf_iterator<wchar_t>(in)),
                             std::istreambuf_iterator<wchar_t>());
        out.ok = true;
        out.title = ToUtf8(name);
        out.text = ToUtf8(content);
        out.finalUrl = out.finalUrl + L"/" + Slugify(name);
        return out;
    }
    // Index of saved articles.
    out.ok = true;
    out.title = "Офлайн-википедия";
    std::string text =
        "Сохранённые статьи (wiki/*.txt).\n"
        "wsave <статья> — скачать и сохранить, woffline <имя> — открыть.\n\n";
    std::error_code ec;
    const fs::path dir = WikiDirPath();
    if (fs::exists(dir, ec)) {
        std::vector<std::wstring> files;
        for (const auto& e : fs::directory_iterator(dir, ec))
            if (e.path().extension() == L".txt")
                files.push_back(e.path().stem().wstring());
        std::sort(files.begin(), files.end());
        if (files.empty()) text += "Пока пусто.\n";
        for (size_t i = 0; i < files.size(); ++i) {
            const std::wstring label = files[i];
            text += std::to_string(i + 1) + ". " +
                    ToUtf8(label) + "\n";
            out.links.push_back(
                {std::to_string(i + 1) + ". " + ToUtf8(label),
                 std::wstring(kInternalPrefix) + L"wiki/" + label});
        }
    } else {
        text += "Папки wiki/ ещё нет.\n";
    }
    out.text = NormalizeDisplayText(text);
    return out;
}

// ---------------------------------------------------------------------------
// Feeds digest — everything new across subscriptions.
// ---------------------------------------------------------------------------

FetchOutcome BuildDigestPage() {
    FetchOutcome out;
    out.ok = true;
    out.finalUrl = std::wstring(kInternalPrefix) + L"digest";
    out.title = "Дайджест лент";
    out.isFeed = true;

    struct Row { std::wstring feed; std::string title, link, date, desc;
                 uint64_t ts; };
    std::vector<Row> rows;
    const uint64_t now = UnixNow();
    const auto feeds = LoadFeedSubscriptions();

    for (const auto& feedUrl : feeds) {
        Response r = Request(feedUrl, CacheMode::Use, nullptr);
        if (!r.ok) continue;
        const std::string body(r.body.begin(), r.body.end());
        if (!LooksLikeFeed(body)) continue;
        for (const auto& it : ParseFeedItems(body)) {
            if (it.title.empty() && it.link.empty()) continue;
            Row row;
            row.feed = feedUrl;
            row.title = it.title; row.link = it.link;
            row.date = it.date; row.desc = it.desc;
            row.ts = ParseFeedDate(it.date);
            // Old dated items (>7 days) are noise in a digest.
            if (row.ts && now > row.ts + 7ULL * 86400) continue;
            rows.push_back(std::move(row));
        }
    }
    std::stable_sort(rows.begin(), rows.end(),
                     [](const Row& a, const Row& b) {
                         return a.ts > b.ts; // newest first, 0-ts last
                     });
    if (rows.size() > 80) rows.resize(80);

    std::string text =
        "Дайджест по " + std::to_string(feeds.size()) +
        " лентам — свежие записи первыми.\n"
        "feeds digest — обновить, feed add/del — подписки.\n\n";
    if (rows.empty()) {
        text += "Новых записей нет или ленты недоступны.\n";
    } else {
        std::wstring lastFeed;
        int n = 0;
        for (const auto& row : rows) {
            ++n;
            const std::wstring feedName = HostOf(row.feed);
            if (feedName != lastFeed) {
                text += "■ " + ToUtf8(feedName) + "\n";
                lastFeed = feedName;
            }
            text += std::to_string(n) + ". " +
                    (row.title.empty() ? row.link : row.title) + "\n";
            if (!row.date.empty()) text += "   " + row.date + "\n";
            if (!row.desc.empty()) text += "   " + row.desc + "\n";
            const std::wstring abs = ResolveRedirectUrl(
                row.feed, ToWide(row.link));
            if (!abs.empty())
                out.links.push_back(
                    {std::to_string(n) + ". " +
                         (row.title.empty() ? row.link : row.title), abs});
            text += "\n";
        }
    }
    out.text = NormalizeDisplayText(text);
    return out;
}

// ---------------------------------------------------------------------------
// Batch markdown export — saved/articles/<date>/.
// ---------------------------------------------------------------------------

void UiExportMarkdown(int days) {
    if (days <= 0) days = 7;
    if (days > 365) days = 365;
    const uint64_t cutoff = UnixNow() - static_cast<uint64_t>(days) * 86400;
    g_consoleStatus = L"Экспорт статей запущен (до " +
                      std::to_wstring(days) + L" дн.)…";
    std::thread([cutoff, days]() {
        auto entries = ReadHistoryEntries(HistoryFilePath());
        std::vector<std::wstring> urls;
        std::unordered_map<std::wstring, bool> seen;
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            if (it->timestamp && it->timestamp < cutoff) continue;
            const std::wstring u = it->url;
            if (u.empty() || IsInternalPageUrl(u)) continue;
            if (u.rfind(L"http", 0) != 0) continue;
            if (seen.insert({LowerWide(u), true}).second)
                urls.push_back(u);
            if (urls.size() >= 15) break; // cap: each page is a fetch
        }
        std::time_t t = static_cast<std::time_t>(UnixNow());
        std::tm tm{};
        localtime_s(&tm, &t);
        wchar_t dbuf[16];
        std::swprintf(dbuf, 16, L"%04d-%02d-%02d",
                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
        const fs::path dir = SavedDirPath() / L"articles" / dbuf;
        std::error_code ec;
        fs::create_directories(dir, ec);

        int done = 0;
        std::string index = "# Экспорт статей — " + ToUtf8(dbuf) +
                            "\n\nИсточник: история браузера, " +
                            std::to_string(days) + " дн.\n\n";
        int n = 0;
        for (const auto& u : urls) {
            FetchOutcome page = FetchPage(u, CacheMode::Use);
            if (!page.ok || page.text.empty()) continue;
            ++n;
            std::string name = page.title.empty()
                ? ToUtf8(u) : page.title;
            for (char& c : name)
                if (!(std::isalnum(static_cast<unsigned char>(c)) ||
                      c == ' ' || c == '-' || c == '_'))
                    c = '_';
            if (name.size() > 60) name.resize(60);
            const fs::path f = dir /
                (std::to_string(n) + "_" + name + ".md");
            std::wofstream out(f, std::ios::trunc);
            out << L"# " << ToWide(page.title.empty()
                                      ? ToUtf8(u) : page.title) << L"\n\n"
                << L"> Источник: " << u << L"\n\n"
                << ToWide(page.text);
            index += "- [" +
                     (page.title.empty() ? ToUtf8(u) : page.title) +
                     "](" + ToUtf8(f.filename().wstring()) + ") — " +
                     ToUtf8(u) + "\n";
            ++done;
        }
        {
            std::wofstream idx(dir / L"index.md", std::ios::trunc);
            idx << ToWide(index);
        }
        g_consoleStatus = L"Экспортировано статей: " +
            std::to_wstring(done) + L" → saved/articles/" + dbuf;
    }).detach();
}

// ---------------------------------------------------------------------------
// Devtools-lite — netlog, response headers, cookie jar.
// ---------------------------------------------------------------------------

void NetLogRecord(const std::wstring& url, const wchar_t* method,
                  DWORD status, uint64_t bytes, uint64_t ms) {
    if (url.empty()) return;
    {
        std::lock_guard<std::mutex> lock(g_netMutex);
        if (!IsInternalPageUrl(url)) g_lastNetUrl = url;
    }
    const uint64_t now = UnixNow();
    std::wofstream out(NetLogPath(), std::ios::app);
    // unixtime \t method \t url \t status \t bytes \t ms
    out << now << L'\t' << (method ? method : L"GET") << L'\t'
        << url << L'\t' << status << L'\t' << bytes << L'\t'
        << ms << L'\n';
    std::error_code ec;
    if (fs::file_size(NetLogPath(), ec) > 64 * 1024) {
        auto lines = ReadLines(NetLogPath());
        if (lines.size() > 200) {
            lines.erase(lines.begin(),
                        lines.begin() + static_cast<long>(lines.size() - 200));
            WriteLines(NetLogPath(), lines);
        }
    }
}

void RecordResponseHeaders(const std::wstring& url,
                           const std::wstring& rawHeaders) {
    if (url.empty() || rawHeaders.empty()) return;
    {
        std::lock_guard<std::mutex> lock(g_netMutex);
        g_headerLog.push_back({url, rawHeaders});
        if (g_headerLog.size() > 40)
            g_headerLog.erase(g_headerLog.begin(),
                              g_headerLog.begin() +
                                  static_cast<long>(g_headerLog.size() - 40));
        if (!IsInternalPageUrl(url)) g_lastNetUrl = url;
    }
    // Cookie jar: harvest Set-Cookie lines.
    std::vector<std::wstring> lines;
    std::wistringstream ss(rawHeaders);
    std::wstring line;
    const std::wstring host = HostOf(url);
    while (std::getline(ss, line)) {
        const std::wstring low = LowerWide(line);
        if (low.rfind(L"set-cookie:", 0) == 0)
            lines.push_back(TrimWide(line.substr(11)));
    }
    if (lines.empty() || host.empty()) return;
    // Jar line: domain \t name=value \t expires \t secure \t path \t hostonly
    // (hostonly=1 means no Domain= attribute → exact-host match only).
    auto jar = ReadLines(CookiesPath());
    bool changed = false;
    // Request path for Path= matching.
    std::wstring reqPath = L"/";
    {
        const size_t scheme = url.find(L"://");
        if (scheme != std::wstring::npos) {
            size_t p = url.find(L'/', scheme + 3);
            if (p != std::wstring::npos) reqPath = url.substr(p);
        }
    }
    const bool isHttps = LowerWide(url).rfind(L"https:", 0) == 0;
    for (const auto& sc : lines) {
        // "name=value; Path=/; Domain=.x.com; Secure; Max-Age=60; Expires=…"
        std::wstring rest = sc;
        const size_t firstSemi = rest.find(L';');
        const std::wstring pair = TrimWide(rest.substr(
            0, firstSemi == std::wstring::npos ? rest.size() : firstSemi));
        const size_t eq = pair.find(L'=');
        if (eq == std::wstring::npos || eq == 0) continue;
        const std::wstring name = pair.substr(0, eq);
        if (firstSemi != std::wstring::npos)
            rest = rest.substr(firstSemi + 1);

        std::wstring cookieDomain = host;
        bool hostOnly = true, secure = false;
        std::wstring cookiePath = L"/";
        uint64_t exp = 0;
        // Parse attributes.
        size_t pos = 0;
        while (pos <= rest.size()) {
            const size_t semi = rest.find(L';', pos);
            const std::wstring attr = TrimWide(rest.substr(
                pos, semi == std::wstring::npos
                         ? std::wstring::npos : semi - pos));
            const std::wstring low = LowerWide(attr);
            auto attrVal = [&](const wchar_t* key) -> std::wstring {
                const std::wstring k = key;
                if (low.rfind(k, 0) != 0) return {};
                size_t v = k.size();
                while (v < attr.size() && attr[v] != L'=') ++v;
                if (v >= attr.size()) return {};
                return TrimWide(attr.substr(v + 1));
            };
            if (low == L"secure") secure = true;
            else if (const std::wstring v = attrVal(L"domain"); !v.empty()) {
                cookieDomain = LowerWide(v);
                if (!cookieDomain.empty() && cookieDomain.front() == L'.')
                    cookieDomain.erase(cookieDomain.begin());
                hostOnly = false;
            } else if (const std::wstring v = attrVal(L"path"); !v.empty()) {
                cookiePath = v.front() == L'/' ? v : L"/" + v;
            } else if (const std::wstring v = attrVal(L"max-age");
                       !v.empty()) {
                try {
                    const long age = std::stol(v);
                    exp = age > 0 ? UnixNow() + static_cast<uint64_t>(age) : 0;
                } catch (...) {}
            } else if (const std::wstring v = attrVal(L"expires");
                       !v.empty() && exp == 0) {
                exp = ParseFeedDate(ToUtf8(v));
            }
            if (semi == std::wstring::npos) break;
            pos = semi + 1;
        }
        // Upsert: drop existing jar entry for domain+name.
        jar.erase(std::remove_if(jar.begin(), jar.end(),
            [&](const std::wstring& e) {
                const auto f = WSplitTabs(e);
                if (f.size() < 2 ||
                    LowerWide(TrimWide(f[0])) != cookieDomain)
                    return false;
                const size_t e2 = f[1].find(L'=');
                return e2 != std::wstring::npos &&
                       f[1].substr(0, e2) == name;
            }), jar.end());
        jar.push_back(cookieDomain + L"\t" + pair + L"\t" +
                      std::to_wstring(exp) + L"\t" +
                      (secure ? L"1" : L"0") + L"\t" + cookiePath +
                      L"\t" + (hostOnly ? L"1" : L"0"));
        changed = true;
    }
    if (changed) WriteLines(CookiesPath(), jar);
}

std::wstring CookieHeaderFor(const std::wstring& url) {
    const std::wstring host = HostOf(url);
    if (host.empty()) return {};
    const uint64_t now = UnixNow();
    const bool isHttps = LowerWide(url).rfind(L"https:", 0) == 0;
    // Request path for Path= prefix matching.
    std::wstring reqPath = L"/";
    {
        const size_t scheme = url.find(L"://");
        if (scheme != std::wstring::npos) {
            const size_t p = url.find(L'/', scheme + 3);
            if (p != std::wstring::npos) reqPath = url.substr(p);
        }
    }
    std::wstring header;
    for (const auto& e : ReadLines(CookiesPath())) {
        const auto f = WSplitTabs(e);
        if (f.size() < 2) continue;
        const std::wstring dom = LowerWide(TrimWide(f[0]));
        uint64_t exp = 0;
        if (f.size() >= 3) {
            try { exp = std::stoull(TrimWide(f[2])); } catch (...) {}
        }
        if (exp && exp < now) continue; // expired
        const bool secure = f.size() >= 4 && TrimWide(f[3]) == L"1";
        if (secure && !isHttps) continue; // Secure cookies: https only
        // hostonly (old 3-field lines count as host-only).
        const bool hostOnly = f.size() < 6 || TrimWide(f[5]) == L"1";
        bool match;
        if (hostOnly) {
            match = dom == host;
        } else {
            match = dom == host ||
                (host.size() > dom.size() &&
                 host.compare(host.size() - dom.size(), dom.size(), dom) == 0 &&
                 host[host.size() - dom.size() - 1] == L'.');
        }
        if (!match) continue;
        // Path= prefix match (default "/" matches everything).
        const std::wstring cpath = f.size() >= 5 && !TrimWide(f[4]).empty()
            ? TrimWide(f[4]) : L"/";
        if (cpath != L"/" &&
            reqPath.compare(0, cpath.size(), cpath) != 0)
            continue;
        const std::wstring pair = TrimWide(f[1]);
        if (pair.empty()) continue;
        if (!header.empty()) header += L"; ";
        header += pair;
    }
    return header;
}

std::vector<std::wstring> HeadersForUrl(const std::wstring& url) {
    std::lock_guard<std::mutex> lock(g_netMutex);
    for (auto it = g_headerLog.rbegin(); it != g_headerLog.rend(); ++it)
        if (LowerWide(it->url) == LowerWide(url))
            return {it->raw};
    const std::wstring host = HostOf(url);
    if (!host.empty()) {
        for (auto it = g_headerLog.rbegin(); it != g_headerLog.rend(); ++it)
            if (HostOf(it->url) == host) return {it->raw};
    }
    return {};
}

void NetLogClear() {
    std::wofstream out(NetLogPath(), std::ios::trunc);
}

std::wstring NetLogExportHar() {
    const auto lines = ReadLines(NetLogPath());
    std::string json =
        "{\n  \"log\": {\n    \"version\": \"1.2\",\n"
        "    \"creator\": {\"name\": \"Browser Console\", "
        "\"version\": \"1.6\"},\n    \"entries\": [\n";
    bool first = true;
    int n = 0;
    for (const auto& line : lines) {
        const auto f = WSplitTabs(line);
        if (f.size() < 6) continue;
        uint64_t ts = 0;
        try { ts = std::stoull(TrimWide(f[0])); } catch (...) { continue; }
        if (ts < 1000000000) continue; // legacy HH:MM:SS lines
        const std::string method = ToUtf8(TrimWide(f[1]));
        const std::string url = ToUtf8(TrimWide(f[2]));
        const std::string status = ToUtf8(TrimWide(f[3]));
        const std::string bytes = ToUtf8(TrimWide(f[4]));
        const std::string ms = ToUtf8(TrimWide(f[5]));
        // ISO-8601 UTC timestamp.
        std::time_t t = static_cast<std::time_t>(ts);
        std::tm tm{};
        gmtime_s(&tm, &t);
        char iso[32];
        std::snprintf(iso, sizeof(iso), "%04d-%02d-%02dT%02d:%02d:%02d.000Z",
                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                      tm.tm_hour, tm.tm_min, tm.tm_sec);
        if (!first) json += ",\n";
        first = false;
        ++n;
        json += "      {\"startedDateTime\": \"" + std::string(iso) +
                "\", \"request\": {\"method\": \"" + method +
                "\", \"url\": \"" + url + "\"}, \"response\": {\"status\": " +
                status + ", \"content\": {\"size\": " + bytes +
                "}}, \"time\": " + ms + "}";
    }
    json += "\n    ]\n  }\n}\n";
    std::time_t t = static_cast<std::time_t>(UnixNow());
    std::tm tm{};
    localtime_s(&tm, &t);
    wchar_t dbuf[16];
    std::swprintf(dbuf, 16, L"%04d-%02d-%02d", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday);
    const fs::path dir = SavedDirPath();
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path path = dir / (L"netlog-" + std::wstring(dbuf) +
                                 L".har");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << json;
    if (!out) return {};
    return path;
}
void CookiesClearAll() {
    std::wofstream out(CookiesPath(), std::ios::trunc);
}
void CookiesClear() { CookiesClearAll(); }

FetchOutcome BuildHeadersPage() {
    FetchOutcome out;
    out.ok = true;
    out.finalUrl = std::wstring(kInternalPrefix) + L"headers";
    out.title = "Заголовки ответа";
    std::wstring url;
    {
        std::lock_guard<std::mutex> lock(g_netMutex);
        url = g_lastNetUrl;
    }
    auto recs = HeadersForUrl(url);
    std::string text;
    if (recs.empty()) {
        text = "Заголовки ещё не записаны — откройте любую страницу.\n";
    } else {
        std::wistringstream ss(recs.front());
        std::wstring line;
        text += "URL: " + ToUtf8(url) + "\n\n";
        while (std::getline(ss, line)) {
            if (!line.empty() && line.back() == L'\r') line.pop_back();
            text += ToUtf8(line) + "\n";
        }
    }
    out.text = NormalizeDisplayText(text);
    return out;
}

FetchOutcome BuildCookiesPage() {
    FetchOutcome out;
    out.ok = true;
    out.finalUrl = std::wstring(kInternalPrefix) + L"cookies";
    out.title = "Cookie";
    const auto jar = ReadLines(CookiesPath());
    std::string text = "Cookie-файлы (data/cookies.txt), " +
                       std::to_string(jar.size()) + " записей.\n"
                       "cookies clear — очистить.\n\n";
    for (const auto& e : jar) {
        const auto f = WSplitTabs(e);
        if (f.size() < 2) continue;
        uint64_t exp = 0;
        if (f.size() >= 3) {
            try { exp = std::stoull(TrimWide(f[2])); } catch (...) {}
        }
        const std::string when = exp
            ? "до " + FormatVisitTime(exp) : "сессия";
        text += ToUtf8(TrimWide(f[0])) + "  " +
                ToUtf8(TrimWide(f[1])) +
                "   (" + when + ")\n";
    }
    if (jar.empty()) text += "Пусто.\n";
    out.text = NormalizeDisplayText(text);
    return out;
}

FetchOutcome BuildNetlogPage() {
    FetchOutcome out;
    out.ok = true;
    out.finalUrl = std::wstring(kInternalPrefix) + L"netlog";
    out.title = "Журнал сети";
    auto lines = ReadLines(NetLogPath());
    std::string text = "Последние сетевые запросы (data/netlog.txt).\n"
                       "netlog clear — очистить.\n\n";
    const size_t start = lines.size() > 60 ? lines.size() - 60 : 0;
    for (size_t i = start; i < lines.size(); ++i) {
        const auto f = WSplitTabs(lines[i]);
        if (f.size() >= 6) {
            // Field 0 is a unix timestamp (new format); legacy lines held
            // HH:MM:SS there — show them as-is.
            std::wstring stamp = TrimWide(f[0]);
            try {
                const uint64_t ts = std::stoull(stamp);
                if (ts > 1000000000) {
                    std::time_t t = static_cast<std::time_t>(ts);
                    std::tm tm{};
                    localtime_s(&tm, &t);
                    wchar_t tb[32];
                    std::swprintf(tb, 32, L"%02d:%02d:%02d",
                                  tm.tm_hour, tm.tm_min, tm.tm_sec);
                    stamp = tb;
                }
            } catch (...) {}
            text += ToUtf8(stamp) + "  " + ToUtf8(TrimWide(f[1])) +
                    "  " + ToUtf8(TrimWide(f[3])) + "  " +
                    ToUtf8(TrimWide(f[4])) + "B  " +
                    ToUtf8(TrimWide(f[5])) + "ms  " + ToUtf8(TrimWide(f[2])) +
                    "\n";
        } else {
            text += ToUtf8(lines[i]) + "\n";
        }
    }
    if (lines.empty()) text += "Запросов пока не было.\n";
    out.text = NormalizeDisplayText(text);
    return out;
}

FetchOutcome BuildRulesPage() {
    FetchOutcome out;
    out.ok = true;
    out.finalUrl = std::wstring(kInternalPrefix) + L"rules";
    out.title = "Правила сайтов";
    std::string text =
        "Декларативные правила: data/rules/*.txt и data/rules.txt.\n"
        "Формат строки: <часть-url>\\thide <текст> | replace <от> => <до>"
        " | theme <имя>\n"
        "Темы по сайту: data/sites/<хост>.theme с именем темы.\n\n";
    auto rules = LoadSiteRules();
    if (rules.empty()) {
        text += "Правил нет.\n";
    } else {
        for (const auto& r : rules)
            text += ToUtf8(r.match) + "  →  " + r.action + "\n";
    }
    std::error_code ec;
    const fs::path sd = SitesDirPath();
    if (fs::exists(sd, ec)) {
        text += "\nТемы по сайтам:\n";
        for (const auto& e : fs::directory_iterator(sd, ec)) {
            if (e.path().extension() != L".theme") continue;
            std::wifstream in(e.path());
            std::wstring name;
            std::getline(in, name);
            text += "  " + ToUtf8(e.path().stem().wstring()) +
                    " → " + ToUtf8(TrimWide(name)) + "\n";
        }
    }
    out.text = NormalizeDisplayText(text);
    return out;
}

// browser://gallery — theme gallery: local packs + remote index.
FetchOutcome BuildGalleryPage() {
    FetchOutcome out;
    out.ok = true;
    out.finalUrl = std::wstring(kInternalPrefix) + L"gallery";
    out.title = "Галерея тем";
    std::string text =
        "Темы: theme <имя> — применить, theme pull <имя> — скачать,\n"
        "theme save <имя> — сохранить текущую.\n\n";

    std::set<std::string> local;
    {
        std::error_code ec;
        const fs::path dir = DataDirPath() / L"themes";
        if (fs::exists(dir, ec)) {
            for (const auto& e : fs::directory_iterator(dir, ec))
                if (e.path().extension() == L".json")
                    local.insert(ToUtf8(e.path().stem().wstring()));
        }
    }
    text += "Установленные темы (data/themes/):\n";
    if (local.empty()) text += "  (нет — только встроенные)\n";
    for (const auto& n : local) text += "  " + n + "\n";

    // Remote gallery index.
    Response r = Request(
        L"https://fanxcz.github.io/browser/themes/index.json",
        CacheMode::Use, nullptr);
    text += "\nГалерея (онлайн):\n";
    if (!r.ok) {
        text += "  недоступна — нет сети или индекс не опубликован.\n";
    } else {
        const std::string js(r.body.begin(), r.body.end());
        // "name": "x" occurrences.
        size_t pos = 0;
        int found = 0;
        while (found < 30) {
            const size_t p = js.find("\"name\"", pos);
            if (p == std::string::npos) break;
            const size_t q1 = js.find('"', js.find(':', p) + 1);
            if (q1 == std::string::npos) break;
            const size_t q2 = js.find('"', q1 + 1);
            if (q2 == std::string::npos) break;
            const std::string name = js.substr(q1 + 1, q2 - q1 - 1);
            pos = q2 + 1;
            if (name.empty()) continue;
            ++found;
            const bool have = local.count(name) > 0;
            text += "  " + name + (have ? "  [установлена]"
                                        : "  → theme pull " + name) + "\n";
        }
        if (!found) text += "  пусто.\n";
    }
    text += "\nСообщество: examples/macros.txt и examples/rules.txt в "
            "репозитории — готовые шаблоны.\n";
    out.text = NormalizeDisplayText(text);
    return out;
}

FetchOutcome BuildQueuePage() {
    FetchOutcome out;
    out.ok = true;
    out.finalUrl = std::wstring(kInternalPrefix) + L"queue";
    out.title = "Очередь чтения";
    const auto q = LoadQueue();
    std::string text = "Прочитать позже — " + std::to_string(q.size()) +
                       " страниц.\n"
                       "queue add <url>, queue next — открыть первую,\n"
                       "queue del <n>, queue clear.\n\n";
    if (q.empty()) text += "Очередь пуста.\n";
    for (size_t i = 0; i < q.size(); ++i) {
        text += std::to_string(i + 1) + ". " + ToUtf8(q[i]) + "\n";
        out.links.push_back(
            {std::to_string(i + 1) + ". " + ToUtf8(q[i]), q[i]});
    }
    out.text = NormalizeDisplayText(text);
    return out;
}

// browser://file/<percent-encoded-utf8-path> — local file viewer used by
// the .html file association.
FetchOutcome BuildFilePage(const std::wstring& encoded) {
    FetchOutcome out;
    // Percent-decode UTF-8.
    std::string bytes;
    for (size_t i = 0; i < encoded.size(); ++i) {
        const wchar_t c = encoded[i];
        if (c == L'%' && i + 2 < encoded.size()) {
            auto hex = [](wchar_t h) -> int {
                if (h >= L'0' && h <= L'9') return h - L'0';
                if (h >= L'a' && h <= L'f') return h - L'a' + 10;
                if (h >= L'A' && h <= L'F') return h - L'A' + 10;
                return -1;
            };
            const int hi = hex(encoded[i + 1]), lo = hex(encoded[i + 2]);
            if (hi >= 0 && lo >= 0) {
                bytes.push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        // wchar_t → UTF-8 for non-ASCII path chars.
        if (c < 0x80) bytes.push_back(static_cast<char>(c));
        else {
            std::wstring w(1, c);
            const std::string u = ToUtf8(w);
            bytes += u;
        }
    }
    const std::wstring path = ToWide(bytes);
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        out.ok = false;
        out.error = L"Файл не найден: " + path;
        return out;
    }
    const std::string raw((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
    out.ok = true;
    out.title = ToUtf8(fs::path(path).filename().wstring());
    out.finalUrl = std::wstring(kInternalPrefix) + L"file/" + encoded;
    const std::wstring ext = LowerWide(fs::path(path).extension().wstring());
    if (ext == L".html" || ext == L".htm") {
        ParsedPage page = ParseHtml5(raw, out.finalUrl);
        out.text = page.text;
        if (!page.title.empty()) out.title = page.title;
        ApplyContentRules(out.finalUrl, out.text);
        out.links = page.links;
    } else {
        out.text = NormalizeDisplayText(ToUtf8(ToWide(raw)));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Windows integration — protocol, file association, toasts.
// ---------------------------------------------------------------------------

std::wstring UiOsCommand(const std::wstring& args) {
    const std::wstring a = LowerWide(TrimWide(args));
    const std::wstring exe = ExePath();
    const std::wstring quoted = L"\"" + exe + L"\"";

    if (a == L"register-protocol" || a == L"protocol on") {
        const std::wstring cls = L"HKCU\\Software\\Classes\\browser";
        RunHidden(L"reg add \"" + cls + L"\" /ve /d \"URL:Browser Console\" /f");
        RunHidden(L"reg add \"" + cls + L"\\DefaultIcon\" /ve /d \"" +
                  quoted + L",0\" /f");
        RunHidden(L"reg add \"" + cls + L"\\shell\\open\\command\" /ve /d \"" +
                  quoted + L" \"%%1\"\" /f");
        return L"Протокол browser:// зарегистрирован (HKCU).";
    }
    if (a == L"unregister-protocol" || a == L"protocol off") {
        RunHidden(L"reg delete \"HKCU\\Software\\Classes\\browser\" /f");
        return L"Протокол browser:// удалён.";
    }
    if (a == L"fileassoc" || a == L"fileassoc on") {
        const std::wstring verb =
            L"HKCU\\Software\\Classes\\.html\\shell\\Browser Console";
        RunHidden(L"reg add \"" + verb + L"\" /ve /d \"Открыть в Browser\" /f");
        RunHidden(L"reg add \"" + verb + L"\\command\" /ve /d \"" +
                  quoted + L" \"%%1\"\" /f");
        return L"«Открыть в Browser» добавлено в меню .html.";
    }
    if (a == L"fileassoc off") {
        RunHidden(L"reg delete \"HKCU\\Software\\Classes\\.html\\shell\\Browser Console\" /f");
        return L"Пункт меню для .html удалён.";
    }
    return L"Команды: register-protocol, unregister-protocol, "
             L"fileassoc [on|off].";
}

void UiDownloadToast(const std::wstring& fileName) {
    std::wstring safe = fileName;
    for (auto& c : safe)
        if (c == L'\'' || c == L'"') c = L' ';
    const std::wstring script =
        L"[Windows.UI.Notifications.ToastNotificationManager,"
        L"Windows.UI.Notifications,ContentType=WindowsRuntime]|Out-Null;"
        L"[Windows.Data.Xml.Dom.XmlDocument,Windows.Data.Xml.Dom.XmlDocument,"
        L"ContentType=WindowsRuntime]|Out-Null;"
        L"$x=[Windows.UI.Notifications.ToastNotificationManager]"
        L"::GetTemplateContent([Windows.UI.Notifications."
        L"ToastTemplateType]::ToastText02);"
        L"$x.GetElementsByTagName('text')[0].AppendChild("
        L"$x.CreateTextNode('Browser — загрузка'))|Out-Null;"
        L"$x.GetElementsByTagName('text')[1].AppendChild("
        L"$x.CreateTextNode('" + safe + L"'))|Out-Null;"
        L"$t=[Windows.UI.Notifications.ToastNotification]::new($x);"
        L"[Windows.UI.Notifications.ToastNotificationManager]"
        L"::CreateToastNotifier('Browser').Show($t)";
    std::thread([script]() {
        RunHidden(L"powershell -NoProfile -WindowStyle Hidden -Command \"" +
                  script + L"\"", 20000);
    }).detach();
}

// ---------------------------------------------------------------------------
// Internal pages dispatcher.
// ---------------------------------------------------------------------------

FetchOutcome BuildExtraInternalPage(const std::wstring& url) {
    FetchOutcome out;
    const std::wstring kind = LowerWide(url.substr(
        std::wstring(kInternalPrefix).size()));
    if (kind == L"queue") return BuildQueuePage();
    if (kind == L"digest") return BuildDigestPage();
    if (kind == L"headers") return BuildHeadersPage();
    if (kind == L"cookies") return BuildCookiesPage();
    if (kind == L"netlog") return BuildNetlogPage();
    if (kind == L"rules") return BuildRulesPage();
    if (kind == L"gallery") return BuildGalleryPage();
    if (kind == L"sync") return BuildSyncPage();
    if (kind.rfind(L"wiki/", 0) == 0)
        return BuildWikiPage(url.substr(
            std::wstring(kInternalPrefix).size() + 5));
    if (kind == L"wiki") return BuildWikiPage(L"");
    if (kind.rfind(L"file/", 0) == 0)
        return BuildFilePage(url.substr(
            std::wstring(kInternalPrefix).size() + 5));
    return out; // ok=false → unknown
}
