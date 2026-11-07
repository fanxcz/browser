# Changelog

## 1.1 — Лёгкая оконная версия (BrowserGui.exe)

- Новый target `BrowserGui` (Win32, без консоли): то же окно «как терминал» — тёмная палитра, Consolas, полоса вкладок, навигационные кнопки, омнибокс с автодополнением, контент страниц с кликабельными ссылками/полями/картинками.
- Полностью переиспользует `browser_core`: загрузка, вкладки, история, закладки, формы, загрузки, RSS, headless Edge, Gist-синхронизация — всё то же самое.
- Управление с клавиатуры — тот же keymap, что в консольной версии (`j/k`, `Enter`, `o`, `Ctrl+T/W/J/H/D/B/S/F`, `d`, `t`, `g/G`, `/`, `Esc`-иерархия). Мышь опциональна: клик по ссылкам/вкладкам/кнопкам, наведение, колесо.
- Дополнительно: `Ctrl+Shift+L` переключает тёмную/светлую палитру GUI, мигающий курсор в омнибоксе, статус-строка с прогрессом загрузки, восстановление вкладок при запуске.

## 1.0 — Первый публичный релиз

- Публичная версия: `Browser/1.0` в User-Agent, баннер `v1.0`, VERSIONINFO 1.0.0.0.
- Весь функционал из записей 4.x–5.x ниже — история разработки до публикации.

## 5.1 — Modular source, Gist sync, download pause

- **Source split for maintainability**: `app.cpp` (~3600 lines) was split into focused translation units — `storage.cpp` (history/bookmarks/export/import/internal pages), `feeds.cpp` (RSS/Atom rendering), `downloads.cpp` (queue, parallel workers, resume, SHA-256, downloads page) and `sync.cpp` (Gist sync). Shared declarations live in `browser.h`; largest file is now ~2100 lines.
- **GitHub Gist sync**: `sync push` / `sync pull` / `sync status` in the omnibox (and fallback mode) sync history, bookmarks, tabs and settings through a secret Gist via the `gh` CLI. Gist id is stored in `data/settings.txt` (`gist=<id>`); missing `gh`/auth produces a clear status message. No credentials are stored by the browser itself.
- **Download pause**: `pause` omnibox command or `p` while downloads run pauses the queue (in-flight files finish); press again to resume.
- **Help updated**: `help` now lists `edge`, `save`, `export`/`import`, `history-clear-older`, `pause`, `sync` commands.
- Verified: Debug+Release builds clean after the split, 171 checks 0 failures.

## 5.0 — Feeds, meta refresh, tables, headless Edge, themes

- **RSS/Atom/RDF feeds**: pages whose source looks like a feed (`<rss>`, `<feed>`, `<rdf:RDF>`, `<channel>`+`<item>`) are rendered as a readable item list — title, date, summary, clickable article links (relative links resolved against the feed URL). CDATA and common XML entities are decoded; malformed feeds degrade gracefully.
- **`<meta http-equiv="refresh">`**: parsed from the document head (seconds capped at 3600, refresh loop guarded by the load-running check). Bare `content="N"` reloads the page; `url=` navigates. Works per tab via the normal page-commit path.
- **Aligned tables**: `<tr>` now starts a new line and consecutive `a | b | c` rows are column-aligned by UTF-8 width (≤10 columns, ≤40 chars/cell) in `NormalizeDisplayText`. Single " | " occurrences in prose are untouched.
- **Headless Edge rendering**: pages that fetch successfully but look like JS-app shells (almost no readable text + ≥3 `<script>` tags or a noscript/"enable javascript" notice) automatically retry through `msedge --headless --dump-dom` (JS runs up to 8 s virtual time, 30 s wall timeout, dedicated `data/edge-profile/`). Manual triggers: omnibox `edge <url>`, `Ctrl+Shift+E` on the current page, `edge <url>` in fallback mode. Graceful error when Edge is not installed.
- **Light theme**: `Ctrl+Shift+L` toggles a light palette (dark text on gray cells); persisted as `theme=light|dark` in `data/settings.txt` alongside `downloads=`.
- **Download filenames**: `Content-Disposition: attachment; filename=…` (and `filename*=`) is preferred over the URL-derived name; traversal/unsafe characters are still rejected.
- **`Ctrl+V`** pastes clipboard text (Unicode) into the omnibox at the caret.
- **Custom download directory**: `downloads=<path>` in `data/settings.txt` redirects all download/list/verify/open-folder operations.
- **History tools**: `clear-history-older <days>` prunes dated entries; `export`/`import <file>` round-trip history+bookmarks through a Netscape-style HTML file.
- **Session restore**: per-tab scroll offset and `browser://downloads` tabs are persisted and restored; closed-tab stack unchanged.
- **Per-download cancel**: `x` on the downloads page cancels the selected in-flight job (`.part` kept for resume) without touching other downloads.
- Verified: Debug+Release builds clean, 171 checks 0 failures; live-tested BBC RSS feed (renders item list) and table alignment on a local page.

