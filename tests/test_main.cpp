// Unit tests for Browser core functions.
// Run from the build directory: browser_tests.exe
// Fixtures are read from ../tests/fixtures relative to the source tree via
// the BROWSER_FIXTURES define or the default relative path.

#include "../src/browser.h"

#include <cstdio>
#include <fstream>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            ++g_failures;                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                  \
    } while (0)

static std::string ReadFile(const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        // Tests may run from the repo root or from build/<Config>.
        for (const char* prefix : {"../", "../../"}) {
            const std::string alt = std::string(prefix) + path;
            file.open(alt, std::ios::binary);
            if (file) break;
        }
        if (!file) return {};
    }
    return std::string(std::istreambuf_iterator<char>(file),
                       std::istreambuf_iterator<char>());
}

// ---------------------------------------------------------------------------

static void TestEncoding() {
    std::printf("== encoding ==\n");
    const std::wstring wide = L"Привет, мир! 🌍";
    const std::string utf8 = ToUtf8(wide);
    CHECK(ToWide(utf8) == wide);
    CHECK(!ToWide("").empty() == false);
    CHECK(ToUtf8(L"").empty());

    CHECK(UrlEncode("hello world") == "hello+world");
    CHECK(UrlEncode("a&b=c") == "a%26b%3Dc");
    const std::string enc = UrlEncode("привет мир");
    CHECK(enc.find('%') != std::string::npos);
    CHECK(PercentDecode(enc) == ToUtf8(L"привет мир"));
    CHECK(PercentDecode("a%20b+c") == "a b c");
}

static void TestUrls() {
    std::printf("== urls ==\n");
    CHECK(NormalizeUrl(L"example.com") == L"https://example.com");
    CHECK(NormalizeUrl(L"http://a.com/x?y=1") == L"http://a.com/x?y=1");
    CHECK(NormalizeUrl(L"https://a.com") == L"https://a.com");
    CHECK(NormalizeUrl(L"ftp://a.com") .empty());
    CHECK(NormalizeUrl(L"").empty());
    CHECK(NormalizeUrl(L"  example.com  ") == L"https://example.com");

    // IDN: cyrillic domain is converted to punycode.
    const std::wstring idn = NormalizeUrl(L"пример.рф");
    CHECK(idn.rfind(L"https://xn--", 0) == 0);
    CHECK(idn.find(L'п') == std::wstring::npos);

    CHECK(IsUrlLike(L"example.com"));
    CHECK(IsUrlLike(L"www.site.ru"));
    CHECK(IsUrlLike(L"https://a.b/c"));
    CHECK(!IsUrlLike(L"hello world"));
    CHECK(!IsUrlLike(L"just words here"));
}

static void TestCacheMeta() {
    std::printf("== cache meta ==\n");
    CHECK(ReadMaxAge(L"max-age=3600") == 3600);
    CHECK(ReadMaxAge(L"public, max-age=60") == 60);
    CHECK(ReadMaxAge(L"no-store") == 0);
    CHECK(ReadMaxAge(L"no-cache") == 0);
    CHECK(ReadMaxAge(L"") == 60);
    CHECK(ReadMaxAge(L"max-age=99999999999999999999") <= 30ULL * 24 * 60 * 60);

    CacheMeta meta;
    meta.savedAt = UnixNow();
    meta.maxAge = 60;
    CHECK(CacheIsFresh(meta));
    meta.maxAge = 0;
    CHECK(!CacheIsFresh(meta));
    meta.maxAge = 1;
    meta.savedAt = UnixNow() - 100;
    CHECK(!CacheIsFresh(meta));
}

