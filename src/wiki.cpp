// wiki.cpp — offline Wikipedia library.
// Resumable article downloads (ru/en) via the MediaWiki API, local HTML
// rendering served to the WebView2 window through the wiki.local virtual
// host, and title/content search from the console.

#include "browser.h"
#include "wvhost.h"

#include <algorithm>
#include <atomic>
#include <codecvt>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <locale>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace {

// ---------------------------------------------------------------------------
// Minimal JSON parser (objects/arrays/strings/numbers/bools/null, \uXXXX).
// ---------------------------------------------------------------------------

struct Json {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool boolean = false;
    double num = 0;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;

    const Json* find(const std::string& key) const {
        if (type != Obj) return nullptr;
        for (const auto& [k, v] : obj)
            if (k == key) return &v;
        return nullptr;
    }
    std::string strOf(const std::string& key) const {
        const Json* v = find(key);
        return v && v->type == Str ? v->str : std::string();
    }
};

struct JsonParser {
    const std::string& s;
    size_t p = 0;
    explicit JsonParser(const std::string& src) : s(src) {}

    void ws() { while (p < s.size() && (s[p] == ' ' || s[p] == '\t' ||
           s[p] == '\n' || s[p] == '\r')) ++p; }
    bool eat(char c) { ws(); if (p < s.size() && s[p] == c) { ++p; return true; } return false; }
    bool lit(const char* w) {
        const size_t n = std::strlen(w);
        if (s.compare(p, n, w) == 0) { p += n; return true; }
        return false;
    }
    void appendUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) out.push_back(static_cast<char>(cp));
        else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    int hex4() {
        if (p + 4 > s.size()) return -1;
        int v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s[p++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return -1;
        }
        return v;
    }
    bool parseString(std::string& out) {
        if (!eat('"')) return false;
        out.clear();
        while (p < s.size()) {
            const char c = s[p++];
            if (c == '"') return true;
            if (c == '\\') {
                if (p >= s.size()) return false;
                const char e = s[p++];
                switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    int cp = hex4();
                    if (cp < 0) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF && p + 6 <= s.size() &&
                        s[p] == '\\' && s[p + 1] == 'u') {
                        p += 2;
                        const int lo = hex4();
                        if (lo >= 0xDC00 && lo <= 0xDFFF)
                            cp = 0x10000 + ((cp - 0xD800) << 10) +
                                 (lo - 0xDC00);
                    }
                    appendUtf8(out, static_cast<uint32_t>(cp));
                    break;
                }
                default: return false;
                }
            } else {
                out.push_back(c);
            }
        }
        return false;
    }
    bool parseValue(Json& v) {
        ws();
        if (p >= s.size()) return false;
        const char c = s[p];
        if (c == '{') {
            ++p; v.type = Json::Obj;
            ws();
            if (eat('}')) return true;
            for (;;) {
                std::string key;
                if (!parseString(key)) return false;
                if (!eat(':')) return false;
                Json val;
                if (!parseValue(val)) return false;
                v.obj.emplace_back(std::move(key), std::move(val));
                if (eat('}')) return true;
                if (!eat(',')) return false;
            }
        }
        if (c == '[') {
            ++p; v.type = Json::Arr;
            ws();
            if (eat(']')) return true;
            for (;;) {
                Json val;
                if (!parseValue(val)) return false;
                v.arr.push_back(std::move(val));
                if (eat(']')) return true;
                if (!eat(',')) return false;
            }
        }
        if (c == '"') {
            v.type = Json::Str;
            return parseString(v.str);
        }
        if (lit("true")) { v.type = Json::Bool; v.boolean = true; return true; }
        if (lit("false")) { v.type = Json::Bool; v.boolean = false; return true; }
        if (lit("null")) { v.type = Json::Null; return true; }
        // number
        const char* start = s.c_str() + p;
        char* end = nullptr;
        const double d = std::strtod(start, &end);
        if (end == start) return false;
        p += static_cast<size_t>(end - start);
        v.type = Json::Num; v.num = d;
        return true;
    }
};

bool ParseJson(const std::string& text, Json& out) {
    JsonParser parser(text);
    return parser.parseValue(out);
}

} // namespace

// ---------------------------------------------------------------------------
// Paths and index
// ---------------------------------------------------------------------------

fs::path WikiRoot() {
    return DataDirPath().parent_path() / L"wiki";
}

fs::path WikiLangDir(const std::string& lang) {
    return WikiRoot() / ToWide(lang);
}

fs::path WikiIndexPath() { return WikiRoot() / L"index.tsv"; }

// Folder mapped to https://wiki.local/ inside the WebView2 window.
fs::path WikiHtmlRoot() {
    const fs::path d = WikiRoot() / L"html";
    std::error_code ec;
    fs::create_directories(d / L"ru", ec);
    fs::create_directories(d / L"en", ec);
    return d;
}

