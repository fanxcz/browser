# Changelog

## 1.8 — Целостность обновлений, поиск по истории

- **Проверка обновления по sha256**: `update apply` теперь скачивает ассет `.sha256` из релиза и сверяет хэш exe до подмены — битый или подменённый файл не установится. CI при создании релиза по тегу генерирует `Browser.exe.sha256` автоматически.
- **Self-check exe при запуске**: `data/selfhash.txt` хранит SHA-256 Browser.exe; при несовпадении (повреждённый файл, частичное обновление) показывается предупреждение в статус-баре. После `update apply` база сбрасывается.
- **`history <запрос>` / `hist <запрос>`** — страница истории с фильтром по URL.
- **Ротация бэкапов**: `data/backups/` хранит не больше 14 последних ежедневных снимков.

## 1.7 — Скорость, куки, HAR, галерея

- **Виртуализация рендера**: `pageLines` (до ~3000 строк с ссылками/полями/картинками) больше не пересобирается каждый кадр — кэшируется и перестраивается только при смене контента (fingerprint по тексту, ссылкам, полям, картинкам, ширине).
- **Persistent-выделение**: выделение мышью «прикрепляется» к строкам страницы — при прокрутке прямоугольник конвертируется в диапазон строк и следует за текстом, как в Firefox, а не съезжает на чужой контент.
- **Cookie jar до RFC-уровня**: `data/cookies.txt` теперь хранит Domain/Path/Secure/Expires/Max-Age — Secure-куки не уходят по http, host-only куки не утекают на поддомены, Path= фильтрует по префиксу пути.
- **HAR-экспорт**: `netlog har` пишет `saved/netlog-<дата>.har` (HAR 1.2) — открывается в DevTools любого браузера. netlog теперь хранит unix-timestamp.
- **`queue done <n>`** — отметить прочитанным: убирает из очереди и пишет в историю.
- **`browser://sync` diff**: показывает не только счётчики, но и сами новые записи («Новое на gist» — до 20 URL/строк).
- **`browser://gallery`** — галерея тем: локальные паки + онлайн-индекс `themes/index.json` с подсказкой `theme pull`.
- **`stats`**: топ-10 сайтов за 7 дней по истории.
- **Статус-бар**: бейдж очереди чтения (обновляется раз в 2 с).
- **Тесты**: +21 юнит-проверка (merge строк, даты лент, slug, cookie matching) — всего 203/0; механики 58/0.
- **CI**: при пуше тега `v*` собирается релиз автоматически, release notes берутся из верхней секции CHANGELOG.

## 1.6 — Экосистема: синхронизация, правила, очередь, devtools

- **Синхронизация через Gist — merge**: в gist теперь уходят история, закладки, заметки, RSS-ленты и очередь чтения (7 секций). При `sync pull` mergeable-секции **объединяются** с локальными (union по URL/строкам), а не перезаписываются — две машины больше не затирают данные друг друга. `browser://sync` (`syncpage`) показывает построчный состав локальных данных и содержимого gist.
- **Очередь чтения**: `queue add <url>` / `queue next` / `queue del <n>` / `queue clear` / `browser://queue` — «прочитать позже» в консоли, хранится в `data/queue.txt`.
- **RSS-дайджест**: `digest` — все подписки на одной странице, свежие записи первыми, записи старше 7 дней отфильтровываются. Парсер лент вынесен в общий `ParseFeedItems`.
- **Декларативные правила**: `data/rules.txt` и `data/rules/*.txt` — строки `<часть-url>⇥hide <текст>` / `⇥replace <от> => <до>` / `⇥theme <имя>`; применяются к тексту страницы до отображения, без JS-движка. `browser://rules` показывает активные правила.
- **Темы по сайтам**: `data/sites/<хост>.theme` с именем темы — страница сайта открывается в своей теме, при уходе возвращается сохранённая.
- **Макросы**: `data/macros.txt` — `имя = команда ; команда`; `macro <имя>` выполняет шаги через обычный диспетчер команд.
- **Офлайн-википедия**: `wsave <статья>` сохраняет статью в `wiki/<slug>.txt`, `wiki` — указатель сохранённых, `woffline <имя>` — открыть без сети.
- **Экспорт в Markdown**: `export-md <дни>` — до 15 свежих страниц из истории скачиваются и складываются в `saved/articles/<дата>/*.md` + `index.md` (готово для Obsidian).
- **Windows-интеграция**: `register-protocol` — протокол `browser://` в реестре (HKCU), `browser://example.com` из других программ открывает сайт; `fileassoc` — пункт «Открыть в Browser» в контекстном меню `.html`; `browser://file/<path>` — просмотр локальных html-файлов; toast-уведомления Windows о завершении загрузок (вместо/вместе с beep).
- **DevTools-lite**: `browser://headers` — заголовки последнего ответа, `browser://cookies` — cookie-файл (`data/cookies.txt`, Set-Cookie собирается автоматически и отправляется с запросами), `browser://netlog` + `netlog clear` — журнал всех запросов (время, метод, URL, статус, байты, мс).
- **Галерея тем**: `theme pull <имя>` — скачать `themes/<имя>.json` с сайта проекта; в репо добавлены `moonlight` и `solarized-light`.
- **CI**: GitHub Action собирает все цели, гоняет юнит-тесты (`ctest`) и полный механики-сценарий (`browser_harness` + `tests/mechanics.txt`, теперь 58 проверок).
- **Механики**: +14 проверок экосистемы — очередь, дайджест, headers/cookies/netlog/rules/wiki/sync-страницы, export-md, макросы.

