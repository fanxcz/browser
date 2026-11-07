#include "browser.h"

int wmain() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    SetConsoleTitleW(L"Browser - Console Search");
    g_useAnsi = ui::Enable();
    // User-data dirs live next to the executable (see util.cpp helpers).
    (void)CacheDirPath();
    (void)DownloadsDirPath();
    (void)DataDirPath();
    (void)SavedDirPath();

    g_session = WinHttpOpen(
        USER_AGENT,
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);

    if (!g_session) {
        std::cerr << "WinHttpOpen failed. Windows error: "
                  << GetLastError() << '\n';
        return 1;
    }

    // One long-lived session lets WinHTTP reuse pooled connections.
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

    LoadHistory();

    const HRESULT comInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninitializeCom = SUCCEEDED(comInit);

    auto shutdown = [&]() {
        ShutdownAsyncLoads();
        g_loadCancel.store(true);
        CancelAllDownloads();
        if (g_downloadThread.joinable())
            g_downloadThread.join();
        if (g_session) {
            CloseConnectionPool();
            WinHttpCloseHandle(g_session);
            g_session = nullptr;
        }
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

        if (command == "post") {
            // post <url> <urlencoded-body> — debug form submissions.
            std::string target;
            input >> target;
            std::string body;
            std::getline(input, body);
            body = Trim(body);
            if (target.empty() || body.empty()) {
                std::cout << "Usage: post <url> <urlencoded-body>\n";
                continue;
            }
            const std::wstring url = NormalizeUrl(ToWide(target));
            if (url.empty()) {
                std::cout << "Invalid URL.\n";
                continue;
            }
            PostPage(url, body);
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