std::wstring SafeFileName(const std::wstring& title) {
    std::wstring out;
    out.reserve(title.size());
    for (wchar_t c : title) {
        const bool ok =
            (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') ||
            (c >= L'0' && c <= L'9') || c == L'.' || c == L'-' || c == L'_' ||
            c >= 0x80; // keep cyrillic/CJK/etc.
        out.push_back(ok ? c : L'_');
    }
    while (!out.empty() && (out.back() == L' ' || out.back() == L'.'))
        out.pop_back();
    if (out.empty()) out = L"article";
    if (out.size() > 120) out.resize(120);
    return out;
}

struct WikiEntry {
    std::string lang;
    std::wstring title;
    std::wstring txtName;   // file inside wiki/{lang}/
};

namespace {

// Wide streams with an explicit UTF-8 codecvt — the classic locale cannot
// convert Cyrillic/CJK and would silently mangle article text.
std::wifstream OpenUtf8In(const fs::path& p) {
    std::wifstream f(p);
    f.imbue(std::locale(std::locale::classic(),
                        new std::codecvt_utf8<wchar_t>));
    return f;
}

std::wofstream OpenUtf8Out(const fs::path& p,
                           std::ios::openmode m = std::ios::trunc) {
    std::wofstream f(p, m);
    f.imbue(std::locale(std::locale::classic(),
                        new std::codecvt_utf8<wchar_t>));
    return f;
}

// Index cache: LoadIndex() returns a shared snapshot so the UI thread can
// iterate it while the download worker keeps appending (AppendIndex marks
// the cache dirty; the next load rebuilds it).
std::mutex g_indexMutex;
bool g_indexDirty = true;
std::shared_ptr<const std::vector<WikiEntry>> g_indexCache;

} // namespace

std::shared_ptr<const std::vector<WikiEntry>> LoadIndex() {
    {
        std::lock_guard<std::mutex> lock(g_indexMutex);
        if (!g_indexDirty && g_indexCache) return g_indexCache;
    }
    auto entries = std::make_shared<std::vector<WikiEntry>>();
    auto in = OpenUtf8In(WikiIndexPath());
    std::wstring line;
    while (std::getline(in, line)) {
        const size_t t1 = line.find(L'\t');
        const size_t t2 = t1 == std::wstring::npos
            ? std::wstring::npos : line.find(L'\t', t1 + 1);
        if (t1 == std::wstring::npos || t2 == std::wstring::npos) continue;
        WikiEntry e;
        e.lang = ToUtf8(line.substr(0, t1));
        e.title = line.substr(t1 + 1, t2 - t1 - 1);
        e.txtName = line.substr(t2 + 1);
        // strip a trailing \t<size> if present
        const size_t t3 = e.txtName.find_last_of(L'\t');
        if (t3 != std::wstring::npos) e.txtName.resize(t3);
        entries->push_back(std::move(e));
    }
    std::lock_guard<std::mutex> lock(g_indexMutex);
    g_indexCache = entries;
    g_indexDirty = false;
    return entries;
}

void AppendIndex(const WikiEntry& e, uint64_t bytes) {
    auto out = OpenUtf8Out(WikiIndexPath(), std::ios::app);
    out << ToWide(e.lang) << L'\t' << e.title << L'\t' << e.txtName
        << L'\t' << bytes << L'\n';
    std::lock_guard<std::mutex> lock(g_indexMutex);
    g_indexDirty = true;
}

// ---------------------------------------------------------------------------
// HTML generation for the WebView2 window
// ---------------------------------------------------------------------------

std::wstring HtmlEscape(const std::wstring& in) {
    std::wstring out;
    out.reserve(in.size() + 16);
    for (wchar_t c : in) {
        switch (c) {
        case L'&': out += L"&amp;"; break;
        case L'<': out += L"&lt;"; break;
        case L'>': out += L"&gt;"; break;
        case L'"': out += L"&quot;"; break;
        default: out.push_back(c);
        }
    }
    return out;
}

std::wstring JsEscape(const std::wstring& in) {
    std::wstring out;
    out.reserve(in.size() + 8);
    for (wchar_t c : in) {
        switch (c) {
        case L'\\': out += L"\\\\"; break;
        case L'"':  out += L"\\\""; break;
        case L'\n': out += L"\\n"; break;
        case L'\r': out += L"\\r"; break;
        case L'\t': out += L"\\t"; break;
        default:
            if (c < 0x20) {
                const wchar_t* hex = L"0123456789abcdef";
                out += L"\\u";
                out += hex[(c >> 12) & 0xF];
                out += hex[(c >> 8) & 0xF];
                out += hex[(c >> 4) & 0xF];
                out += hex[c & 0xF];
            } else {
                out.push_back(c);
            }
        }
    }
    return out;
}

const wchar_t* kArticleCss = LR"CSS(
*{box-sizing:border-box}
body{background:#0b0e11;color:#c9d1d9;margin:0;padding:14px 18px 60px;
font:13.5px/1.5 Consolas,'Cascadia Mono',monospace}
.wrap{max-width:840px;margin:0 auto}
h1{color:#58a6ff;font-size:17px;font-weight:600;margin:0 0 4px;
border-bottom:1px solid #21262d;padding-bottom:6px}
.meta{color:#8b949e;font-size:12px;margin-bottom:14px}
p{margin:0 0 10px}
a{color:#58a6ff;text-decoration:none}
a:hover{text-decoration:underline}
.nav{margin-top:24px;padding-top:8px;border-top:1px solid #21262d;
font-size:12.5px}
.res{display:block;padding:3px 8px;border-radius:4px;color:#58a6ff;
text-decoration:none;white-space:nowrap;overflow:hidden;
text-overflow:ellipsis}
.res:hover{background:#161b22;text-decoration:underline}
.res .l{color:#8b949e;font-size:11.5px;margin-left:8px}
.langs a{margin-right:14px}
#q{width:100%;padding:8px 10px;font:inherit;font-size:14px;
border-radius:6px;border:1px solid #30363d;background:#0d1117;
color:#e6edf3;outline:none}
#q:focus{border-color:#58a6ff}
#res{margin-top:10px}
)CSS";

fs::path ArticleHtmlPath(const WikiEntry& e) {
    return WikiHtmlRoot() / ToWide(e.lang) /
           (SafeFileName(e.title) + L".html");
}

// Renders wiki/{lang}/{txt}.txt into a styled HTML page (cached).
void EnsureArticleHtml(const WikiEntry& e) {
    const fs::path out = ArticleHtmlPath(e);
    std::error_code ec;
    if (fs::exists(out, ec)) {
        // Regenerate pages rendered with an older stylesheet.
        std::ifstream chk(out, std::ios::binary);
        std::string head(4096, '\0');
        chk.read(head.data(), static_cast<std::streamsize>(head.size()));
        head.resize(static_cast<size_t>(chk.gcount()));
        if (head.find("Consolas") != std::string::npos) return;
        fs::remove(out, ec);
    }

    auto in = OpenUtf8In(WikiLangDir(e.lang) / e.txtName);
    std::wstring title = e.title, text;
    std::getline(in, title); // first line is the title
    std::wstringstream ss;
    ss << in.rdbuf();
    text = ss.str();

    std::wostringstream body;
    body << LR"(<html><head><meta charset="utf-8"><title>)"
         << HtmlEscape(title) << LR"(</title><style>)" << kArticleCss
         << LR"(</style></head><body><div class="wrap">)";
    body << L"<h1>" << HtmlEscape(title) << L"</h1>";
    body << L"<div class=\"meta\">Wikipedia · " << ToWide(e.lang)
         << L" · локальная копия</div>";

    std::wstringstream para;
    auto flush = [&] {
        const std::wstring p = TrimWide(para.str());
        if (!p.empty())
            body << L"<p>" << HtmlEscape(p) << L"</p>";
        para.str(L"");
        para.clear();
    };
    std::wistringstream lines(text);
    std::wstring firstLine;
    std::getline(lines, firstLine); // skip the title line
    std::wstring line;
    while (std::getline(lines, line)) {
        if (TrimWide(line).empty()) {
            flush();
        } else {
            para << line << L'\n';
        }
    }
    flush();

    body << L"<div class=\"nav\"><a href=\"../index.html\">← Все статьи</a>"
            L" · <a href=\"https://wiki.local/\">Портал</a></div>"
         << L"</div></body></html>";

    auto fout = OpenUtf8Out(out);
    fout << body.str();
}

std::wstring ArticleWebUrl(const WikiEntry& e) {
    return L"https://wiki.local/" + ToWide(e.lang) + L"/" +
           SafeFileName(e.title) + L".html";
}

// ---------------------------------------------------------------------------
// Portal page + JS search index
// ---------------------------------------------------------------------------

void RebuildPortal() {
    const fs::path root = WikiHtmlRoot();
    const auto entriesPtr = LoadIndex();
    const auto& entries = *entriesPtr;

    // articles.js — embedded title→file map for in-browser search.
    {
        auto js = OpenUtf8Out(root / L"articles.js");
        js << L"window.WIKI=[";
        const size_t cap = std::min<size_t>(entries.size(), 150000);
        for (size_t i = 0; i < cap; ++i) {
            if (i) js << L',';
            js << L"[\"" << JsEscape(entries[i].title)
               << L"\",\"" << ToWide(entries[i].lang) << L"/"
               << SafeFileName(entries[i].title) << L".html\"]";
        }
        js << L"];\n";
    }

    // Per-language index pages.
    for (const char* lang : {"ru", "en"}) {
        std::vector<const WikiEntry*> mine;
        for (const auto& e : entries)
            if (e.lang == lang) mine.push_back(&e);
        auto lp = OpenUtf8Out(WikiHtmlRoot() / ToWide(lang) / L"index.html");
        lp << LR"L1(<html><head><meta charset="utf-8"><title>Wikipedia )L1"
           << ToWide(lang) << LR"L2(</title><style>)L2" << kArticleCss
           << LR"L3(
)L3" << LR"L4(</style></head><body><div class="wrap">
<h1>Wikipedia — )L4" << ToWide(lang)
           << LR"L5(</h1><div class="meta">)L5" << mine.size()
           << LR"L6( статей</div>)L6";
        for (const WikiEntry* e : mine)
            lp << L"<a class=\"res\" href=\"" << SafeFileName(e->title)
               << L".html\">" << HtmlEscape(e->title) << L"</a>";
        lp << L"<div class=\"nav\"><a href=\"../index.html\">← Портал</a></div>"
              L"</div></body></html>";
    }

    size_t ru = 0, en = 0;
    for (const auto& e : entries) {
        if (e.lang == "ru") ++ru; else if (e.lang == "en") ++en;
    }

    auto out = OpenUtf8Out(root / L"index.html");
    out << LR"W1(<html><head><meta charset="utf-8"><title>Wikipedia — офлайн</title>
<style>)W1" << kArticleCss << LR"W2()W2" << LR"W3(</style></head><body><div class="wrap">
<h1>Wikipedia — офлайн библиотека</h1>
<div class="meta">Статей: )W3" << entries.size()
       << LR"W4( · RU: )W4" << ru << LR"W5( · EN: )W5" << en
       << LR"W6(</div>
<div class="langs"><a href="ru/index.html">Русская</a><a href="en/index.html">English</a></div>
<input id="q" placeholder="Поиск по заголовкам…" autofocus>
<div id="res"></div>
<script src="articles.js"></script>
<script>
const q=document.getElementById('q'),res=document.getElementById('res');
q.addEventListener('input',()=>{
  const s=q.value.trim().toLowerCase();res.innerHTML='';
  if(!s)return;
  let n=0;
  for(const [t,f] of window.WIKI){
    if(t.toLowerCase().includes(s)){
      const a=document.createElement('a');a.className='res';
      a.href=f;a.textContent=t;res.appendChild(a);
      if(++n>=200)break;
    }
  }
  if(!n)res.innerHTML='<div class="meta">Ничего не найдено</div>';
});
</script></div></body></html>)W6";
}

// ---------------------------------------------------------------------------
// Download worker
// ---------------------------------------------------------------------------

std::atomic_bool g_wikiStop{false};
std::thread g_wikiThread;
std::mutex g_wikiMutex;
std::wstring g_wikiStatus;

struct WikiProgress {
    std::string apcontinue;
    long long done = 0;
};

WikiProgress LoadProgress(const std::string& lang) {
    WikiProgress pr;
    std::ifstream in(WikiLangDir(lang) / ".progress");
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("continue=", 0) == 0) pr.apcontinue = line.substr(9);
        else if (line.rfind("done=", 0) == 0)
            pr.done = std::atoll(line.c_str() + 5);
    }
    return pr;
}

