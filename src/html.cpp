#include "browser.h"

#include <array>
#include <cctype>
#include <cwchar>
#include <unordered_map>
#include <unordered_set>

// ---------------------------------------------------------------------------
// HTML5 parsing with Lexbor
// ---------------------------------------------------------------------------

static std::string GetAttr(lxb_dom_element_t* element, const char* name,
                           size_t nameLength);
static std::wstring ResolveAbsoluteUrl(const std::string& hrefUtf8,
                                       const std::wstring& baseUrl);

static bool IsBlockTag(const std::string& tag) {
    static const std::vector<std::string> tags = {
        "address", "article", "aside", "blockquote", "br", "dd", "div",
        "dl", "dt", "fieldset", "figcaption", "figure", "footer", "form",
        "h1", "h2", "h3", "h4", "h5", "h6", "header", "hr", "li",
        "main", "nav", "ol", "p", "pre", "section", "table", "tbody",
        "tr", "ul"
    };
    // td/th are intentionally NOT block: cells of a row are joined with " | ".
    return std::find(tags.begin(), tags.end(), tag) != tags.end();
}

static bool IsHeadingTag(const std::string& tag) {
    return tag.size() == 2 && tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6';
}

// Block elements that get a blank line after them — paragraph-like
// spacing instead of a wall of text. Lists, table parts and inline
// wrappers stay tight.
static bool IsParagraphTag(const std::string& tag) {
    static const std::vector<std::string> tags = {
        "address", "article", "aside", "blockquote", "div", "figcaption",
        "figure", "footer", "form", "h1", "h2", "h3", "h4", "h5", "h6",
        "header", "main", "p", "section"
    };
    return std::find(tags.begin(), tags.end(), tag) != tags.end();
}

static bool IsSkippedTag(const std::string& tag) {
    return tag == "script" || tag == "style" || tag == "noscript" ||
           tag == "template" || tag == "svg" || tag == "canvas" ||
           tag == "iframe" || tag == "object" || tag == "embed" ||
           tag == "nav" || tag == "aside" || tag == "select" ||
           tag == "datalist";
}

// header/footer are chrome only when they contain no heading — article
// headers often carry the page's h1 and must be kept.
static bool IsConditionalChromeTag(const std::string& tag) {
    return tag == "header" || tag == "footer";
}

static bool SubtreeHasHeading(lxb_dom_node_t* node, size_t depth = 0) {
    if (!node || depth > 64) return false;
    if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        auto* element = lxb_dom_interface_element(node);
        size_t tagLength = 0;
        const lxb_char_t* tagData =
            lxb_dom_element_local_name(element, &tagLength);
        if (tagData && IsHeadingTag(
                std::string(reinterpret_cast<const char*>(tagData), tagLength)))
            return true;
    }
    for (lxb_dom_node_t* child = node->first_child;
         child != nullptr; child = child->next) {
        if (SubtreeHasHeading(child, depth + 1))
            return true;
    }
    return false;
}

// Cookie-consent / newsletter / promo blocks identified by class or id.
static bool HasBoilerplateAttr(lxb_dom_element_t* element) {
    static const char* attrs[] = {"class", "id"};
    static const char* needles[] = {
        "cookie", "consent", "gdpr", "newsletter", "subscribe", "promo",
        // Wikipedia & common CMS chrome markers.
        "noprint", "p-lang", "catlinks", "mw-jump", "mw-editsection",
        "mw-panel", "mw-navigation", "sitesub", "contentsub", "mw-footer",
        "p-personal", "p-navigation", "vector-menu", "mw-indicators",
        // Wikipedia article chrome: infoboxes, navboxes, hatnotes,
        // cleanup templates, hidden metadata. ("toc" is matched as a
        // whole token below — as a substring it hits "stock", "protocol".)
        "infobox", "ambox", "hatnote", "navbox", "metadata",
        // Citation/reference blocks: <sup class="reference"> markers and
        // <ol class="references"> lists would flood the link list.
        "reference", "reflist", "portalbox", "sistersite"
    };
    // Whole-token matches for short, ambiguous class/id names.
    static const char* tokens[] = {"toc"};
    for (const char* attr : attrs) {
        size_t valueLength = 0;
        const lxb_char_t* value = lxb_dom_element_get_attribute(
            element,
            reinterpret_cast<const lxb_char_t*>(attr),
            std::char_traits<char>::length(attr), &valueLength);
        if (!value || valueLength == 0) continue;
        const std::string lowered = LowerAscii(
            std::string(reinterpret_cast<const char*>(value), valueLength));
        for (const char* needle : needles) {
            if (lowered.find(needle) != std::string::npos)
                return true;
        }
        // Whole-token match: split on common class/id separators.
        size_t pos = 0;
        while (pos <= lowered.size()) {
            const size_t end = lowered.find_first_of(" -_:", pos);
            const std::string token = lowered.substr(
                pos, end == std::string::npos ? end : end - pos);
            for (const char* want : tokens) {
                if (token == want)
                    return true;
            }
            if (end == std::string::npos) break;
            pos = end + 1;
        }
    }
    return false;
}