## 1.5 — Командная палитра, настройки, RSS-агрегатор

- **Командная палитра `Ctrl+K`**: fuzzy-поиск по командам, истории omnibox, закладкам и открытым вкладкам в одном оверлее; ↑↓ выбор, Enter — выполнить/открыть, Esc — закрыть. Команды ранжируются выше остального.
- **`browser://settings`**: страница настроек — все тумблеры (тема, zoom, картинки, префетч, статус-бар, офлайн, поисковик, домашняя) и значения в одном месте.
- **RSS-агрегатор**: `feed add <url>` / `feed del <n>` / `browser://feeds` — подписки на ленты в одном списке.
- **Автообновление**: `update` проверяет свежий релиз на GitHub, `update apply` скачивает exe, сохраняет бэкап и перезапускается.
- **`--safe-mode`**: запуск без восстановления сессии, кэша и фоновых задач — для отладки.
- **Мышь**: боковые кнопки X1/X2 — назад/вперёд; prefetch по наведению курсора на ссылку.
- **Темы**: `theme save <имя>` — сохранить текущую палитру как именованную тему.
- **Статусы**: цветные ✓/✗/⚠ в статус-баре, индикатор загрузки на вкладке.
- **Исправлено**: отложенный клик (~250 мс) больше не отбирает фокус у адресной строки — устранена гонка «Ctrl+L → сразу печатаю».

## 1.4 — Модульный UI и пакет из ~30 фишек

- **Рефакторинг интерфейса**: монолитный `console_ui.cpp` разбит на модули — `ui_theme`, `ui_primitives`, `ui_select`, `ui_render`, `ui_actions`, `ui_commands`, `ui_input`, `ui_mouse`, `ui_main` + общий `ui_int.h`.
- **Вкладки**: `tab <n>` — переход по номеру, `tabname <имя>` / `rename` — своё имя, `pin`/`unpin` — закрепление (не закрывается случайно и не трогается close-others), `close-left`/`close-right`, `tabmove <n>`, `dup` — дубликат, `reopen` — вернуть закрытую.
- **Навигация**: `Ctrl+Enter` в адресной строке открывает в новой вкладке, `engine <ddg|bing|google>` — выбор поисковика, `dl <url>` — быстрая загрузка, `up` — на уровень выше, `hard-reload` — мимо кэша, `offline on|off` — только кэш, `reload <сек>` — автообновление.
- **Контент**: `images` / `linkpage` — страницы всех картинок и ссылок, `saveimg` — сохранить картинку под курсором, `imglinks` — URL всех картинок, `copy url|domain|title|text|links|images|selection`, `source <url>` — исходник чужой страницы, `find <текст>` + `find next|prev|case`.
- **Отображение**: `theme <имя>` — именованные темы, `zoom in|out|<%>` — масштаб текста, `statusbar on|off`, `noimg`/`images off` — без картинок, `prefetch off` — без упреждения, `fullscreen`/`F11`.
- **Данные**: `export`/`import` — бэкап и восстановление данных, `note del <n>` / `notes clear`, `lucky` — случайная страница, `snapshot` — текстовый снимок экрана в `saved/shots/`, `sessions` / `restore <имя>` — именованные сессии вкладок.
- **Статус-бар**: процент прокрутки и счётчик выделения.
- **Тесты**: механики расширены до 44 живых проверок (вкладки, команды фич, копирование, find, snapshot, sessions) — все зелёные; юнит-тесты 182/0.