## 4.9 — Downloads as a real tab, parallel downloads, Ctrl+C

- **`browser://downloads` is now a real tab**: `Ctrl+J` and the `downloads` omnibox command go through the normal `LoadJob` pipeline (`Kind::Internal`), so the page gets a navigation entry — `Alt+Left` returns to the previous page, it works in background tabs, and tab state is committed like any other page. Page content is built by `BuildDownloadsPage()` shared with the instant `UiShowDownloadsPage()` path (used by `r` reload).
- **Parallel downloads**: up to 3 files download concurrently via a dispatcher thread + per-download cancel/progress state (`ActiveDownload`). `stop` now cancels ALL active downloads and clears the queue; the status line sums progress across active files (`⇩ N MB (+K в очереди)`).
- **`Ctrl+C`** copies the selected link/image URL to the clipboard (page URL when nothing is selected).
- Verified: 5 queued downloads → 3 ran in parallel, all completed (one httpbin 429 = server rate limit); fresh download + SHA-256 OK on Release; 171 checks Debug+Release.

## 4.8 — Download queue and downloads-folder access

- **Download queue**: `d`/`download` no longer rejects a second file while one is running — jobs queue up and drain sequentially on the worker thread. Status shows "В очереди загрузок (N ждёт): name", the live status line shows `⇩ size (+N в очереди)`, and the downloads page lists pending names. `stop` cancels the active download and reports how many remain queued.
- **`e` on the downloads page** opens the downloads folder in Explorer; `t`/`d` on a finished file opens it instead of trying to re-download.
- Downloads page status now shows the queue summary alongside the key hints.

## 4.7 — Download resume, file opening, external browser

- **Download resume (Range)**: interrupting a download (`stop`/`Esc`) keeps the `.part` file; repeating `download <url>` sends `Range: bytes=N-` and continues where it stopped (HTTP 206). Servers that ignore Range get a clean restart (200). A completed `.part` promoted on HTTP 416. Resumed downloads use a dedicated WinHTTP session without transparent decompression — gzip-encoded 206 responses can't be inflated mid-stream and previously aborted the read. The read loop now uses `WinHttpReadData` directly (works around `QueryDataAvailable` failing on some 206 responses).
- **Open downloaded files**: `Ctrl+J` downloads page entries are clickable — Enter/click opens the file with the shell's default handler; `v` verifies it against a local `.sha256.txt`/`.sha256` sidecar (records `✓` into `data/verified.txt`, shown as `✓ sha256` in the list).
- **Ctrl+Shift+O** opens the current page in the system default browser — fallback for JS-heavy sites the console renderer can't display.
- Verified live: seeded 200 KB `.part` + resume → 476,875-byte file, SHA-256 OK; fresh downloads unaffected; 171 checks Debug+Release.

## 4.6 — Download verification and full file-type coverage

- SHA-256 sidecar verification: after a download completes, the browser fetches `<url>.sha256.txt` / `.sha256` (when published) and verifies the file via BCrypt — reports `SHA-256 OK` or `MISMATCH` in the status line / fallback output.
- File-link detection extended to ~60 extensions (.vsix, .appimage, .jar, .xpi, .crx, .aab, .run, .app, .msu, .sys, .cab, .chm, .epub, .cbz, .sketch, .fig, .xd added) — Enter/click on any of them downloads directly.
- Verified all 10 distributables on fanxcz.github.io/nexus-lang/downloads.html end-to-end (~590 MB total): sizes match Content-Length and every SHA-256 matches the published checksum.

## 4.5 — Download UX fixes

- Download messages no longer scribble raw text over the console UI: status line and completion reports go through `g_consoleStatus` in UI mode (`⇩ N MB` live progress already shown).
- `StartDownload` normalizes the URL and rejects non-http(s) targets with a clear status instead of a silent network failure.
- `d` with no item selected downloads the current page URL — covers the "open a file link, get 'cannot display', then download" flow. Pages with undisplayable binary content now hint "нажмите d, чтобы скачать".
- `Ctrl+J` downloads page + `r` refresh shows live `.part` growth.
- `download <url> [name]` and `stop` now work as omnibox commands in the console UI (previously only in fallback mode).
- Enter/click on a link pointing to a file (.zip/.exe/.pdf/.mp4/etc.) now downloads it directly instead of trying to render it as a page.

## 4.4 — Downloads in the browser UI and exe-relative data dirs

- Fixed downloads "not landing in downloads/": all user-data paths (`cache/`, `data/`, `downloads/`, `saved/`) are now resolved next to Browser.exe instead of the process working directory, so files always land in the same place regardless of how the app is launched.
- In-browser downloads: `Ctrl+J` (or `downloads` in the omnibox) opens a downloads page listing files newest-first with sizes; `.part` files mark in-progress downloads. The status line shows live download progress (`⇩ N MB`) while a download runs. `r` refreshes the page.
- Download retry: the download worker now retries once on a fresh connection when a pooled keep-alive connection turns out dead.
- Tests updated for exe-relative paths; 171 checks in Debug and Release.