void SaveProgress(const std::string& lang, const WikiProgress& pr) {
    std::ofstream out(WikiLangDir(lang) / ".progress", std::ios::trunc);
    out << "continue=" << pr.apcontinue << "\ndone=" << pr.done << "\n";
}

std::string JsonRequest(const std::wstring& url) {
    Response r = Request(url, CacheMode::Bypass, &g_wikiStop);
    if (!r.ok || r.status != 200) return {};
    return std::string(r.body.begin(), r.body.end());
}

void WikiWorker(std::string lang, long long maxTotal) {
    const bool countMode = maxTotal > 0;
    WikiProgress pr = LoadProgress(lang);
    long long produced = pr.done;

    auto setStatus = [&](const std::wstring& s) {
        std::lock_guard<std::mutex> lock(g_wikiMutex);
        g_wikiStatus = s;
        g_consoleStatus = s;
    };

    for (;;) {
        if (g_wikiStop) break;
        if (countMode && produced >= maxTotal) break;

        // 1) next batch of titles
        std::wstring api =
            L"https://" + ToWide(lang) +
            L".wikipedia.org/w/api.php?action=query&format=json"
            L"&formatversion=2&list=allpages&apnamespace=0&aplimit=500";
        if (!pr.apcontinue.empty())
            api += L"&apcontinue=" + ToWide(UrlEncode(pr.apcontinue));
        const std::string json = JsonRequest(api);
        if (json.empty()) {
            if (g_wikiStop) break;
            setStatus(L"Wikipedia: ошибка API, повтор через 5 с…");
            Sleep(5000);
            continue;
        }
        Json root;
        if (!ParseJson(json, root)) {
            setStatus(L"Wikipedia: не удалось разобрать ответ API.");
            break;
        }
        const Json* query = root.find("query");
        const Json* pages = query ? query->find("allpages") : nullptr;
        const Json* cont = root.find("continue");
        if (cont) pr.apcontinue = cont->strOf("apcontinue");
        else pr.apcontinue.clear();

        std::vector<std::string> titles;
        if (pages && pages->type == Json::Arr)
            for (const auto& pg : pages->arr) {
                const std::string t = pg.strOf("title");
                if (!t.empty()) titles.push_back(t);
            }
        if (titles.empty()) break; // nothing more

        // 2) fetch extracts in chunks of 20 titles
        for (size_t off = 0; off < titles.size() && !g_wikiStop; off += 20) {
            if (countMode && produced >= maxTotal) break;
            std::wstring tlist;
            const size_t end = std::min(off + 20, titles.size());
            for (size_t i = off; i < end; ++i) {
                if (i > off) tlist += L"|";
                tlist += ToWide(UrlEncode(titles[i]));
            }
            const std::wstring exUrl =
                L"https://" + ToWide(lang) +
                L".wikipedia.org/w/api.php?action=query&format=json"
                L"&formatversion=2&prop=extracts&explaintext=1"
                L"&exsectionformat=plain&redirects=1&titles=" + tlist;
            const std::string exJson = JsonRequest(exUrl);
            if (exJson.empty()) {
                if (!g_wikiStop) Sleep(2000);
                continue;
            }
            Json exRoot;
            if (!ParseJson(exJson, exRoot)) continue;
            const Json* q = exRoot.find("query");
            const Json* plist = q ? q->find("pages") : nullptr;
            if (!plist || plist->type != Json::Arr) continue;

            for (const auto& pg : plist->arr) {
                if (g_wikiStop || (countMode && produced >= maxTotal))
                    break;
                // extract text is UTF-8
                std::string extract;
                if (const Json* ex = pg.find("extract"))
                    if (ex->type == Json::Str) extract = ex->str;
                const std::string titleUtf8 = pg.strOf("title");
                if (extract.empty() && titleUtf8.empty()) continue;
                const std::wstring title =
                    ToWide(titleUtf8.empty() ? titles[off] : titleUtf8);
                const std::wstring file = SafeFileName(title) + L".txt";

                {
                    // The lang dir may not exist yet on a fresh install —
                    // without this the .txt write silently fails while the
                    // index still records the article.
                    std::error_code dirEc;
                    fs::create_directories(WikiLangDir(lang), dirEc);
                    auto out = OpenUtf8Out(WikiLangDir(lang) / file);
                    out << title << L"\n\n" << ToWide(extract);
                }
                WikiEntry e{lang, title, file};
                AppendIndex(e, static_cast<uint64_t>(extract.size()));
                ++produced;
                ++pr.done;
            }
            setStatus(L"Wikipedia " + ToWide(lang) + L": " +
                      std::to_wstring(produced) + L" статей…");
            Sleep(150); // be polite to the API
        }
        SaveProgress(lang, pr);
        if (pr.apcontinue.empty()) break; // reached the end
    }

    {
        std::lock_guard<std::mutex> lock(g_wikiMutex);
        g_wikiStatus = g_wikiStop
            ? L"Wikipedia: остановлено."
            : L"Wikipedia: загрузка завершена.";
    }
    g_consoleStatus = g_wikiStatus;
    RebuildPortal();
}