static void TestCacheRoundtrip() {
    std::printf("== cache roundtrip ==\n");
    (void)CacheDirPath();
    const std::wstring url = L"https://cache-test.example/page";

    Response out;
    out.status = 200;
    out.ok = true;
    const std::string bodyText = "cached-body-123";
    out.body.assign(bodyText.begin(), bodyText.end());
    out.contentType = L"text/html; charset=utf-8";

    CacheMeta meta;
    meta.savedAt = UnixNow();
    meta.maxAge = 3600;
    meta.etag = L"\"abc123\"";
    meta.contentType = out.contentType;

    SaveCache(url, out, meta);

    Response in;
    CacheMeta inMeta;
    CHECK(LoadCache(url, in, inMeta));
    CHECK(in.ok && in.fromCache);
    CHECK(in.status == 200);
    CHECK(std::string(in.body.begin(), in.body.end()) == "cached-body-123");
    CHECK(inMeta.etag == meta.etag);
    CHECK(inMeta.maxAge == 3600);

    RemoveCacheEntry(url);
    Response gone;
    CacheMeta goneMeta;
    CHECK(!LoadCache(url, gone, goneMeta));
}

static void TestCharset() {
    std::printf("== charset ==\n");
    size_t bom = 999;

    // UTF-8 BOM wins.
    const std::string bomUtf8 = "\xEF\xBB\xBF<html></html>";
    CHECK(DetectHtmlCharset(bomUtf8, L"", bom) == "utf-8");
    CHECK(bom == 3);

    // Content-Type charset beats meta.
    const std::string cp1251Bytes =
        "<html><head><meta charset=\"utf-8\"></head><body>"
        "\xCF\xF0\xE8\xE2\xE5\xF2</body></html>"; // "Привет" in cp1251
    CHECK(DetectHtmlCharset(cp1251Bytes,
                            L"text/html; charset=windows-1251", bom) ==
          "windows-1251");

    const std::string converted =
        ConvertHtmlToUtf8(cp1251Bytes, L"text/html; charset=windows-1251");
    CHECK(converted.find(ToUtf8(L"Привет")) != std::string::npos);

    // Plain UTF-8 stays UTF-8.
    const std::string utf8 = "<p>" + ToUtf8(L"Здравствуй") + "</p>";
    CHECK(ConvertHtmlToUtf8(utf8, L"text/html; charset=utf-8") == utf8);
}