## 1.3 — Полировка UI, новые страницы, чистка команд

- **Терминальное выделение**: drag мышью по любому тексту экрана (ссылки, статус-бар, вкладки) — прямоугольное выделение как в cmd; `Ctrl+C` копирует видимый текст. Двойной клик — слово, тройной — строка, auto-scroll у краёв при drag, `Esc` сбрасывает.
- **Deferred click**: одиночный клик диспатчится через ~250 мс — double/triple-click больше не активируют ссылки; клик в другом месте мгновенно «проталкивает» отложенный.
- **Diff-рендер**: кадр сравнивается с предыдущим, на консоль пишутся только изменившиеся строки.
- **Новые внутренние страницы**: `browser://about`, `notes`, `cache`, `ip`, `define/<слово>`, `tr/<текст>` + команды `note`, `define`/`dict`, `tr`/`translate`, `w`/`wiki`, `title`, `links`, `mdlink`, `scroll <1-20>`, `home <url>`, `feedback`/`repo`/`site`.
- **Статус-бар**: часы HH:MM, цветные статусы (ошибка/загрузка/успех), прогресс-бар загрузки, beep по завершении закачек, уведомление об обновлении GitHub-релиза.
- **Главная**: строка «недавно посещали» (top-3 из истории) под закладками.
- **Тестовый харнесс** (`tools/harness.cpp` + `tests/mechanics.txt`): подключается к живому Browser.exe, шлёт клавиши/мышь, читает экран и буфер, делает скриншоты — 21 живая проверка механик.
- **Удалено**: встроенный терминал (`browser://terminal`, `Ctrl+\``, `!cmd`), утилиты-игрушки (`calc`, `pass`, `uuid`, `timer`, `words`/`readtime`) — команды, не связанные с браузингом.

## 1.2 — WebView2-окно и офлайн-Wikipedia

- **`web`-команда / WebView2-хост** (`src/wvhost.cpp`): отдельное окно на движке Edge/Chromium с настоящим рендером HTML/CSS/JS. Команды: `web <url>`, `web back|forward|reload`, `web zoom-in|zoom-out|zoom-reset`, `web find <текст>`, `web external`, `web off`. В окне работают горячие клавиши: `F5`/`Ctrl+R` — обновить, `Alt+←/→` — история, `Ctrl+=/-/0` — зум, `Ctrl+F` — поиск на странице, `Ctrl+Shift+E` — открыть в системном браузере.
- **Загрузки из WebView2**: `DownloadStarting` → файлы в `downloads/`, имя из `Content-Disposition`/URL, защита от коллизий имён, статус в консоли.
- **Офлайн-библиотека Wikipedia** (`src/wiki.cpp`): `wiki download <ru|en> [часть|all|N]` — резюмируемая закачка статей через MediaWiki API (только текст, UTF-8); `часть` ≈ 50 000 статей (~200 МБ), `all` — вся энциклопедия. Прогресс сохраняется в `.progress`, `wiki stop` отменяет, повторный запуск продолжает с места остановки.
- **Чтение и поиск офлайн**: сгенерированные HTML-страницы отдаются в WebView2-окно через виртуальный хост `https://wiki.local/`; `wiki portal` — каталог со счётчиками и живым поиском по заголовкам (`articles.js`), `wiki search <текст>` / `wiki <запрос>` — поиск по заголовкам и текстам (`wiki grep`), статьи открываются прямо в окне.
- История посещений wiki-страниц пишется в общую `history.txt`.

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