// ---------------------------------------------------------------------------
// Search + open
// ---------------------------------------------------------------------------

// Trigram index over lowered titles, rebuilt only when the index snapshot
// changes. Queries of 3+ chars intersect posting lists instead of scanning
// every title; shorter queries fall back to a linear scan.
struct WikiTitleIndex {
    const std::vector<WikiEntry>* key = nullptr;
    std::vector<std::wstring> lowered;
    std::unordered_map<std::wstring, std::vector<uint32_t>> grams;
};
std::mutex g_titleMutex;
WikiTitleIndex g_titleIndex;

const WikiTitleIndex& GetTitleIndex(const std::vector<WikiEntry>& entries) {
    std::lock_guard<std::mutex> lock(g_titleMutex);
    if (g_titleIndex.key == &entries) return g_titleIndex;
    WikiTitleIndex fresh;
    fresh.key = &entries;
    fresh.lowered.reserve(entries.size());
    fresh.grams.reserve(entries.size() * 8);
    for (uint32_t i = 0; i < entries.size(); ++i) {
        const std::wstring low = LowerWide(entries[i].title);
        fresh.lowered.push_back(low);
        if (low.size() < 3) continue;
        std::unordered_set<std::wstring> seen;
        for (size_t j = 0; j + 3 <= low.size(); ++j) {
            const std::wstring g = low.substr(j, 3);
            if (seen.insert(g).second)
                fresh.grams[g].push_back(i);
        }
    }
    g_titleIndex = std::move(fresh);
    return g_titleIndex;
}