static void TestHtmlParse() {
    std::printf("== html parse ==\n");
    const std::string fixture = ReadFile("tests/fixtures/simple.html");
    CHECK(!fixture.empty());
    if (fixture.empty()) return;

    const ParsedPage page = ParseHtml5(fixture, L"https://example.org/base/page");
    CHECK(page.title == ToUtf8(L"Тестовая & страница"));
    CHECK(page.text.find("■") != std::string::npos);
    CHECK(page.text.find("• пункт один") != std::string::npos);
    CHECK(page.text.find("Заголовок") != std::string::npos);
    CHECK(page.text.find("абзац") != std::string::npos);
    CHECK(page.text.find("пункт один") != std::string::npos);
    CHECK(page.text.find("script-text") == std::string::npos);
    CHECK(page.text.find("color: red") == std::string::npos);

    // Links: relative resolved, absolute kept, duplicates removed,
    // javascript:/mailto: rejected.
    CHECK(page.links.size() == 2);
    bool hasRel = false, hasAbs = false;
    for (const auto& link : page.links) {
        if (link.url == L"https://example.org/rel") hasRel = true;
        if (link.url == L"https://example.com/abs") hasAbs = true;
        CHECK(link.url.rfind(L"javascript:", 0) != 0);
        CHECK(link.url.rfind(L"mailto:", 0) != 0);
    }
    CHECK(hasRel);
    CHECK(hasAbs);

    // Malformed markup must not crash and must yield text.
    const std::string bad = ReadFile("tests/fixtures/malformed.html");
    CHECK(!bad.empty());
    if (!bad.empty()) {
        const ParsedPage broken = ParseHtml5(bad, L"https://example.org/");
        CHECK(broken.text.find("один") != std::string::npos);
        CHECK(broken.text.find("четыре") != std::string::npos);
        CHECK(broken.links.size() == 1);
        CHECK(broken.links[0].url == L"https://example.org/page2.html");
    }

    // Inline <img> becomes a \x02 marker line with the absolute URL and
    // also lands in the images list.
    const ParsedPage withImg = ParseHtml5(
        "<html><body><p>before<img src='/pic.png' alt='Cat'>after</p>"
        "</body></html>", L"https://example.org/dir/page");
    CHECK(withImg.text.find("\x02https://example.org/pic.png") !=
          std::string::npos);
    CHECK(withImg.images.size() == 1);
    CHECK(withImg.images[0].url == L"https://example.org/pic.png");
    CHECK(withImg.images[0].alt == "Cat");

    // Paragraph spacing: consecutive <p> blocks are separated by a
    // blank (\x01 verbatim) line.
    const ParsedPage paras = ParseHtml5(
        "<html><body><p>one</p><p>two</p></body></html>",
        L"https://example.org/");
    CHECK(paras.text.find("one\n\x01\ntwo") != std::string::npos);

    // Large page: 5000 paragraphs parse quickly and stay bounded.
    std::string big = "<html><body>";
    for (int i = 0; i < 5000; ++i)
        big += "<p>paragraph number " + std::to_string(i) + "</p>";
    big += "</body></html>";
    const auto start = Clock::now();
    const ParsedPage large = ParseHtml5(big, L"https://example.org/");
    const double ms = std::chrono::duration<double, std::milli>(
        Clock::now() - start).count();
    CHECK(large.text.find("paragraph number 4999") != std::string::npos);
    std::printf("  large page parsed in %.1f ms\n", ms);
    CHECK(ms < 10000.0);

    // Reader-mode filtering: nav/footer/cookie junk is removed, but an
    // article <header> containing an h1 is kept.
    const ParsedPage reader = ParseHtml5(
        "<html><body>"
        "<nav>menu junk <a href='/m'>menu link</a></nav>"
        "<div class='cookie-banner'>accept cookies</div>"
        "<article><header><h1>Article title</h1></header>"
        "<p>real content</p></article>"
        "<footer>site footer junk</footer>"
        "</body></html>", L"https://example.org/");
    CHECK(reader.text.find("Article title") != std::string::npos);
    CHECK(reader.text.find("real content") != std::string::npos);
    CHECK(reader.text.find("menu junk") == std::string::npos);
    CHECK(reader.text.find("accept cookies") == std::string::npos);
    CHECK(reader.text.find("site footer junk") == std::string::npos);
    CHECK(reader.links.empty()); // menu link filtered too

    // <pre> keeps indentation verbatim via \x01 line markers.
    const ParsedPage pre = ParseHtml5(
        "<html><body><pre>line1\n  indented\n\tdeep</pre><p>after</p>"
        "</body></html>", L"https://example.org/");
    CHECK(pre.text.find("\x01line1") != std::string::npos);
    CHECK(pre.text.find("\x01  indented") != std::string::npos);
    CHECK(pre.text.find("\x01\tdeep") != std::string::npos);
    CHECK(pre.text.find("after") != std::string::npos);

    // Nested lists get depth markers and nbsp indentation.
    const ParsedPage nested = ParseHtml5(
        "<html><body><ul><li>top<ul><li>nested</li></ul></li></ul>"
        "<blockquote>quote</blockquote><dl><dt>term</dt><dd>def</dd></dl>"
        "<hr><p>end</p></body></html>", L"https://example.org/");
    CHECK(nested.text.find("• top") != std::string::npos);
    CHECK(nested.text.find("◦ nested") != std::string::npos);
    CHECK(nested.text.find("▎ quote") != std::string::npos);
    CHECK(nested.text.find("▪ term") != std::string::npos);
    CHECK(nested.text.find("―――") != std::string::npos);
}