static std::string ElementTagName(lxb_dom_node_t* node) {
    if (!node || node->type != LXB_DOM_NODE_TYPE_ELEMENT) return {};
    auto* element = lxb_dom_interface_element(node);
    size_t tagLength = 0;
    const lxb_char_t* tagData =
        lxb_dom_element_local_name(element, &tagLength);
    return tagData
        ? std::string(reinterpret_cast<const char*>(tagData), tagLength)
        : std::string();
}

// Links/images inside navigation chrome or boilerplate blocks are noise.
static bool HasSkippedAncestor(lxb_dom_node_t* node) {
    for (lxb_dom_node_t* parent = node->parent;
         parent != nullptr && parent->type == LXB_DOM_NODE_TYPE_ELEMENT;
         parent = parent->parent) {
        const std::string tag = ElementTagName(parent);
        if (IsSkippedTag(tag) || IsConditionalChromeTag(tag) ||
            HasBoilerplateAttr(lxb_dom_interface_element(parent)))
            return true;
        if (tag == "body" || tag == "html")
            break;
    }
    return false;
}

// Collect raw text of a subtree without any normalization — used for <pre>.
static void AppendRawText(lxb_dom_node_t* node, std::string& output,
                          size_t depth = 0) {
    if (!node || depth > MAX_DOM_DEPTH) return;
    if (node->type == LXB_DOM_NODE_TYPE_TEXT ||
        node->type == LXB_DOM_NODE_TYPE_CDATA_SECTION) {
        auto* data = lxb_dom_interface_character_data(node);
        if (data->data.data && data->data.length) {
            output.append(reinterpret_cast<const char*>(data->data.data),
                          data->data.length);
        }
        return;
    }
    for (lxb_dom_node_t* child = node->first_child;
         child != nullptr; child = child->next)
        AppendRawText(child, output, depth + 1);
}

static void AppendDomTextNode(lxb_dom_node_t* node, std::string& output,
                              const std::wstring& baseUrl,
                              size_t depth = 0, int listDepth = 0) {
    if (!node || depth > MAX_DOM_DEPTH) return;

    if (node->type == LXB_DOM_NODE_TYPE_TEXT ||
        node->type == LXB_DOM_NODE_TYPE_CDATA_SECTION) {
        auto* data = lxb_dom_interface_character_data(node);
        if (data->data.data && data->data.length) {
            output.append(reinterpret_cast<const char*>(data->data.data),
                          data->data.length);
        }
        return;
    }

    if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        const std::string tag = ElementTagName(node);
        auto* element = lxb_dom_interface_element(node);

        if (IsSkippedTag(tag) || HasBoilerplateAttr(element))
            return;
        if (IsConditionalChromeTag(tag) && !SubtreeHasHeading(node))
            return;

        // <img>: emit a \x02 marker line with the absolute URL. The GUI
        // renders it as an inline image block; the console shows a
        // placeholder. data:/blob: URIs are skipped like in the image list.
        if (tag == "img") {
            std::string src = GetAttr(element, "src", 3);
            if (src.empty()) src = GetAttr(element, "data-src", 8);
            if (src.empty()) src = GetAttr(element, "data-original", 13);
            if (!src.empty() && src.rfind("data:", 0) != 0) {
                // Skip icons/favicons — they are chrome, not content, and
                // would flood the image section and inline markers.
                std::string probe = src;
                for (char& c : probe)
                    c = static_cast<char>(std::tolower(
                        static_cast<unsigned char>(c)));
                if (probe.find("favicon") == std::string::npos &&
                    probe.find(".ico") == std::string::npos) {
                    const std::wstring abs =
                        ResolveAbsoluteUrl(src, baseUrl);
                    if (!abs.empty())
                        output += "\n\x02" + ToUtf8(abs) + "\n";
                }
            }
            return;
        }

        // <pre>: emit each source line prefixed with \x01 so
        // NormalizeDisplayText keeps it verbatim (indentation preserved).
        if (tag == "pre") {
            std::string raw;
            AppendRawText(node, raw);
            output += "\n";
            size_t start = 0;
            for (size_t i = 0; i <= raw.size(); ++i) {
                if (i == raw.size() || raw[i] == '\n') {
                    std::string line = raw.substr(start, i - start);
                    if (!line.empty() && line.back() == '\r')
                        line.pop_back();
                    output += "\x01" + line + "\n";
                    start = i + 1;
                }
            }
            return;
        }

        const bool block = IsBlockTag(tag);
        if (block && !output.empty() && output.back() != '\n')
            output.push_back('\n');

        // Structural markers survive NormalizeDisplayText and let the console
        // UI color headings, lists and table rows like a reader view.
        // List indentation uses U+00A0 (nbsp): plain spaces after a newline
        // are collapsed by the normalizer, nbsp bytes pass through as chars.
        const int childListDepth = listDepth +
            ((tag == "ul" || tag == "ol") ? 1 : 0);
        if (IsHeadingTag(tag)) {
            // Blank line before the heading (\x01 = verbatim empty line).
            output += "\n\x01\n■ ";
        } else if (tag == "li") {
            static const char* marks[] = {"•", "◦", "▪"};
            const int level = std::clamp(listDepth, 1, 3);
            // Indent with U+00A0 (nbsp): plain spaces after a newline are
            // collapsed by the normalizer, nbsp passes through as a char.
            std::string pad;
            for (int i = 1; i < level; ++i) pad += "\xC2\xA0\xC2\xA0";
            output += "\n" + pad + marks[level - 1] + " ";
        } else if (tag == "dt") {
            output += "\n▪ ";
        } else if (tag == "dd") {
            output += "\n\xC2\xA0\xC2\xA0- ";
        } else if (tag == "blockquote") {
            output += "\n▎ ";
        } else if (tag == "hr") {
            output += "\n―――\n";
        } else if (tag == "tr") {
            if (!output.empty() && output.back() != '\n')
                output += "\n";
        } else if (tag == "td" || tag == "th") {
            if (!output.empty() && output.back() != '\n')
                output += " | ";
        }

        for (lxb_dom_node_t* child = node->first_child;
             child != nullptr; child = child->next) {
            AppendDomTextNode(child, output, baseUrl, depth + 1,
                              childListDepth);
        }

        if (block) {
            if (IsParagraphTag(tag)) {
                // Paragraph break: "\n\x01\n" yields one blank line and
                // collapses when a blank line was already emitted.
                const size_t n = output.size();
                const bool hasBreak =
                    (n >= 2 && output[n - 2] == '\x01' &&
                     output[n - 1] == '\n') ||
                    (n >= 3 && output.compare(n - 3, 3, "\n\x01\n") == 0) ||
                    (n >= 2 && output[n - 2] == '\n' &&
                     output[n - 1] == '\n');
                if (!output.empty() && !hasBreak) {
                    if (output.back() != '\n') output.push_back('\n');
                    output += "\x01\n";
                }
            } else if (output.empty() || output.back() != '\n') {
                output.push_back('\n');
            }
        }
        return;
    }

    for (lxb_dom_node_t* child = node->first_child;
         child != nullptr; child = child->next) {
        AppendDomTextNode(child, output, baseUrl, depth + 1, listDepth);
    }
}