std::vector<WikiEntry> SearchTitles(const std::wstring& query,
                                    size_t limit = 50) {
    const std::wstring q = LowerWide(query);
    std::vector<WikiEntry> out;
    if (q.empty()) return out;
    const auto entries = LoadIndex();
    const WikiTitleIndex& idx = GetTitleIndex(*entries);

    if (q.size() >= 3 && idx.grams.size() > entries->size() / 4) {
        // Intersect posting lists of the query's trigrams.
        std::vector<uint32_t> candidates;
        bool first = true;
        for (size_t j = 0; j + 3 <= q.size() && candidates.size() < 4096;
             ++j) {
            const auto it = idx.grams.find(q.substr(j, 3));
            if (it == idx.grams.end()) return out; // impossible match
            if (first) { candidates = it->second; first = false; }
            else {
                std::vector<uint32_t> next;
                std::set_intersection(candidates.begin(), candidates.end(),
                                      it->second.begin(), it->second.end(),
                                      std::back_inserter(next));
                candidates = std::move(next);
                if (candidates.empty()) return out;
            }
        }
        for (uint32_t i : candidates) {
            if (idx.lowered[i].find(q) != std::wstring::npos) {
                out.push_back((*entries)[i]);
                if (out.size() >= limit) break;
            }
        }
        return out;
    }

    for (size_t i = 0; i < entries->size(); ++i) {
        if (idx.lowered[i].find(q) != std::wstring::npos) {
            out.push_back((*entries)[i]);
            if (out.size() >= limit) break;
        }
    }
    return out;
}

void OpenEntry(const WikiEntry& e) {
    EnsureArticleHtml(e);
    wv::Show(ArticleWebUrl(e));
}

void OpenResultsPage(const std::wstring& query,
                     const std::vector<WikiEntry>& results,
                     const std::wstring& note = L"") {
    const fs::path path = WikiHtmlRoot() / L"results.html";
    auto out = OpenUtf8Out(path);
    out << LR"R1(<html><head><meta charset="utf-8"><title>Поиск: )R1"
        << HtmlEscape(query) << LR"R2(</title><style>)R2" << kArticleCss
        << LR"R3()R3" << LR"R4(</style></head><body><div class="wrap">
<h1>Поиск: )R4" << HtmlEscape(query) << LR"R5(</h1>)R5";
    if (!note.empty())
        out << L"<div class=\"meta\">" << HtmlEscape(note) << L"</div>";
    out << L"<div class=\"meta\">Найдено: " << results.size() << L"</div>";
    for (const auto& e : results) {
        out << L"<a class=\"res\" href=\"" << ToWide(e.lang) << L"/"
            << SafeFileName(e.title) << L".html\">"
            << HtmlEscape(e.title)
            << L"<span class=\"l\">" << ToWide(e.lang) << L"</span></a>";
    }
    out << L"<div class=\"nav\"><a href=\"index.html\">← Портал</a></div>"
           L"</div></body></html>";
    wv::Show(L"https://wiki.local/results.html");
}

std::wstring WikiStatusLine() {
    size_t ru = 0, en = 0;
    for (const auto& e : *LoadIndex()) {
        if (e.lang == "ru") ++ru; else if (e.lang == "en") ++en;
    }
    std::wstring s = L"Wikipedia офлайн: RU " + std::to_wstring(ru) +
                     L", EN " + std::to_wstring(en);
    {
        std::lock_guard<std::mutex> lock(g_wikiMutex);
        if (!g_wikiStatus.empty()) s += L" · " + g_wikiStatus;
    }
    return s;
}

// ---------------------------------------------------------------------------
// Page builder — wiki:// content flows through the normal load pipeline
// (LoadJob::Kind::Internal -> WikiFetchPage), so tabs, back/forward and
// history work exactly like for web pages.
// ---------------------------------------------------------------------------

std::wstring ArticleConsoleUrl(const WikiEntry& e) {
    return L"wiki://" + ToWide(e.lang) + L"/" + SafeFileName(e.title);
}

bool IsWikiPageUrl(const std::wstring& url) {
    return url.rfind(L"wiki://", 0) == 0;
}