static void TestForms() {
    std::printf("== forms ==\n");
    const ParsedPage page = ParseHtml5(
        "<html><body>"
        "<form action='/search' method='get'>"
        "<label for='q'>Query</label>"
        "<input type='text' name='q' id='q' value='x'>"
        "<input type='password' name='pw'>"
        "<input type='hidden' name='h' value='hv'>"
        "<input type='checkbox' name='c' value='yes' checked>"
        "<input type='radio' name='r' value='a'>"
        "<input type='radio' name='r' value='b' checked>"
        "<select name='s'>"
        "<option value='1'>One</option>"
        "<option value='2' selected>Two</option>"
        "</select>"
        "<textarea name='t'>hello</textarea>"
        "<button type='submit' name='go' value='yes'>Go</button>"
        "<input type='submit' name='s2' value='Send'>"
        "<input type='reset'>"
        "<input type='text' name='dis' disabled>"
        "</form>"
        "<form method='post' action='https://api.example/submit'>"
        "<input name='a' value='1'></form>"
        "</body></html>", L"https://example.org/base/");

    CHECK(page.forms.size() == 2);
    if (page.forms.size() != 2) return;

    const FormDef& get = page.forms[0];
    CHECK(get.method == "get");
    CHECK(get.action == L"https://example.org/search"); // relative resolved

    const FormDef& post = page.forms[1];
    CHECK(post.method == "post");
    CHECK(post.action == L"https://api.example/submit");

    // 10 fields in form 1: q, pw, h, c, r, r, s, t, go, s2
    // (reset skipped, disabled skipped) + 1 in form 2 = 11 total.
    CHECK(page.fields.size() == 11);
    if (page.fields.size() != 11) return;

    const FormField& q = page.fields[0];
    CHECK(q.kind == FieldKind::Text);
    CHECK(q.name == "q");
    CHECK(q.value == "x");
    CHECK(q.label == "Query");       // resolved via <label for>
    CHECK(q.formIndex == 0);

    CHECK(page.fields[1].kind == FieldKind::Password);
    CHECK(page.fields[2].kind == FieldKind::Hidden);
    CHECK(page.fields[2].value == "hv");

    CHECK(page.fields[3].kind == FieldKind::Checkbox);
    CHECK(page.fields[3].checked);

    CHECK(page.fields[4].kind == FieldKind::Radio);
    CHECK(!page.fields[4].checked);
    CHECK(page.fields[5].kind == FieldKind::Radio);
    CHECK(page.fields[5].checked);

    const FormField& sel = page.fields[6];
    CHECK(sel.kind == FieldKind::Select);
    CHECK(sel.options.size() == 2);
    CHECK(sel.options[0] == "One");
    CHECK(sel.options[1] == "Two");
    CHECK(sel.optionValues.size() == 2);
    CHECK(sel.optionValues[0] == "1");
    CHECK(sel.optionValues[1] == "2");
    CHECK(sel.selected == 1);

    CHECK(page.fields[7].kind == FieldKind::Textarea);
    CHECK(page.fields[7].value == "hello");

    CHECK(page.fields[8].kind == FieldKind::Submit);
    CHECK(page.fields[8].name == "go");
    CHECK(page.fields[8].value == "yes");
    CHECK(page.fields[9].kind == FieldKind::Submit);
    CHECK(page.fields[9].value == "Send"); // from value attr, not button text

    // formIndex back-references must point at the right forms.
    CHECK(page.forms[0].fields.size() == 10);
    CHECK(page.forms[1].fields.size() == 1);
    CHECK(page.fields[9].formIndex == 0);
    CHECK(page.fields[10].formIndex == 1);

    // Buttons with type=button / fields outside <form> are ignored.
    const ParsedPage bare = ParseHtml5(
        "<html><body><input name='orphan' value='v'>"
        "<form><button type='button'>noop</button>"
        "<input name='in'></form></body></html>",
        L"https://example.org/");
    CHECK(bare.forms.size() == 1);
    CHECK(bare.fields.size() == 1);
    CHECK(bare.fields[0].name == "in");
    CHECK(bare.forms[0].action.empty()); // no action attr → page URL at submit
}

