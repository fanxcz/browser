// feeds.cpp — RSS 2.0 / Atom / RDF feed detection and reader rendering.
// Split out of app.cpp.

#include "browser.h"

#include <cstring>

namespace {

// Case-insensitive substring search.
size_t FindCi(const std::string& hay, const std::string& needle,
              size_t from) {
    if (needle.empty() || from >= hay.size()) return std::string::npos;
    const std::string low = LowerAscii(hay.substr(from));
    const size_t p = low.find(needle);
    return p == std::string::npos ? p : from + p;
}

// Text content of the first <tag>…</tag> inside block (CDATA unwrapped).
std::string XmlTagText(const std::string& block, const char* tag) {
    const std::string open = std::string("<") + tag;
    size_t p = FindCi(block, open, 0);
    if (p == std::string::npos) return {};
    p = block.find('>', p);
    if (p == std::string::npos) return {};
    const std::string close = std::string("</") + tag;
    size_t e = FindCi(block, close, p + 1);
    if (e == std::string::npos) e = block.size();
    std::string text = block.substr(p + 1, e - p - 1);
    // Unwrap CDATA and decode the common entities.
    const std::string cdataOpen = "<![CDATA[";
    if (text.find(cdataOpen) != std::string::npos) {
        size_t cs = text.find(cdataOpen);
        size_t ce = text.find("]]>", cs);
        if (ce != std::string::npos)
            text = text.substr(cs + cdataOpen.size(),
                               ce - cs - cdataOpen.size());
    }
    auto rep = [](std::string& s, const char* a, const char* b) {
        size_t pos = 0;
        const size_t alen = std::strlen(a);
        while ((pos = s.find(a, pos)) != std::string::npos) {
            s.replace(pos, alen, b);
            pos += std::strlen(b);
        }
    };
    rep(text, "&amp;", "&"); rep(text, "&lt;", "<");
    rep(text, "&gt;", ">"); rep(text, "&quot;", "\"");
    rep(text, "&#39;", "'"); rep(text, "&apos;", "'");
    // Collapse whitespace.
    std::string out;
    out.reserve(text.size());
    bool ws = false;
    for (char c : text) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            ws = true;
        } else {
            if (ws && !out.empty()) out.push_back(' ');
            ws = false;
            out.push_back(c);
        }
    }
    return Trim(out);
}

} // namespace

bool LooksLikeFeed(const std::string& html) {
    if (html.size() < 200) return false;
    const std::string probe = LowerAscii(html.substr(0, 8192));
    return probe.find("<rss") != std::string::npos ||
           probe.find("<feed") != std::string::npos ||
           probe.find("<rdf:rdf") != std::string::npos ||
           (probe.find("<channel") != std::string::npos &&
            probe.find("<item") != std::string::npos);
}

FetchOutcome BuildFeedPage(const std::wstring& url,
                           const std::string& xml) {
    FetchOutcome out;
    out.ok = true;
    out.finalUrl = url;
    out.isFeed = true;
    out.title = "Лента";

    // Feed/channel title.
    const std::string feedTitle =
        XmlTagText(xml.substr(0, std::min<size_t>(xml.size(), 65536)),
                   "title");
    if (!feedTitle.empty()) out.title = feedTitle;

    struct Item { std::string title, link, date, desc; };
    std::vector<Item> items;
    size_t pos = 0;
    while (items.size() < 100) {
        // Next <item> (RSS/RDF) or <entry> (Atom), whichever comes first.
        size_t pi = FindCi(xml, "<item", pos);
        size_t pe = FindCi(xml, "<entry", pos);
        size_t start = std::min(pi, pe);
        if (start == std::string::npos) break;
        // End of this item: matching close tag or next item start.
        const char* closeTag = xml[start + 1] == 'i' ||
            (xml[start + 1] == 'I') ? "</item" : "</entry";
        size_t next = std::min(FindCi(xml, "<item", start + 5),
                               FindCi(xml, "<entry", start + 6));
        size_t end = FindCi(xml, closeTag, start + 5);
        if (end == std::string::npos || (next != std::string::npos && next < end))
            end = next;
        if (end == std::string::npos) end = xml.size();
        const std::string block = xml.substr(start, end - start);
        pos = end;

        Item it;
        it.title = XmlTagText(block, "title");
        it.link = XmlTagText(block, "link");
        if (it.link.empty()) {
            // Atom: <link href="..."/> or <link ...>text</link>.
            const std::string low = LowerAscii(block);
            const size_t lp = low.find("<link");
            if (lp != std::string::npos) {
                const size_t gt = block.find('>', lp);
                if (gt != std::string::npos) {
                    const std::string attrs = LowerAscii(
                        block.substr(lp, gt - lp));
                    const size_t hp = attrs.find("href=");
                    if (hp != std::string::npos) {
                        // Map the attrs offset back into the original block.
                        size_t v = hp + 5;
                        char quote = 0;
                        if (v < attrs.size() &&
                            (attrs[v] == '"' || attrs[v] == '\''))
                            quote = attrs[v++];
                        const size_t abs_v = lp + v;
                        size_t ae = abs_v;
                        while (ae < block.size() &&
                               (quote ? block[ae] != quote
                                      : block[ae] != ' ' &&
                                        block[ae] != '>'))
                            ++ae;
                        it.link = block.substr(abs_v, ae - abs_v);
                    }
                }
            }
            if (it.link.empty()) it.link = XmlTagText(block, "id");
        }
        it.date = XmlTagText(block, "pubDate");
        if (it.date.empty()) it.date = XmlTagText(block, "published");
        if (it.date.empty()) it.date = XmlTagText(block, "updated");
        if (it.date.empty()) it.date = XmlTagText(block, "dc:date");
        it.desc = XmlTagText(block, "description");
        if (it.desc.empty()) it.desc = XmlTagText(block, "summary");
        if (it.desc.size() > 160)
            it.desc = it.desc.substr(0, 157) + "...";
        if (it.title.empty() && it.link.empty()) continue;
        items.push_back(std::move(it));
    }

    std::string text;
    for (size_t i = 0; i < items.size(); ++i) {
        const Item& it = items[i];
        text += std::to_string(i + 1) + ". " +
                (it.title.empty() ? it.link : it.title) + "\n";
        if (!it.date.empty()) text += "   " + it.date + "\n";
        if (!it.desc.empty()) text += "   " + it.desc + "\n";
        if (!it.link.empty()) text += "   " + it.link + "\n";
        const std::wstring abs = ResolveRedirectUrl(url, ToWide(it.link));
        if (!abs.empty())
            out.links.push_back(
                {std::to_string(i + 1) + ". " +
                     (it.title.empty() ? it.link : it.title), abs});
        text += "\n";
    }
    if (items.empty())
        text = "Лента не содержит элементов.";
    out.text = NormalizeDisplayText(text);
    return out;
}