namespace {

struct WikiPage {
    std::wstring title;
    std::wstring body;
    std::vector<Link> links;
};

// Returns an entry with an empty lang when the library is empty.
WikiEntry PickRandomEntry() {
    const auto entries = LoadIndex();
    WikiEntry none;
    if (entries->empty()) return none;
    static std::mt19937 rng{std::random_device{}()};
    return (*entries)[rng() % entries->size()];
}

WikiPage PortalPage() {
    const auto entries = LoadIndex();
    size_t ru = 0, en = 0;
    for (const auto& e : *entries) {
        if (e.lang == "ru") ++ru; else if (e.lang == "en") ++en;
    }
    WikiPage p;
    p.title = L"Wikipedia — офлайн библиотека";
    p.body += L"■ Wikipedia — офлайн библиотека\n";
    p.body += L"▎ Только текст · RU + EN · локальные копии\n";
    p.body += L"▎ Статей: " + std::to_wstring(entries->size()) +
              L"  (RU " + std::to_wstring(ru) + L" · EN " +
              std::to_wstring(en) + L")\n\x01\n";
    p.body += L"▎ wiki search <текст> — поиск по заголовкам\n";
    p.body += L"▎ wiki <запрос> — открыть статью сразу\n";
    p.body += L"▎ wiki grep <текст> — поиск по текстам статей\n";
    p.body += L"▎ wiki random — случайная статья\n";
    p.body += L"▎ wiki download <ru|en> [часть|all|N] — закачать ещё\n";
    p.body += L"▎ wiki stop — остановить закачку\n";
    p.body += L"▎ wiki status — статус загрузки\n";
    p.links.push_back({ToUtf8(L"Русская Википедия — указатель"),
                       L"wiki://ru"});
    p.links.push_back({ToUtf8(L"English Wikipedia — index"),
                       L"wiki://en"});
    p.links.push_back({ToUtf8(L"Случайная статья"), L"wiki://random"});
    // Article of the day — deterministic pick by calendar day.
    if (!entries->empty()) {
        std::time_t now = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &now);
        const WikiEntry& day =
            (*entries)[static_cast<size_t>(tm.tm_yday) % entries->size()];
        p.body += L"\x01\n";
        p.links.push_back({ToUtf8(L"Статья дня — " + day.title +
                                  L"  ·  " + ToWide(day.lang)),
                           ArticleConsoleUrl(day)});
    }
    return p;
}

WikiPage IndexPage(const std::string& lang) {
    const auto entries = LoadIndex();
    std::vector<const WikiEntry*> mine;
    for (const auto& e : *entries)
        if (e.lang == lang) mine.push_back(&e);

    WikiPage p;
    p.title = L"Wikipedia " + ToWide(lang);
    p.body += L"■ Wikipedia " + ToWide(lang) + L" — указатель\n";
    p.body += L"▎ " + std::to_wstring(mine.size()) + L" статей\n";
    const size_t cap = std::min<size_t>(mine.size(), 400);
    if (mine.size() > cap)
        p.body += L"▎ Показаны первые " + std::to_wstring(cap) +
                  L" — для поиска используйте wiki search <текст>\n";

    p.links.push_back({ToUtf8(L"← Портал Wikipedia"), L"wiki://portal"});
    p.links.push_back({ToUtf8(L"Случайная статья"), L"wiki://random"});
    for (size_t i = 0; i < cap; ++i)
        p.links.push_back({ToUtf8(mine[i]->title),
                           ArticleConsoleUrl(*mine[i])});
    return p;
}

WikiPage ArticlePage(WikiEntry e) {
    auto in = OpenUtf8In(WikiLangDir(e.lang) / e.txtName);
    std::wstring line;
    std::wstring title;
    std::getline(in, title); // first line is the title
    if (title.empty()) title = e.title;
    e.title = title;

    WikiPage p;
    p.title = title;
    p.body += L"■ " + title + L"\n";
    p.body += L"▎ Wikipedia · " + ToWide(e.lang) +
              L" · офлайн копия\n\x01\n";
    bool first = true;
    while (std::getline(in, line)) {
        const std::wstring t = TrimWide(line);
        if (t.empty()) {
            if (!first) p.body += L"\x01\n";
            continue;
        }
        first = false;
        // MediaWiki list markers → console bullets (colored bright).
        if (t.rfind(L"* ", 0) == 0 || t.rfind(L"*\t", 0) == 0)
            p.body += L"• " + TrimWide(t.substr(1)) + L"\n";
        else
            p.body += t + L"\n";
    }

    p.links.push_back({ToUtf8(L"← Портал Wikipedia"), L"wiki://portal"});
    p.links.push_back({ToUtf8(L"Указатель " + ToWide(e.lang)),
                       L"wiki://" + ToWide(e.lang)});
    p.links.push_back({ToUtf8(L"Случайная статья"), L"wiki://random"});
    p.links.push_back({ToUtf8(L"Искать «" + title + L"»"),
                       L"wiki://search/" +
                       ToWide(UrlEncode(ToUtf8(title)))});
    // Cross-language link: same title in the other wiki (exact match).
    const std::wstring other = e.lang == "ru" ? L"en" : L"ru";
    for (const auto& cand : *LoadIndex()) {
        if (cand.lang == ToUtf8(other) && cand.title == e.title) {
            p.links.push_back({ToUtf8(L"Также на " + other + L": " +
                                      cand.title),
                               ArticleConsoleUrl(cand)});
            break;
        }
    }
    return p;
}