static void AlignTableColumns(std::string& text);

std::string NormalizeDisplayText(const std::string& input) {
    std::string output;
    output.reserve(input.size());

    bool pendingSpace = false;
    bool pendingNewline = false;
    // \x01 starts a verbatim line (used for <pre>): everything up to and
    // including the next '\n' is copied unchanged — spaces and tabs survive.
    bool verbatim = false;

    for (size_t i = 0; i < input.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(input[i]);
        if (verbatim) {
            output.push_back(static_cast<char>(ch));
            if (ch == '\n') {
                verbatim = false;
                pendingNewline = false;
                pendingSpace = false;
            }
            continue;
        }
        if (ch == 0x01) {
            // "\x01\n" is an empty verbatim line = a requested blank line.
            // Collapse runs of them so paragraphs never produce stacks
            // of empty rows (also keeps intentional <pre> gaps to one).
            if (i + 1 < input.size() && input[i + 1] == '\n' &&
                output.size() >= 2 &&
                static_cast<unsigned char>(output[output.size() - 2]) ==
                    0x01 &&
                output.back() == '\n')
                continue;
            if (pendingNewline) {
                while (!output.empty() && output.back() == ' ')
                    output.pop_back();
                if (!output.empty() && output.back() != '\n')
                    output.push_back('\n');
            } else if (pendingSpace) {
                if (!output.empty() && output.back() != '\n' &&
                    output.back() != ' ')
                    output.push_back(' ');
            }
            pendingNewline = false;
            pendingSpace = false;
            output.push_back('\x01');
            verbatim = true;
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            pendingNewline = true;
            pendingSpace = false;
            continue;
        }

        if (ch == ' ' || ch == '\t' || ch == '\f') {
            if (!pendingNewline)
                pendingSpace = true;
            continue;
        }

        if (pendingNewline) {
            while (!output.empty() && output.back() == ' ')
                output.pop_back();
            if (!output.empty() && output.back() != '\n')
                output.push_back('\n');
            pendingNewline = false;
            pendingSpace = false;
        } else if (pendingSpace) {
            if (!output.empty() && output.back() != '\n' && output.back() != ' ')
                output.push_back(' ');
            pendingSpace = false;
        }

        output.push_back(static_cast<char>(ch));
    }

    while (!output.empty() &&
           (output.back() == ' ' || output.back() == '\n'))
        output.pop_back();

    AlignTableColumns(output);
    return output;
}