static void TestTabs() {
    std::printf("== tabs ==\n");
    const size_t initial = UiTabCount();
    CHECK(initial >= 1);
    const size_t first = UiActiveTabIndex();

    TabViewState view;
    view.scrollOffset = 5;
    view.selectedItem = 2;
    view.findMode = true;
    view.findQuery = L"needle";

    // New tab: becomes active; the passed view is saved into the tab
    // that was active before (per the API contract).
    TabViewState fresh = UiNewTab(view);
    CHECK(UiTabCount() == initial + 1);
    CHECK(UiActiveTabIndex() == UiTabCount() - 1);
    CHECK(fresh.selectedItem == -1);
    CHECK(UiTabTitle(UiActiveTabIndex()) == L"Новая вкладка");

    // Switching back to the previous tab restores its saved view.
    TabViewState restored = UiSwitchTabTo(first, fresh);
    CHECK(UiActiveTabIndex() == first);
    CHECK(restored.scrollOffset == 5);
    CHECK(restored.selectedItem == 2);
    CHECK(restored.findMode);
    CHECK(restored.findQuery == L"needle");

    // Cycle forward wraps around to the new tab.
    TabViewState cycled = UiCycleTab(+1, restored);
    CHECK(UiActiveTabIndex() == UiTabCount() - 1);
    CHECK(cycled.selectedItem == -1);

    // Close the active (last) tab → count back to initial, neighbor active.
    TabViewState after = UiCloseTab(cycled);
    CHECK(UiTabCount() == initial);
    CHECK(UiActiveTabIndex() == UiTabCount() - 1);
    CHECK(after.scrollOffset == 5); // previous tab's view came back

    // Closing down to a single tab resets it to a fresh "new tab" page.
    while (UiTabCount() > 1)
        UiCloseTab(UiSwitchTabTo(UiTabCount() - 1, after));
    CHECK(UiTabCount() == 1);
    UiCloseTab(after);
    CHECK(UiTabCount() == 1);
    CHECK(UiTabTitle(UiActiveTabIndex()) == L"Новая вкладка");
}