WikiPage ResultsPage(const std::wstring& query,
                     const std::vector<WikiEntry>& results,
                     const std::wstring& note = L"") {
    WikiPage p;
    p.title = L"Wikipedia — поиск: " + query;
    p.body += L"■ Поиск: " + query + L"\n";
    p.body += L"▎ Найдено: " + std::to_wstring(results.size()) + L"\n";
    if (!note.empty()) p.body += L"▎ " + note + L"\n";
    p.body += L"\x01\n";
    p.body += L"▎ wiki <запрос> — открыть статью сразу\n";
    p.body += L"▎ wiki grep <текст> — поиск по текстам статей\n";
    p.body += L"▎ wiki random — случайная статья\n";

    p.links.push_back({ToUtf8(L"← Портал Wikipedia"), L"wiki://portal"});
    for (const auto& e : results)
        p.links.push_back({ToUtf8(e.title + L"  ·  " + ToWide(e.lang)),
                           ArticleConsoleUrl(e)});
    return p;
}

// Fetch one article's plaintext extract and store it under wiki/<lang>/.
// Heals index entries whose .txt was lost (e.g. the missing-directory bug)
// without re-running the whole download.
bool FetchArticleToDisk(const std::string& lang, const std::wstring& title) {
    const std::string titleUtf8 = ToUtf8(title);
    const std::wstring url =
        L"https://" + ToWide(lang) +
        L".wikipedia.org/w/api.php?action=query&format=json"
        L"&formatversion=2&prop=extracts&explaintext=1"
        L"&exsectionformat=plain&redirects=1&titles=" +
        ToWide(UrlEncode(titleUtf8));
    const std::string json = JsonRequest(url);
    if (json.empty()) return false;
    Json root;
    if (!ParseJson(json, root)) return false;
    const Json* q = root.find("query");
    const Json* pages = q ? q->find("pages") : nullptr;
    if (!pages || pages->type != Json::Arr || pages->arr.empty())
        return false;
    const Json& pg = pages->arr.front();
    std::string extract;
    if (const Json* ex = pg.find("extract"))
        if (ex->type == Json::Str) extract = ex->str;
    if (extract.empty()) return false;
    const std::string realTitle = pg.strOf("title");
    const std::wstring wtitle =
        ToWide(realTitle.empty() ? titleUtf8 : realTitle);
    const std::wstring file = SafeFileName(wtitle) + L".txt";
    std::error_code ec;
    fs::create_directories(WikiLangDir(lang), ec);
    auto out = OpenUtf8Out(WikiLangDir(lang) / file);
    out << wtitle << L"\n\n" << ToWide(extract);
    return true;
}

} // namespace

// wiki://portal | wiki://ru | wiki://en | wiki://random |
// wiki://<lang>/<file> | wiki://search/<urlencoded> | wiki://grep/<urlencoded>
FetchOutcome WikiFetchPage(const std::wstring& url) {
    FetchOutcome out;
    out.finalUrl = url;
    // Keep the original case for article filenames (SafeFileName preserves
    // it); lowercase only for keyword comparisons.
    const std::wstring restRaw = TrimWide(
        url.size() > 7 ? url.substr(7) : L"");
    const std::wstring rest = LowerWide(restRaw);

    WikiPage page;
    bool have = false;

    if (rest.empty() || rest == L"portal" || rest == L"index") {
        page = PortalPage();
        have = true;
    } else if (rest == L"random") {
        const WikiEntry e = PickRandomEntry();
        if (e.lang.empty()) {
            out.ok = false;
            out.error = L"Библиотека пуста — сначала wiki download ru|en.";
            return out;
        }
        out.finalUrl = ArticleConsoleUrl(e);
        page = ArticlePage(e);
        have = true;
    } else if (rest.rfind(L"search/", 0) == 0 || rest == L"search") {
        const std::wstring q = rest.size() > 7
            ? ToWide(PercentDecode(ToUtf8(TrimWide(rest.substr(7)))))
            : L"";
        page = ResultsPage(q, SearchTitles(q, 200));
        have = true;
    } else if (rest.rfind(L"grep/", 0) == 0) {
        const std::wstring q = LowerWide(ToWide(PercentDecode(
            ToUtf8(TrimWide(rest.substr(5))))));
        std::vector<WikiEntry> hits;
        if (!q.empty()) {
            for (const auto& e : *LoadIndex()) {
                if (hits.size() >= 50) break;
                auto in = OpenUtf8In(WikiLangDir(e.lang) / e.txtName);
                std::wstring line;
                bool found = false;
                while (std::getline(in, line)) {
                    if (LowerWide(line).find(q) != std::wstring::npos) {
                        found = true;
                        break;
                    }
                }
                if (found) hits.push_back(e);
            }
        }
        page = ResultsPage(q, hits,
            hits.empty() ? L"В текстах ничего не найдено." : L"");
        have = true;
    } else {
        const size_t slash = rest.find(L'/');
        const std::wstring lang = slash == std::wstring::npos
            ? rest : rest.substr(0, slash);
        if (lang != L"ru" && lang != L"en") {
            page = PortalPage();
            have = true;
        } else if (slash == std::wstring::npos ||
                   rest.substr(slash + 1) == L"index" ||
                   rest.substr(slash + 1).empty()) {
            page = IndexPage(lang == L"en" ? "en" : "ru");
            have = true;
        } else {
            // Article: wiki://<lang>/<SafeFileName(title)> — original case.
            const size_t slashRaw = restRaw.find(L'/');
            const std::wstring fileBase = slashRaw == std::wstring::npos
                ? L"" : restRaw.substr(slashRaw + 1);
            const std::string langUtf8 = lang == L"en" ? "en" : "ru";
            // Fast path: txtName is always SafeFileName(title)+".txt".
            std::error_code ec;
            const fs::path txtPath =
                WikiLangDir(langUtf8) / (fileBase + L".txt");
            WikiEntry e;
            bool found = false;
            if (fs::exists(txtPath, ec)) {
                e = WikiEntry{langUtf8, L"", fileBase + L".txt"};
                found = true;
            } else {
                for (const auto& cand : *LoadIndex()) {
                    if (cand.lang == langUtf8 &&
                        SafeFileName(cand.title) == fileBase) {
                        e = cand;
                        found = true;
                        break;
                    }
                }
            }
            if (found) {
                // Heal missing .txt files (older downloads lost them when
                // the lang dir didn't exist): fetch this one article now.
                const fs::path txtFile =
                    WikiLangDir(e.lang) / e.txtName;
                std::error_code ec;
                if (!fs::exists(txtFile, ec)) {
                    std::wstring title = e.title;
                    if (title.empty()) {
                        for (const auto& cand : *LoadIndex())
                            if (cand.lang == e.lang &&
                                cand.txtName == e.txtName) {
                                title = cand.title;
                                break;
                            }
                    }
                    if (title.empty()) title = fileBase;
                    if (!FetchArticleToDisk(e.lang, title)) {
                        out.ok = false;
                        out.error =
                            L"Файл статьи отсутствует, и докачать не "
                            L"удалось (нет сети?).";
                        return out;
                    }
                    e.title = title;
                    e.txtName = SafeFileName(title) + L".txt";
                }
                page = ArticlePage(e);
                have = true;
            }
        }
    }

    if (!have) {
        out.ok = false;
        out.error = L"Статья не найдена в локальной библиотеке.";
        return out;
    }
    out.ok = true;
    out.title = ToUtf8(page.title);
    out.text = NormalizeDisplayText(ToUtf8(page.body));
    out.links = std::move(page.links);
    return out;
}

