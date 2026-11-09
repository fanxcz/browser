// ai.cpp — built-in AI assistant via Pollinations.ai.
// Free cloud text API, no registration and no API key required.
// Uses the plain-text GET endpoint so no JSON parsing is needed.

#include "browser.h"

namespace {

// GET a text endpoint; returns {} on any failure.
std::string HttpGetText(const std::wstring& url) {
    // Request() has its own timeouts; Bypass keeps answers out of cache.
    Response r = Request(url, CacheMode::Bypass, nullptr);
    if (!r.ok || r.body.empty()) return {};
    return std::string(r.body.begin(), r.body.end());
}

} // namespace

// Ask the AI. `prompt` is UTF-8; `withPage` prepends the current page text
// (capped) as context. Result commits as an internal browser://ai page.
FetchOutcome AiAsk(const std::string& prompt, bool withPage) {
    FetchOutcome out;
    out.finalUrl = std::wstring(kInternalPrefix) + L"ai";

    std::string full = prompt;
    if (withPage) {
        std::string ctx = g_currentText;
        if (ctx.size() > 10000) ctx.resize(10000);
        full = "Ниже текст веб-страницы. Ответь на вопрос, опираясь на него. "
               "Если в тексте нет ответа — скажи об этом честно.\n\n"
               "ТЕКСТ СТРАНИЦЫ:\n" + ctx + "\n\nВОПРОС: " + prompt;
    }
    if (full.empty()) {
        out.error = L"Пустой запрос к ИИ.";
        return out;
    }

    const std::wstring url = L"https://text.pollinations.ai/" +
        ToWide(UrlEncode(full));
    const std::string answer = HttpGetText(url);
    if (answer.empty()) {
        out.error = L"ИИ не ответил — проверьте интернет-соединение.";
        return out;
    }

    out.ok = true;
    out.title = "ИИ — Pollinations";
    out.text = NormalizeDisplayText(answer);
    return out;
}