// Count UTF-8 codepoints (console cells ≈ codepoints for our fonts).
static size_t Utf8Len(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

// Align consecutive "a | b | c" lines into padded columns.
static void AlignTableColumns(std::string& text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < text.size()) {
        const size_t nl = text.find('\n', start);
        if (nl == std::string::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, nl - start));
        start = nl + 1;
    }
    if (lines.empty()) return;

    auto isRow = [](const std::string& l) {
        return l.size() > 3 && l.find(" | ") != std::string::npos &&
               l[0] != '\x01';
    };
    auto splitRow = [](const std::string& l) {
        std::vector<std::string> cells;
        size_t pos = 0;
        while (true) {
            const size_t p = l.find(" | ", pos);
            if (p == std::string::npos) {
                cells.push_back(l.substr(pos));
                break;
            }
            cells.push_back(l.substr(pos, p - pos));
            pos = p + 3;
        }
        return cells;
    };

    std::vector<std::string> outLines;
    outLines.reserve(lines.size());
    size_t i = 0;
    while (i < lines.size()) {
        if (!isRow(lines[i])) {
            outLines.push_back(lines[i]);
            ++i;
            continue;
        }
        size_t j = i;
        std::vector<std::vector<std::string>> rows;
        size_t cols = 0;
        while (j < lines.size() && isRow(lines[j])) {
            auto cells = splitRow(lines[j]);
            cols = std::max(cols, cells.size());
            rows.push_back(std::move(cells));
            ++j;
        }
        if (rows.size() < 2 || cols < 2 || cols > 10) {
            for (size_t k = i; k < j; ++k) outLines.push_back(lines[k]);
            i = j;
            continue;
        }
        std::vector<size_t> w(cols, 0);
        for (const auto& r : rows)
            for (size_t c = 0; c < r.size(); ++c)
                w[c] = std::min<size_t>(40, std::max(w[c], Utf8Len(r[c])));
        for (const auto& r : rows) {
            std::string line;
            for (size_t c = 0; c < r.size(); ++c) {
                if (c) line += " | ";
                line += r[c];
                if (c + 1 < r.size()) {
                    const size_t pad = w[c] > Utf8Len(r[c])
                        ? w[c] - Utf8Len(r[c]) : 0;
                    line.append(pad, ' ');
                }
            }
            // Trim trailing spaces.
            while (!line.empty() && line.back() == ' ') line.pop_back();
            outLines.push_back(std::move(line));
        }
        i = j;
    }

    text.clear();
    for (size_t k = 0; k < outLines.size(); ++k) {
        if (k) text += '\n';
        text += outLines[k];
    }
}

static std::string NodeText(lxb_dom_node_t* node) {
    if (!node) return {};
    size_t length = 0;
    lxb_char_t* value = lxb_dom_node_text_content(node, &length);
    if (!value || length == 0) return {};
    return std::string(reinterpret_cast<const char*>(value), length);
}

static std::string GetAttr(lxb_dom_element_t* element, const char* name,
                           size_t nameLength) {
    size_t valueLength = 0;
    const lxb_char_t* value = lxb_dom_element_get_attribute(
        element, reinterpret_cast<const lxb_char_t*>(name), nameLength,
        &valueLength);
    if (!value || valueLength == 0)
        return {};
    return std::string(reinterpret_cast<const char*>(value), valueLength);
}

static std::wstring ResolveAbsoluteUrl(const std::string& hrefUtf8,
                                       const std::wstring& baseUrl) {
    const std::wstring relative = ToWide(hrefUtf8);
    if (relative.empty())
        return {};

    wchar_t absolute[8192]{};
    DWORD absoluteLength = static_cast<DWORD>(std::size(absolute));
    const HRESULT combineResult = UrlCombineW(
        baseUrl.c_str(), relative.c_str(),
        absolute, &absoluteLength, 0);
    if (FAILED(combineResult))
        return {};

    return NormalizeUrl(absolute);
}

std::vector<Link> ExtractDomLinks(lxb_html_document_t* document,
                                  const std::wstring& baseUrl) {
    std::vector<Link> links;
    if (!document || !document->body)
        return links;

    lxb_dom_collection_t* collection =
        lxb_dom_collection_make(&document->dom_document, 128);
    if (!collection)
        return links;

    const lxb_status_t status = lxb_dom_elements_by_tag_name(
        lxb_dom_interface_element(document->body),
        collection,
        reinterpret_cast<const lxb_char_t*>("a"),
        1);

    if (status == LXB_STATUS_OK) {
        const size_t count = lxb_dom_collection_length(collection);
        std::unordered_set<std::wstring> seen;
        for (size_t i = 0; i < count && links.size() < MAX_PAGE_LINKS; ++i) {
            lxb_dom_element_t* element = lxb_dom_collection_element(collection, i);
            if (!element) continue;
            if (HasSkippedAncestor(lxb_dom_interface_node(element)))
                continue; // navigation menus, cookie banners, etc.

            const std::string hrefUtf8 = GetAttr(element, "href", 4);
            if (hrefUtf8.empty())
                continue;

            std::wstring finalUrl = ResolveAbsoluteUrl(hrefUtf8, baseUrl);
            if (finalUrl.empty())
                continue;

            if (!seen.insert(finalUrl).second)
                continue; // skip duplicate targets

            std::string label = NormalizeDisplayText(
                NodeText(lxb_dom_interface_node(element)));
            if (label.empty())
                label = finalUrl.size() > 100
                    ? ToUtf8(finalUrl.substr(0, 100))
                    : ToUtf8(finalUrl);

            links.push_back({std::move(label), std::move(finalUrl)});
        }
    }

    lxb_dom_collection_destroy(collection, true);
    return links;
}

