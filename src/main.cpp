#include "browser.h"
#include "wvhost.h"

#include <cstdio>
#include <ctime>
#include <exception>
#include <fstream>

namespace {

void LogCrash(const char* what, DWORD code = 0) {
    try {
        std::wofstream f(DataDirPath() / L"crash.log", std::ios::app);
        std::time_t now = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &now);
        wchar_t ts[32];
        swprintf_s(ts, L"%04d-%02d-%02d %02d:%02d:%02d",
                   tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                   tm.tm_hour, tm.tm_min, tm.tm_sec);
        f << ts << L"  " << ToWide(what ? what : "unknown");
        if (code) f << L"  code=0x" << std::hex << code;
        f << L"  url=" << g_currentUrl << L'\n';
    } catch (...) {}
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    LogCrash("unhandled exception",
             ep && ep->ExceptionRecord
                 ? ep->ExceptionRecord->ExceptionCode : 0);
    return EXCEPTION_EXECUTE_HANDLER;
}

int wmainImpl() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    SetConsoleTitleW(L"Browser - Console Search");
    g_useAnsi = ui::Enable();
    // User-data dirs live next to the executable (see util.cpp helpers).
    (void)CacheDirPath();
    (void)DownloadsDirPath();
    (void)DataDirPath();
    (void)SavedDirPath();

    if (!InitNetworkSession()) {
        std::cerr << "WinHttpOpen failed. Windows error: "
                  << GetLastError() << '\n';
        return 1;
    }

    LoadHistory();

    const HRESULT comInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninitializeCom = SUCCEEDED(comInit);

    auto shutdown = [&]() {
        wv::Shutdown();
        ShutdownAsyncLoads();
        g_loadCancel.store(true);
        CancelAllDownloads();
        if (g_downloadThread.joinable())
            g_downloadThread.join();
        ShutdownNetworkSession();
        if (shouldUninitializeCom)
            CoUninitialize();
    };

    if (console_ui::Run()) {
        shutdown();
        return 0;
    }

    // Fallback line mode for environments without a usable console
    // (redirected stdin/stdout, some CI hosts, etc.).
    std::cout << "Browser — console search\n"
              << "Enter a search phrase or website address. Type help for commands.\n\n";

    std::string line;
    while (true) {
        ui::Prompt();
        if (!std::getline(std::cin, line))
            break;

        std::istringstream input(line);
        std::string command;
        input >> command;

        if (!command.empty() &&
            std::all_of(command.begin(), command.end(),
                [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
            const std::string number = command;
            command = "open";
            input.clear();
            input.str(number);
        }

        if (command == "browser")
            input >> command;
        if (command.empty()) continue;

        if (command == "exit" || command == "quit")
            break;

        if (command == "help") {
            ShowHelp();
            continue;
        }

        if (command == "history") {
            ShowHistory();
            continue;
        }

        if (command == "clear-history") {
            ClearHistory();
            continue;
        }

        if (command == "back") {
            NavigateBack();
            continue;
        }

        if (command == "forward") {
            NavigateForward();
            continue;
        }

        if (command == "reload") {
            ReloadPage();
            continue;
        }

        if (command == "sync") {
            std::string action;
            std::getline(input, action);
            UiSync(ToWide(Trim(action)));
            continue;
        }

        if (command == "pause") {
            UiToggleDownloadPause();
            continue;
        }

        if (command == "stop") {
            StopDownload();
            continue;
        }

        if (command == "prefetch") {
            PrefetchLinks();
            continue;
        }

        if (command == "cache-clear") {
            ClearCache();
            continue;
        }

        if (command == "stats") {
            ShowStats();
            continue;
        }

        if (command == "version" || command == "ver") {
            std::cout << "Browser " << kAppVersion << " (console, Windows)\n";
            continue;
        }

        if (command == "downloads") {
            ShowDownloads();
            continue;
        }

        if (command == "export") {
            ExportBookmarksHtml();
            continue;
        }

        if (command == "import") {
            std::string path;
            std::getline(input, path);
            path = Trim(path);
            if (path.empty()) {
                std::cout << "Usage: import <bookmark-file.html>\n";
            } else {
                ImportBookmarksHtml(ToWide(path));
            }
            continue;
        }

        if (command == "history-clear-older") {
            std::string days;
            input >> days;
            uint64_t d = 0;
            try { d = std::stoull(days); } catch (...) { d = 0; }
            if (d == 0) {
                std::cout << "Usage: history-clear-older <days>\n";
            } else {
                ClearHistoryOlderThan(d);
            }
            continue;
        }

        if (command == "bookmarks") {
            ShowBookmarks();
            continue;
        }

        if (command == "bookmark") {
            std::string action, number;
            input >> action >> number;
            if (action == "add") {
                AddBookmark();
            } else if (action == "open") {
                const auto bookmarks = LoadBookmarks();
                size_t index = 0;
                try { index = std::stoul(number); } catch (...) { index = 0; }
                if (index == 0 || index > bookmarks.size()) {
                    std::cout << "Invalid bookmark number. Use: bookmarks\n";
                } else {
                    LoadPage(bookmarks[index - 1], CacheMode::Use, true, true);
                }
            } else {
                std::cout << "Usage: bookmark add | bookmark open <number>\n";
            }
            continue;
        }

        if (command == "images") {
            if (g_currentImages.empty()) {
                std::cout << "No images found on the current page.\n";
            } else {
                for (size_t i = 0; i < g_currentImages.size(); ++i) {
                    std::cout << '[' << i + 1 << "] "
                              << (g_currentImages[i].alt.empty()
                                  ? "(no alt text)" : g_currentImages[i].alt)
                              << " -> " << ToUtf8(g_currentImages[i].url) << '\n';
                }
            }
            continue;
        }

        if (command == "image") {
            std::string value;
            input >> value;
            size_t index = 0;
            try { index = std::stoul(value); } catch (...) { index = 0; }
            if (index == 0 || index > g_currentImages.size()) {
                std::cout << "Usage: image <number from images>\n";
            } else if (!RenderImageInConsole(g_currentImages[index - 1])) {
                std::cout << "Could not render this image in the console.\n";
            }
            continue;
        }

        if (command == "find") {
            std::string query;
            std::getline(input, query);
            FindText(Trim(query));
            continue;
        }

        if (command == "search") {
            std::string query;
            std::getline(input, query);
            SearchWeb(Trim(query));
            continue;
        }

        if (command == "ai") {
            std::string prompt;
            std::getline(input, prompt);
            prompt = Trim(prompt);
            if (prompt.empty()) {
                std::cout << "Usage: ai <question>   (add ' @' before the "
                             "question to include the current page)\n";
                continue;
            }
            bool withPage = prompt.rfind("@ ", 0) == 0;
            if (withPage) prompt = Trim(prompt.substr(2));
            const FetchOutcome out = AiAsk(prompt, withPage);
            if (out.ok) {
                g_currentText = out.text;
                g_currentTitle = out.title;
                g_currentUrl = out.finalUrl;
                std::cout << out.text << "\n";
            } else {
                std::cout << "AI error: "
                          << ToUtf8(out.error) << "\n";
            }
            continue;
        }

        if (command == "terminal" || command == "shell") {
            // Line-mode fallback: run a Windows command in this console.
            std::string c;
            std::getline(input, c);
            c = Trim(c);
            if (c.empty()) {
                std::cout << "Usage: terminal <command>\n";
            } else {
                std::system(c.c_str());
            }
            continue;
        }

        if (command == "open") {
            std::string target;
            input >> target;
            if (target.empty()) {
                std::cout << "Usage: open <url|number>\n";
                continue;
            }

            const bool isNumber = std::all_of(
                target.begin(), target.end(),
                [](unsigned char ch) { return std::isdigit(ch) != 0; });

            std::wstring url;
            if (isNumber) {
                size_t index = 0;
                try { index = std::stoul(target); }
                catch (...) { index = 0; }

                if (index == 0 || index > g_currentLinks.size()) {
                    std::cout << "Invalid link number.\n";
                    continue;
                }
                url = g_currentLinks[index - 1].url;
            } else {
                url = NormalizeUrl(ToWide(target));
                if (url.empty()) {
                    std::cout << "Invalid URL. Only HTTP and HTTPS are supported.\n";
                    continue;
                }
            }

            LoadPage(url, CacheMode::Use, true, true);
            continue;
        }

        if (command == "edge") {
            std::string target;
            std::getline(input, target);
            target = Trim(target);
            if (target.empty()) {
                std::cout << "Usage: edge <url>\n";
            } else {
                EdgePage(ToWide(target));
            }
            continue;
        }

        if (command == "web") {
            // web [url|back|forward|reload|zoom-*|find <t>|off] —
            // WebView2 (Chromium) window.
            std::string arg;
            std::getline(input, arg);
            arg = Trim(arg);
            std::string a = arg;
            std::transform(a.begin(), a.end(), a.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (a == "off" || a == "close" || a == "hide") {
                wv::Hide();
            } else if (a == "back") {
                wv::Back();
            } else if (a == "forward") {
                wv::Forward();
            } else if (a == "reload") {
                wv::Reload();
            } else if (a == "zoom-in" || a == "zoom+") {
                wv::ZoomIn();
            } else if (a == "zoom-out" || a == "zoom-") {
                wv::ZoomOut();
            } else if (a == "zoom-reset" || a == "zoom0") {
                wv::ZoomReset();
            } else if (a == "external" || a == "system") {
                wv::External();
            } else if (a == "find" || a.rfind("find ", 0) == 0) {
                const std::string needle = a == "find"
                    ? Trim(arg.substr(4)) : Trim(arg.substr(5));
                if (needle.empty()) {
                    std::cout << "Usage: web find <text>\n";
                } else {
                    wv::Find(ToWide(needle));
                }
            } else {
                std::wstring url;
                if (arg.empty()) {
                    url = g_currentUrl;
                } else if (IsUrlLike(ToWide(arg))) {
                    url = NormalizeUrl(ToWide(arg));
                } else {
                    url = L"https://html.duckduckgo.com/html/?q=" +
                          ToWide(UrlEncode(arg));
                }
                if (url.empty()) {
                    std::cout << "Usage: web [url]\n";
                } else {
                    wv::Show(url);
                }
            }
            continue;
        }

        if (command == "save") {
            const std::wstring path = UiSavePage();
            if (!path.empty())
                std::cout << "Saved: " << ToUtf8(path) << '\n';
            continue;
        }

        if (command == "download") {
            std::string target, filename;
            input >> target >> filename;
            if (target.empty()) {
                std::cout << "Usage: download <url> [filename]\n";
                continue;
            }

            const std::wstring url = NormalizeUrl(ToWide(target));
            if (url.empty()) {
                std::cout << "Invalid URL.\n";
                continue;
            }

            StartDownload(url, filename);
            continue;
        }

        // Anything else is treated as a search query or bare address.
        std::string rest;
        std::getline(input, rest);
        const std::string full = Trim(command + " " + rest);
        if (!full.empty()) {
            const std::wstring wide = ToWide(full);
            if (IsUrlLike(wide)) {
                const std::wstring url = NormalizeUrl(wide);
                if (!url.empty())
                    LoadPage(url, CacheMode::Use, true, true);
                else
                    std::cout << "Unknown command. Type help.\n";
            } else {
                SearchWeb(full);
            }
        }
    }

    shutdown();
    std::cout << "Browser closed.\n";
    return 0;
}

} // namespace

int wmain() {
    SetUnhandledExceptionFilter(CrashFilter);
    try {
        return wmainImpl();
    } catch (const std::exception& e) {
        LogCrash(e.what());
        throw;
    } catch (...) {
        LogCrash("unknown exception");
        throw;
    }
}