// ---------------------------------------------------------------------------
// Public commands (browser.h)
// ---------------------------------------------------------------------------

void UiWiki(const std::wstring& args) {
    const std::wstring a = LowerWide(TrimWide(args));

    if (a.empty() || a == L"status" || a == L"help") {
        g_consoleStatus = WikiStatusLine() +
            L" | wiki download <ru|en> [часть|200mb|all|N] · "
            L"wiki search <текст> · wiki random · wiki stop · wiki portal";
        return;
    }
    if (a == L"stop") {
        g_wikiStop = true;
        g_consoleStatus = L"Wikipedia: останавливаю…";
        return;
    }
    if (a == L"random" || a == L"portal" || a == L"index") {
        UiOpenUrl(a == L"random" ? L"wiki://random" : L"wiki://portal");
        return;
    }
    if (a.rfind(L"download ", 0) == 0 || a == L"download") {
        std::wstring rest = TrimWide(args);
        if (rest.rfind(L"download", 0) == 0)
            rest = TrimWide(rest.substr(8));
        // forms: ru | en | ru 5000 | ru all | ru part
        std::wstring lang = L"ru";
        std::wstring mode = L"part";
        {
            const size_t sp = rest.find_first_of(L" \t");
            if (sp != std::wstring::npos) {
                lang = rest.substr(0, sp);
                mode = TrimWide(rest.substr(sp + 1));
            } else if (!rest.empty()) {
                lang = rest;
            }
        }
        lang = LowerWide(lang);
        mode = LowerWide(mode);
        if (lang != L"ru" && lang != L"en") {
            g_consoleStatus = L"Формат: wiki download <ru|en> [часть|all|N]";
            return;
        }
        long long maxTotal = 0;
        if (mode == L"all" || mode.empty()) maxTotal = -1;
        else if (mode == L"part" || mode == L"200mb") maxTotal = 50000;
        else {
            try { maxTotal = std::stoll(mode); }
            catch (...) {
                g_consoleStatus = L"Формат: wiki download <ru|en> [часть|all|N]";
                return;
            }
        }
        if (g_wikiThread.joinable()) {
            g_consoleStatus = L"Wikipedia: загрузка уже идёт.";
            return;
        }
        g_wikiStop = false;
        g_consoleStatus = L"Wikipedia: запуск загрузки…";
        g_wikiThread = std::thread(WikiWorker,
            lang == L"en" ? "en" : "ru", maxTotal);
        return;
    }
    if (a.rfind(L"search ", 0) == 0 || a.rfind(L"find ", 0) == 0) {
        const size_t sp = a.find_first_of(L" \t");
        const std::wstring q = TrimWide(args).substr(sp + 1);
        UiOpenUrl(L"wiki://search/" + ToWide(UrlEncode(ToUtf8(q))));
        return;
    }
    if (a.rfind(L"grep ", 0) == 0) {
        const std::wstring q = TrimWide(a.substr(5));
        if (q.empty()) {
            g_consoleStatus = L"Формат: wiki grep <текст>";
            return;
        }
        UiOpenUrl(L"wiki://grep/" + ToWide(UrlEncode(ToUtf8(q))));
        return;
    }
    if (a.rfind(L"open ", 0) == 0) {
        const std::wstring q = TrimWide(a.substr(5));
        const auto results = SearchTitles(q, 10);
        if (results.empty()) {
            g_consoleStatus = L"Статья не найдена: " + q;
            return;
        }
        UiOpenUrl(ArticleConsoleUrl(results.front()));
        return;
    }

    // Bare query → best title match opens directly, else results page.
    const auto results = SearchTitles(a, 50);
    if (results.empty()) {
        g_consoleStatus = L"Ничего не найдено. Подсказка: wiki download ru|en";
        return;
    }
    if (results.size() == 1 ||
        LowerWide(results.front().title) == a) {
        UiOpenUrl(ArticleConsoleUrl(results.front()));
        g_consoleStatus = L"Открыто: " + results.front().title;
        return;
    }
    UiOpenUrl(L"wiki://search/" + ToWide(UrlEncode(ToUtf8(a))));
}

void ShutdownWiki() {
    g_wikiStop = true;
    if (g_wikiThread.joinable()) g_wikiThread.join();
}