std::vector<PageImage> ExtractDomImages(lxb_html_document_t* document,
                                        const std::wstring& baseUrl) {
    std::vector<PageImage> images;
    if (!document || !document->body)
        return images;

    lxb_dom_collection_t* collection =
        lxb_dom_collection_make(&document->dom_document, 64);
    if (!collection)
        return images;

    const lxb_status_t status = lxb_dom_elements_by_tag_name(
        lxb_dom_interface_element(document->body),
        collection,
        reinterpret_cast<const lxb_char_t*>("img"),
        3);

    if (status == LXB_STATUS_OK) {
        const size_t count = lxb_dom_collection_length(collection);

        for (size_t i = 0; i < count && images.size() < 12; ++i) {
            lxb_dom_element_t* element = lxb_dom_collection_element(collection, i);
            if (!element) continue;
            if (HasSkippedAncestor(lxb_dom_interface_node(element)))
                continue;

            std::string src = GetAttr(element, "src", 3);
            if (src.empty()) src = GetAttr(element, "data-src", 8);
            if (src.empty()) src = GetAttr(element, "data-original", 13);
            if (src.empty() || src.rfind("data:", 0) == 0)
                continue;

            std::wstring finalUrl = ResolveAbsoluteUrl(src, baseUrl);
            if (finalUrl.empty())
                continue;

            std::string alt = GetAttr(element, "alt", 3);
            images.push_back({std::move(finalUrl), std::move(alt)});
        }
    }

    lxb_dom_collection_destroy(collection, true);
    return images;
}

// ---------------------------------------------------------------------------
// Form extraction
// ---------------------------------------------------------------------------

static bool HasAttr(lxb_dom_element_t* element, const char* name) {
    return lxb_dom_element_has_attribute(
        element,
        reinterpret_cast<const lxb_char_t*>(name),
        std::char_traits<char>::length(name));
}

// Map of id -> label text for <label for="id"> associations.
static std::unordered_map<std::string, std::string>
CollectLabelForMap(lxb_html_document_t* document) {
    std::unordered_map<std::string, std::string> map;
    if (!document || !document->body)
        return map;

    lxb_dom_collection_t* collection =
        lxb_dom_collection_make(&document->dom_document, 64);
    if (!collection)
        return map;

    if (lxb_dom_elements_by_tag_name(
            lxb_dom_interface_element(document->body), collection,
            reinterpret_cast<const lxb_char_t*>("label"), 5) == LXB_STATUS_OK) {
        const size_t count = lxb_dom_collection_length(collection);
        for (size_t i = 0; i < count; ++i) {
            lxb_dom_element_t* element = lxb_dom_collection_element(collection, i);
            if (!element) continue;
            const std::string forId = GetAttr(element, "for", 3);
            if (forId.empty()) continue;
            std::string text = NormalizeDisplayText(
                NodeText(lxb_dom_interface_node(element)));
            if (!text.empty())
                map[forId] = std::move(text);
        }
    }
    lxb_dom_collection_destroy(collection, true);
    return map;
}

static FieldKind InputFieldKind(const std::string& type, bool& skip) {
    skip = false;
    if (type == "hidden") return FieldKind::Hidden;
    if (type == "checkbox") return FieldKind::Checkbox;
    if (type == "radio") return FieldKind::Radio;
    if (type == "submit" || type == "image") return FieldKind::Submit;
    if (type == "password") return FieldKind::Password;
    if (type == "reset" || type == "button" || type == "file") {
        skip = true;
        return FieldKind::Text;
    }
    return FieldKind::Text; // text/search/url/email/tel/number/date/...
}

static std::string ResolveFieldLabel(lxb_dom_element_t* element,
                                     const std::string& wrapLabel,
                                     const std::unordered_map<std::string, std::string>& forMap) {
    const std::string id = GetAttr(element, "id", 2);
    if (!id.empty()) {
        const auto it = forMap.find(id);
        if (it != forMap.end()) return it->second;
    }
    if (!wrapLabel.empty()) return wrapLabel;
    for (const char* attr : {"placeholder", "aria-label", "title"}) {
        const std::string v = GetAttr(element, attr,
            static_cast<size_t>(std::char_traits<char>::length(attr)));
        if (!v.empty()) return NormalizeDisplayText(v);
    }
    return {};
}