## 4.3 — Stability fix and extra shortcuts

- Fixed intermittent load failures: pooled keep-alive connections that the server had silently closed now trigger one automatic retry on a fresh connection instead of an error page. Applied to both page loads and file downloads.
- Verified end-to-end: page loads (HTTP 200) and downloads (1 MB file with progress) work through the pooled connection path.
- `t` opens the selected link/image in a new tab; `d` downloads it to `downloads/`; `y` copies the page URL to the clipboard.
- `Ctrl+Shift+T` reopens the last closed tab (up to 8 remembered, full page state restored).
- Tests: 171 checks in Debug and Release.

## 4.2 — Image viewer and embedded shell

- In-console image viewer: Enter/click on an «ИЗОБРАЖЕНИЯ» item opens a full-area truecolor preview rendered with WIC-decoded half-block cells (ANSI 24-bit). `D`/`S` downloads the image to `downloads/`; Enter/Esc/click closes.
- Embedded terminal: `Ctrl+\`` launches `cmd.exe` inside the same console window; `exit` returns to the browser UI.
- Refactored WIC decoding into `DecodeImagePixels` shared by the fallback `image N` command and the new viewer.
- Tests: 171 checks in Debug and Release.

## 4.1 — Daily-driver improvements

- Address-bar autocomplete: up to 8 suggestions from history + bookmarks while typing; Up/Down highlight, Tab/Enter accepts, mouse click works; caches file lists by mtime.
- Bookmarks on the home page: up to 6 clickable shortcuts under the search box.
- Ctrl+S saves the current page: raw HTML source to `saved/<title>.html` (reader text as `.txt` fallback); `save` command in fallback mode.
- Session restore: open tab URLs persist to `data/tabs.txt` and reopen on next launch; background tabs lazy-load on first activation.
- History timestamps: entries store visit time (`<unix>\t<url>`, legacy lines still parse); history page and fallback output show relative times.
- Loading progress: status line shows received bytes; WinHTTP connections are pooled per host (keep-alive, up to 4 per host) with dead-connection eviction.
- Editing a form field now scrolls it into view; mouse clicks on form fields already worked via the shared item hit-test.
- Tests: 171 checks in Debug and Release.

## 4.0 — Forms and tabs

- Added HTML form extraction: input (text/password/hidden/checkbox/radio/submit), textarea, select (with option values), button; labels via `<label for>`/wrapping, relative `action` resolution, GET and POST (`application/x-www-form-urlencoded`).
- POST support in WinHTTP layer: request body, content type, no page-cache for POST, redirects 307/308 keep POST while 301/302/303 degrade to GET.
- Form UI: fields listed under «ПОЛЯ ФОРМЫ», Enter edits text fields in the omnibox, Enter toggles checkbox/radio/select/submit, Ctrl+Enter submits the containing form from any field; radio groups uncheck siblings; only the activated submit button contributes name/value.
- Browser tabs: per-tab page state, links, images, fields, forms, back/forward stack, scroll and selection; Ctrl+T/W/Tab/1-9 shortcuts, clickable tab strip with close buttons and "+" new-tab; middle-click closes a tab; max 12 tabs.
- Async loads commit into the tab that started them, so background loads can't overwrite the visible page.
- Fallback mode: new `post <url> <body>` command for debugging submissions.
- Tests: 159 checks (form extraction, tab lifecycle) in Debug and Release.

## 3.4 — Console navigation and usability

- Added hover highlighting for links and navigation controls.
- Made each result row a full-width mouse target, not just the link text.
- Added clickable Back, Forward, Home and Reload controls in the page header.
- Added Tab focus switching between the search field and page links.
- Added F6, Ctrl+R, F5, Alt+Left/Right, Backspace navigation and Ctrl+D bookmarks.
- Added search/address input history on Up/Down.
- Improved long-query editing with horizontal caret-following input view.
- Improved wheel scrolling for multi-notch and high-resolution wheel events.
- Added Home/End page scrolling and click-to-position caret in the search box.

## 3.3.2 — Search fallback correction

- DuckDuckGo HTML is now the first search provider, followed by Bing and Google.
- Detects Google's Russian and English help/feedback interstitial as a non-result page.
- Search skips challenge and service pages and continues to the next provider.

## 3.3.1 — HTML search fallback

- Added fallback from Google to DuckDuckGo HTML and Bing when a JavaScript interstitial or bot-check page is returned.

## 3.3 — Console-only

- Removed graphical mode, the `gui` command, WebView2 SDK, WebView2 runtime requirement and GUI source files.
- `Browser.exe` starts directly in the console search interface.
- Simplified the home screen and page header to a compact search-first layout.
- Added scrolling with the mouse wheel, PageUp and PageDown.
- Retained mouse-clickable links and keyboard link selection.
- CMake downloads only Lexbor for HTML5 parsing.