static void TestSuggestionsAndSave() {
    std::printf("== suggestions/save ==\n");
    (void)DataDirPath();

    {
        std::ofstream b(DataDirPath() / L"bookmarks.txt", std::ios::trunc);
        b << "https://bm-one.example/a\nhttps://other.example/\n";
    }
    {
        std::ofstream h(DataDirPath() / L"history.txt", std::ios::trunc);
        h << "1699999999\thttps://hist-alpha.example/x\n"
             "plain-old.example/\n";
    }

    // Bookmarks rank first, history matches follow, prefix < 2 rejected.
    const auto sug = GetUrlSuggestions(L"example", 8);
    CHECK(!sug.empty());
    CHECK(sug[0] == L"https://bm-one.example/a");
    bool hasHist = false;
    for (const auto& u : sug)
        if (u == L"https://hist-alpha.example/x") hasHist = true;
    CHECK(hasHist);
    CHECK(GetUrlSuggestions(L"a", 8).empty());
    CHECK(GetUrlSuggestions(L"zzz-no-such-host", 8).empty());

    // New history lines carry a "<unix>\t" prefix; legacy lines still parse.
    AppendHistory(L"https://ts-test.example/");
    {
        std::ifstream f(DataDirPath() / L"history.txt");
        std::string all((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
        CHECK(all.find('\t') != std::string::npos);
        CHECK(all.find("ts-test.example") != std::string::npos);
    }
    LoadHistory();
    bool navHas = false;
    for (const auto& u : g_navigation)
        if (u == L"https://ts-test.example/") navHas = true;
    CHECK(navHas);
    ClearHistory();
    ClearBookmarks();

    // Ctrl+S: saves raw HTML source next to a sanitized filename.
    g_currentUrl = L"https://save-test.example/page";
    g_currentTitle = "Save Test & <Co>";
    g_currentHtml = "<html><body>hi</body></html>";
    const std::wstring path = UiSavePage();
    CHECK(!path.empty());
    CHECK(path.size() >= 5 && path.substr(path.size() - 5) == L".html");
    CHECK(fs::exists(path));
    {
        std::ifstream f(path, std::ios::binary);
        std::string saved((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
        CHECK(saved.find("<body>hi</body>") != std::string::npos);
    }
    fs::remove(path);

    g_currentHtml.clear();
    g_currentUrl.clear();
    g_currentTitle.clear();
}

static void TestNormalizeText() {
    std::printf("== normalize text ==\n");
    CHECK(NormalizeDisplayText("a   b\tc") == "a b c");
    CHECK(NormalizeDisplayText("line1\nline2") == "line1\nline2");
    CHECK(NormalizeDisplayText("  padded  ") == "padded");
    CHECK(NormalizeDisplayText("") .empty());
}

static void TestSearchHelpers() {
    std::printf("== search helpers ==\n");

    // DuckDuckGo redirect wrapper is unwrapped to the real URL.
    const std::wstring ddg =
        L"https://duckduckgo.com/l/?uddg=" +
        ToWide(UrlEncode("https://example.com/page?a=1&b=2")) +
        L"&rut=abc";
    const std::wstring unwrapped = UnwrapSearchUrl(ddg);
    CHECK(unwrapped == L"https://example.com/page?a=1&b=2");

    // Google classic /url?q= wrapper.
    const std::wstring g =
        L"https://www.google.com/url?q=" +
        ToWide(UrlEncode("https://target.example/x")) + L"&sa=U";
    CHECK(UnwrapSearchUrl(g) == L"https://target.example/x");

    // Bing /ck/a base64url wrapper.
    const std::wstring bing =
        L"https://www.bing.com/ck/a?u=a1aHR0cHM6Ly9hLmIv&ntb=1";
    CHECK(UnwrapSearchUrl(bing) == L"https://a.b/");

    // Plain URL passes through.
    CHECK(UnwrapSearchUrl(L"https://plain.example/") == L"https://plain.example/");

    // Challenge detection.
    FetchOutcome normal;
    normal.finalUrl = L"https://html.duckduckgo.com/html/?q=test";
    normal.title = "test at DuckDuckGo";
    normal.text = "Result one\nResult two\nResult three";
    CHECK(!IsChallengePage(normal));

    FetchOutcome captcha;
    captcha.finalUrl = L"https://www.google.com/sorry/index";
    captcha.title = "unusual traffic";
    captcha.text = "Our systems have detected unusual traffic";
    CHECK(IsChallengePage(captcha));

    FetchOutcome empty;
    empty.finalUrl = L"https://example.com/";
    CHECK(IsChallengePage(empty)); // no text, no links => not results

    FetchOutcome ddgBot;
    ddgBot.finalUrl = L"https://html.duckduckgo.com/html/";
    ddgBot.title = "DuckDuckGo";
    ddgBot.text = "Unfortunately, bots use DuckDuckGo too. Please complete the following challenge.";
    CHECK(IsChallengePage(ddgBot));
}

static void TestBookmarksAndHistory() {
    std::printf("== bookmarks/history ==\n");
    (void)DataDirPath();

    {
        std::ofstream file(DataDirPath() / L"bookmarks.txt", std::ios::trunc);
        file << "https://one.example/\n"
                "https://two.example/path\n"
                "not a url at all :::\n";
    }
    const auto bookmarks = LoadBookmarks();
    CHECK(bookmarks.size() == 2);
    CHECK(bookmarks[0] == L"https://one.example/");

    ClearBookmarks();
    CHECK(LoadBookmarks().empty());

    AppendHistory(L"https://hist-one.example/");
    AppendHistory(L"https://hist-two.example/");
    {
        std::ifstream file(DataDirPath() / L"history.txt");
        std::string content((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
        CHECK(content.find("hist-one") != std::string::npos);
        CHECK(content.find("hist-two") != std::string::npos);
    }
    ClearHistory();
    {
        std::ifstream file(DataDirPath() / L"history.txt");
        std::string content((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
        CHECK(content.empty());
    }
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0); // unbuffered: see progress when hung
    std::printf("Browser unit tests\n");
    (void)CacheDirPath();
    (void)DataDirPath();
    (void)DownloadsDirPath();

    TestEncoding();
    TestUrls();
    TestCacheMeta();
    TestCacheRoundtrip();
    TestCharset();
    TestHtmlParse();
    TestForms();
    TestTabs();
    TestSuggestionsAndSave();
    TestNormalizeText();
    TestSearchHelpers();
    TestBookmarksAndHistory();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