// Walk a <form> subtree collecting field elements in document order.
static void CollectFormFields(lxb_dom_node_t* node, ParsedPage& page,
                              int formIndex, int depth,
                              const std::string& wrapLabel,
                              const std::unordered_map<std::string, std::string>& forMap) {
    if (!node || depth > MAX_DOM_DEPTH ||
        page.fields.size() >= MAX_PAGE_FIELDS)
        return;

    if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        const std::string tag = ElementTagName(node);
        auto* element = lxb_dom_interface_element(node);

        // "select" is skipped for text extraction but IS a form field;
        // "datalist" stays skipped (its options are not submittable).
        const bool formSkipped = IsSkippedTag(tag) && tag != "select";
        if (formSkipped || HasBoilerplateAttr(element))
            return;

        // Descend into <label> carrying its text for the enclosed field.
        std::string childLabel = wrapLabel;
        if (tag == "label") {
            const std::string text = NormalizeDisplayText(
                NodeText(node));
            if (!text.empty()) childLabel = text;
        }

        const bool disabled = HasAttr(element, "disabled");
        if (!disabled && page.fields.size() < MAX_PAGE_FIELDS) {
            if (tag == "input") {
                const std::string type = LowerAscii(GetAttr(element, "type", 4));
                bool skip = false;
                const FieldKind kind = InputFieldKind(type, skip);
                if (!skip) {
                    FormField field;
                    field.kind = kind;
                    field.name = GetAttr(element, "name", 4);
                    field.value = GetAttr(element, "value", 5);
                    field.checked = HasAttr(element, "checked");
                    field.label = ResolveFieldLabel(element, childLabel, forMap);
                    field.formIndex = formIndex;
                    page.fields.push_back(std::move(field));
                    page.forms[static_cast<size_t>(formIndex)].fields.push_back(
                        static_cast<int>(page.fields.size()) - 1);
                }
            } else if (tag == "textarea") {
                FormField field;
                field.kind = FieldKind::Textarea;
                field.name = GetAttr(element, "name", 4);
                field.value = NormalizeDisplayText(NodeText(node));
                field.label = ResolveFieldLabel(element, childLabel, forMap);
                field.formIndex = formIndex;
                page.fields.push_back(std::move(field));
                page.forms[static_cast<size_t>(formIndex)].fields.push_back(
                    static_cast<int>(page.fields.size()) - 1);
            } else if (tag == "select") {
                if (!HasAttr(element, "multiple")) {
                    FormField field;
                    field.kind = FieldKind::Select;
                    field.name = GetAttr(element, "name", 4);
                    field.label = ResolveFieldLabel(element, childLabel, forMap);
                    field.formIndex = formIndex;
                    // Collect <option> children.
                    for (lxb_dom_node_t* child = node->first_child;
                         child != nullptr; child = child->next) {
                        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
                        if (ElementTagName(child) != "option") continue;
                        auto* optEl = lxb_dom_interface_element(child);
                        field.options.push_back(NormalizeDisplayText(
                            NodeText(child)));
                        // Per HTML spec the submitted value is the value
                        // attribute when present, else the option text.
                        const std::string optValue =
                            GetAttr(optEl, "value", 5);
                        field.optionValues.push_back(optValue.empty()
                            ? field.options.back() : optValue);
                        if (HasAttr(optEl, "selected"))
                            field.selected = field.options.size() - 1;
                        if (field.options.size() >= 100) break;
                    }
                    if (!field.options.empty()) {
                        page.fields.push_back(std::move(field));
                        page.forms[static_cast<size_t>(formIndex)].fields.push_back(
                            static_cast<int>(page.fields.size()) - 1);
                    }
                }
            } else if (tag == "button") {
                const std::string type = LowerAscii(GetAttr(element, "type", 4));
                if (type.empty() || type == "submit") {
                    FormField field;
                    field.kind = FieldKind::Submit;
                    field.name = GetAttr(element, "name", 4);
                    field.value = GetAttr(element, "value", 5);
                    if (field.value.empty())
                        field.value = NormalizeDisplayText(NodeText(node));
                    field.label = ResolveFieldLabel(element, childLabel, forMap);
                    field.formIndex = formIndex;
                    page.fields.push_back(std::move(field));
                    page.forms[static_cast<size_t>(formIndex)].fields.push_back(
                        static_cast<int>(page.fields.size()) - 1);
                }
            }
        }

        for (lxb_dom_node_t* child = node->first_child;
             child != nullptr; child = child->next) {
            // <select> options are handled above; don't recurse into them
            // as fields (option isn't a field tag anyway).
            CollectFormFields(child, page, formIndex, depth + 1,
                              childLabel, forMap);
        }
        return;
    }

    for (lxb_dom_node_t* child = node->first_child;
         child != nullptr; child = child->next) {
        CollectFormFields(child, page, formIndex, depth + 1,
                          wrapLabel, forMap);
    }
}

static void ExtractDomForms(lxb_html_document_t* document,
                            const std::wstring& pageUrl,
                            ParsedPage& page) {
    if (!document || !document->body)
        return;

    lxb_dom_collection_t* collection =
        lxb_dom_collection_make(&document->dom_document, 32);
    if (!collection)
        return;

    const lxb_status_t status = lxb_dom_elements_by_tag_name(
        lxb_dom_interface_element(document->body),
        collection,
        reinterpret_cast<const lxb_char_t*>("form"), 4);

    if (status == LXB_STATUS_OK) {
        const size_t count = lxb_dom_collection_length(collection);
        const auto forMap = CollectLabelForMap(document);

        for (size_t i = 0; i < count && page.forms.size() < MAX_PAGE_FORMS; ++i) {
            lxb_dom_element_t* element = lxb_dom_collection_element(collection, i);
            if (!element) continue;
            if (HasSkippedAncestor(lxb_dom_interface_node(element)) ||
                HasBoilerplateAttr(element))
                continue;

            FormDef form;
            const std::string method = LowerAscii(GetAttr(element, "method", 6));
            form.method = (method == "post") ? "post" : "get";
            const std::string actionAttr = GetAttr(element, "action", 6);
            if (!actionAttr.empty()) {
                form.action = ResolveAbsoluteUrl(actionAttr, pageUrl);
                // javascript:/mailto: actions are rejected by NormalizeUrl.
            }

            const int formIndex = static_cast<int>(page.forms.size());
            const size_t fieldsBefore = page.fields.size();
            page.forms.push_back(std::move(form));

            CollectFormFields(lxb_dom_interface_node(element), page,
                              formIndex, 0, {}, forMap);

            // Drop empty forms (e.g. search widgets with JS-only fields).
            if (page.fields.size() == fieldsBefore)
                page.forms.pop_back();
        }
    }

    lxb_dom_collection_destroy(collection, true);
}

static std::string ExtractTitle(lxb_html_document_t* document) {
    if (!document || !document->head)
        return {};

    lxb_dom_collection_t* collection =
        lxb_dom_collection_make(&document->dom_document, 2);
    if (!collection)
        return {};

    std::string title;
    const lxb_status_t status = lxb_dom_elements_by_tag_name(
        lxb_dom_interface_element(document->head),
        collection,
        reinterpret_cast<const lxb_char_t*>("title"),
        5);

    if (status == LXB_STATUS_OK &&
        lxb_dom_collection_length(collection) > 0) {
        lxb_dom_element_t* element = lxb_dom_collection_element(collection, 0);
        if (element) {
            title = NormalizeDisplayText(
                NodeText(lxb_dom_interface_node(element)));
        }
    }

    lxb_dom_collection_destroy(collection, true);
    return title;
}

// ---------------------------------------------------------------------------
// Encoding conversion
// ---------------------------------------------------------------------------

// Lexbor's HTML parser consumes UTF-8. Network responses may be UTF-8,
// Windows-1251, UTF-16, or another legacy encoding. Convert before parsing.
static void AppendUtf8Codepoint(std::string& output, uint32_t cp) {
    if (cp <= 0x7F) {
        output.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        output.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        output.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF && !(cp >= 0xD800 && cp <= 0xDFFF)) {
        output.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        output.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        output.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        output.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        AppendUtf8Codepoint(output, 0xFFFD);
    }
}

std::string DetectHtmlCharset(const std::string& html,
                              const std::wstring& contentType,
                              size_t& bomLength) {
    bomLength = 0;

    // A BOM has priority over HTTP and in-document declarations.
    if (html.size() >= 3 &&
        static_cast<unsigned char>(html[0]) == 0xEF &&
        static_cast<unsigned char>(html[1]) == 0xBB &&
        static_cast<unsigned char>(html[2]) == 0xBF) {
        bomLength = 3;
        return "utf-8";
    }

    if (html.size() >= 2 &&
        static_cast<unsigned char>(html[0]) == 0xFF &&
        static_cast<unsigned char>(html[1]) == 0xFE) {
        bomLength = 2;
        return "utf-16le";
    }

    if (html.size() >= 2 &&
        static_cast<unsigned char>(html[0]) == 0xFE &&
        static_cast<unsigned char>(html[1]) == 0xFF) {
        bomLength = 2;
        return "utf-16be";
    }

    // HTTP Content-Type charset overrides an HTML meta declaration.
    const std::string header = ToUtf8(contentType);
    const std::string lowerHeader = LowerAscii(header);
    std::string charset;
    {
        const size_t pos = lowerHeader.find("charset");
        if (pos != std::string::npos) {
            size_t value = pos + 7;
            while (value < header.size() &&
                   (header[value] == ' ' || header[value] == '\t'))
                ++value;
            if (value < header.size() && header[value] == '=') {
                ++value;
                while (value < header.size() &&
                       (header[value] == ' ' || header[value] == '\t'))
                    ++value;
                char quote = 0;
                if (value < header.size() &&
                    (header[value] == '"' || header[value] == '\'')) {
                    quote = header[value++];
                }
                const size_t start = value;
                while (value < header.size()) {
                    const char ch = header[value];
                    if ((quote && ch == quote) ||
                        (!quote && (ch == ';' || ch == ' ' || ch == '\t' ||
                                    ch == '\r' || ch == '\n')))
                        break;
                    ++value;
                }
                if (start < value)
                    charset = header.substr(start, value - start);
            }
        }
    }
    if (!charset.empty())
        return charset;

    // Fall back to the HTML5 meta-charset scanner.
    lxb_html_encoding_t* detector = lxb_html_encoding_create();
    if (detector != nullptr) {
        const lxb_status_t initStatus = lxb_html_encoding_init(detector);
        if (initStatus == LXB_STATUS_OK && !html.empty()) {
            const auto* bytes =
                reinterpret_cast<const lxb_char_t*>(html.data());
            const lxb_status_t detectStatus =
                lxb_html_encoding_determine(detector, bytes, bytes + html.size());

            if (detectStatus == LXB_STATUS_OK &&
                lxb_html_encoding_meta_length(detector) > 0) {
                lxb_html_encoding_entry_t* entry =
                    lxb_html_encoding_meta_entry(detector, 0);

                if (entry != nullptr && entry->end > entry->name) {
                    charset.assign(
                        reinterpret_cast<const char*>(entry->name),
                        static_cast<size_t>(entry->end - entry->name));
                }
            }
        }

        lxb_html_encoding_destroy(detector, true);
    }

    // Modern pages overwhelmingly use UTF-8; use it when no declaration exists.
    return charset.empty() ? "utf-8" : charset;
}

std::string ConvertHtmlToUtf8(const std::string& html,
                              const std::wstring& contentType) {
    if (html.empty())
        return {};

    size_t bomLength = 0;
    const std::string charset = DetectHtmlCharset(html, contentType, bomLength);

    const lxb_encoding_data_t* encoding =
        lxb_encoding_data_by_pre_name(
            reinterpret_cast<const lxb_char_t*>(charset.data()),
            charset.size());

    if (encoding == nullptr) {
        // Unknown charset label: keep data as UTF-8 rather than guessing a code page.
        return html.substr(bomLength);
    }

    lxb_encoding_decode_t decoder{};
    if (lxb_encoding_decode_init_single(&decoder, encoding) != LXB_STATUS_OK)
        return html.substr(bomLength);

    const auto* input = reinterpret_cast<const lxb_char_t*>(
        html.data() + bomLength);
    const auto* inputEnd = reinterpret_cast<const lxb_char_t*>(
        html.data() + html.size());

    std::string utf8;
    utf8.reserve(html.size() + 16);

    while (input < inputEnd) {
        const lxb_char_t* previous = input;
        const lxb_codepoint_t cp =
            encoding->decode_single(&decoder, &input, inputEnd);

        if (cp > LXB_ENCODING_DECODE_MAX_CODEPOINT) {
            AppendUtf8Codepoint(utf8, 0xFFFD);

            // Lexbor leaves the pointer on an invalid byte. Advance to avoid a loop.
            if (input == previous)
                ++input;
        } else {
            AppendUtf8Codepoint(utf8, static_cast<uint32_t>(cp));
        }
    }

    lxb_encoding_decode_finish_single(&decoder);
    return utf8;
}

// Pull content="..." out of a <meta …> tag source.
static std::string ExtractMetaContent(const std::string& tagSource) {
    const std::string low = LowerAscii(tagSource);
    const size_t cp = low.find("content=");
    if (cp == std::string::npos) return {};
    size_t v = cp + 8;
    char quote = 0;
    if (v < tagSource.size() &&
        (tagSource[v] == '"' || tagSource[v] == '\''))
        quote = tagSource[v++];
    size_t e = v;
    while (e < tagSource.size() &&
           (quote ? tagSource[e] != quote
                  : tagSource[e] != '>' && tagSource[e] != ' '))
        ++e;
    return tagSource.substr(v, e - v);
}

// Parse "5; url=http://…" refresh content.
static void ParseRefreshContent(const std::string& content,
                                int& seconds, std::string& url) {
    std::string c = content;
    const size_t semi = c.find(';');
    if (semi != std::string::npos) {
        const std::string head = c.substr(0, semi);
        try { seconds = std::stoi(head); } catch (...) { seconds = 0; }
        if (seconds < 0) seconds = 0;
        if (seconds > 3600) seconds = 3600;   // cap runaway refreshes
        c = c.substr(semi + 1);
    } else {
        try { seconds = std::stoi(c); } catch (...) { seconds = 0; }
        if (seconds < 0 || seconds > 3600) { seconds = 0; return; }
        return;  // bare number = reload
    }
    const size_t up = LowerAscii(c).find("url=");
    if (up == std::string::npos) return;
    std::string u = c.substr(up + 4);
    // Trim quotes/space.
    size_t b = u.find_first_not_of(" \t\"'");
    size_t e = u.find_last_not_of(" \t\"'");
    if (b == std::string::npos) return;
    url = u.substr(b, e - b + 1);
}

ParsedPage ParseHtml5(const std::string& html, const std::wstring& url) {
    ParsedPage result;

    lxb_html_document_t* document = lxb_html_document_create();
    if (!document)
        return result;

    const lxb_status_t status = lxb_html_document_parse(
        document,
        reinterpret_cast<const lxb_char_t*>(html.data()),
        html.size());

    if (status == LXB_STATUS_OK) {
        // <meta http-equiv="refresh" content="N; url=..."> — cheap scan of
        // the head area only (first 64 KB is plenty for meta tags).
        {
            const size_t scanLen = std::min<size_t>(html.size(), 65536);
            const std::string probe = LowerAscii(html.substr(0, scanLen));
            size_t mp = probe.find("http-equiv");
            while (mp != std::string::npos && mp < scanLen) {
                const size_t tagEnd = probe.find('>', mp);
                if (tagEnd == std::string::npos) break;
                // Find the enclosing <meta …> start.
                const size_t tagStart = probe.rfind("<meta", mp);
                if (tagStart != std::string::npos &&
                    tagStart < mp &&
                    probe.find("refresh", tagStart) < tagEnd) {
                    const std::string content =
                        ExtractMetaContent(html.substr(tagStart,
                                                       tagEnd - tagStart));
                    if (!content.empty()) {
                        ParseRefreshContent(content,
                                            result.refreshSeconds,
                                            result.refreshUrl);
                    }
                    break;  // first refresh meta wins
                }
                mp = probe.find("http-equiv", tagEnd);
            }
        }
        result.title = ExtractTitle(document);

        if (document->body) {
            std::string rawText;
            AppendDomTextNode(lxb_dom_interface_node(document->body),
                              rawText, url);
            result.text = NormalizeDisplayText(rawText);
        }

        result.links = ExtractDomLinks(document, url);
        result.images = ExtractDomImages(document, url);
        ExtractDomForms(document, url, result);
    }

    lxb_html_document_destroy(document);
    return result;
}
