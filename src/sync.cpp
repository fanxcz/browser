// sync.cpp — cloud sync of history/bookmarks/tabs/settings via GitHub Gist
// (requires the `gh` CLI, logged in). Split out of app.cpp.

#include "browser.h"

#include <algorithm>
#include <cstring>
#include <unordered_set>

namespace {

// Run a console command and capture stdout. Returns {} on failure.
std::string RunCapture(const std::wstring& commandLine,
                       DWORD timeoutMs = 30000) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return {};
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, 0, nullptr);
    if (nul == INVALID_HANDLE_VALUE)
        nul = GetStdHandle(STD_ERROR_HANDLE);

    std::vector<wchar_t> cmd(commandLine.begin(), commandLine.end());
    cmd.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = nul;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(rd); CloseHandle(wr);
        if (nul != GetStdHandle(STD_ERROR_HANDLE)) CloseHandle(nul);
        return {};
    }
    CloseHandle(wr);
    if (nul != GetStdHandle(STD_ERROR_HANDLE)) CloseHandle(nul);
    if (WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_TIMEOUT)
        TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);

    std::string out;
    char buf[8192];
    DWORD got = 0;
    while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got != 0)
        out.append(buf, got);
    CloseHandle(rd);
    CloseHandle(pi.hProcess);
    return out;
}

bool GhAvailable() {
    return !RunCapture(L"gh --version", 10000).empty();
}

std::string ReadTextFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

void WriteTextFile(const fs::path& path, const std::string& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << data;
}

// gist=<id> line inside data/settings.txt. Gist ids are hex; anything
// else is rejected so the value can't inject into the gh command line.
std::wstring GistId() {
    std::wifstream in(DataDirPath() / L"settings.txt");
    std::wstring line;
    while (std::getline(in, line)) {
        if (line.rfind(L"gist=", 0) != 0) continue;
        const std::wstring id = TrimWide(line.substr(5));
        const bool ok = !id.empty() && id.size() <= 64 &&
            std::all_of(id.begin(), id.end(), [](wchar_t c) {
                return (c >= L'0' && c <= L'9') ||
                       (c >= L'a' && c <= L'f') ||
                       (c >= L'A' && c <= L'F');
            });
        return ok ? id : std::wstring();
    }
    return {};
}

void SetGistId(const std::wstring& id) {
    std::vector<std::wstring> lines;
    {
        std::wifstream in(DataDirPath() / L"settings.txt");
        std::wstring line;
        while (std::getline(in, line))
            if (line.rfind(L"gist=", 0) != 0) lines.push_back(line);
    }
    if (!id.empty()) lines.push_back(L"gist=" + id);
    std::wofstream out(DataDirPath() / L"settings.txt",
                       std::ios::trunc);
    for (const auto& l : lines) out << l << L'\n';
}

const char* kSyncMarkers[] = {
    "==HISTORY==", "==BOOKMARKS==", "==TABS==", "==SETTINGS==",
    "==NOTES==", "==FEEDS==", "==QUEUE=="};
const int kSyncSectionCount = 7;
const fs::path* SyncPaths() {
    static const fs::path paths[] = {
        DataDirPath() / L"history.txt",
        DataDirPath() / L"bookmarks.txt",
        DataDirPath() / L"tabs.txt",
        DataDirPath() / L"settings.txt",
        DataDirPath() / L"notes.txt",
        DataDirPath() / L"feeds.txt",
        DataDirPath() / L"queue.txt",
    };
    return paths;
}

std::string BuildSyncPayload() {
    const fs::path* paths = SyncPaths();
    std::string payload;
    for (int i = 0; i < kSyncSectionCount; ++i) {
        payload += kSyncMarkers[i];
        payload += '\n';
        payload += ReadTextFile(paths[i]);
        if (payload.empty() || payload.back() != '\n') payload += '\n';
    }
    return payload;
}

// Sections that merge line-wise on pull (device-local files like tabs and
// settings are overwritten instead — they describe THIS machine).
bool SyncSectionMerges(int index) {
    return index <= 1 || index >= 4; // history, bookmarks, notes, feeds, queue
}

// Union-merge two line-based files: local order first, remote-only lines
// appended. `urlKey` extracts the dedupe key (history carries "ts\turl").
std::string MergeLineContent(const std::string& local,
                             const std::string& remote,
                             bool dedupeByUrl) {
    auto split = [](const std::string& s) {
        std::vector<std::string> v;
        size_t pos = 0;
        while (pos <= s.size()) {
            const size_t nl = s.find('\n', pos);
            std::string line = s.substr(
                pos, nl == std::string::npos ? std::string::npos
                                             : nl - pos);
            while (!line.empty() && (line.back() == '\r' ||
                                     line.back() == ' '))
                line.pop_back();
            if (!line.empty()) v.push_back(line);
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
        return v;
    };
    const auto keyOf = [&](const std::string& line) {
        if (!dedupeByUrl) return line;
        const size_t tab = line.find('\t');
        return tab == std::string::npos ? line : line.substr(tab + 1);
    };
    std::vector<std::string> out = split(local);
    std::unordered_set<std::string> seen;
    for (const auto& l : out) seen.insert(keyOf(l));
    for (const auto& l : split(remote)) {
        if (seen.insert(keyOf(l)).second) out.push_back(l);
    }
    std::string merged;
    for (const auto& l : out) { merged += l; merged += '\n'; }
    return merged;
}

// Split the payload back into the data files. Mergeable sections are
// union-merged with the local file instead of overwritten, so two machines
// syncing the same gist keep BOTH sides' history/bookmarks/notes.
bool ApplySyncPayload(const std::string& payload) {
    const fs::path* paths = SyncPaths();
    std::vector<std::pair<size_t, size_t>> sections;
    size_t pos = 0;
    for (const char* marker : kSyncMarkers) {
        const size_t m = payload.find(marker, pos);
        if (m == std::string::npos) return false;
        sections.push_back({m + std::strlen(marker), 0});
        pos = m + std::strlen(marker);
    }
    for (size_t i = 0; i < sections.size(); ++i) {
        size_t begin = sections[i].first;
        while (begin < payload.size() && payload[begin] == '\n') ++begin;
        size_t end = payload.size();
        if (i + 1 < sections.size()) {
            // end = start of next marker line.
            size_t nextMarker = sections[i + 1].first -
                                std::strlen(kSyncMarkers[i + 1]);
            end = nextMarker;
        }
        // Trim trailing whitespace before the next marker.
        while (end > begin &&
               (payload[end - 1] == '\n' || payload[end - 1] == '\r' ||
                payload[end - 1] == ' '))
            --end;
        std::string remote = payload.substr(begin, end - begin);
        if (!remote.empty() && remote.back() != '\n') remote += '\n';
        if (SyncSectionMerges(static_cast<int>(i))) {
            // Merge with what's already here.
            const std::string local = ReadTextFile(paths[i]);
            const bool urlKey = i == 0; // history.txt: "ts\turl" lines
            WriteTextFile(paths[i],
                          MergeLineContent(local, remote, urlKey));
        } else {
            WriteTextFile(paths[i], remote);
        }
    }
    return true;
}

} // namespace

// Public wrapper for unit tests.
std::string MergeSyncLines(const std::string& local,
                           const std::string& remote,
                           bool dedupeByUrl) {
    return MergeLineContent(local, remote, dedupeByUrl);
}

// Sync push/pull via GitHub Gist. action: "push" | "pull" | "" (status).
void UiSync(const std::wstring& action) {
    const bool ui = g_consoleUiMode;
    auto report = [&](const std::wstring& msg) {
        if (ui) g_consoleStatus = msg; else PrintLine(ToUtf8(msg));
    };
    const std::wstring act = LowerWide(action);

    if (act.empty() || act == L"status") {
        const std::wstring id = GistId();
        report(id.empty()
            ? L"GitHub-синхронизация не настроена. Команды: sync push, sync pull."
            : L"Синхронизация: gist " + id + L" (sync push / sync pull)");
        return;
    }

    if (act == L"page") {
        UiOpenInternalPage(L"sync");
        return;
    }

    if (!GhAvailable()) {
        report(L"gh CLI не найден или не залогинен (gh auth login).");
        return;
    }

    if (act == L"push") {
        const fs::path payloadPath = DataDirPath() / L"browser-sync.txt";
        WriteTextFile(payloadPath, BuildSyncPayload());
        std::wstring id = GistId();
        if (id.empty()) {
            // Create a new secret gist and remember its id.
            const std::string out = RunCapture(
                L"gh gist create --desc \"Console Browser sync\" \"" +
                payloadPath.wstring() + L"\"", 60000);
            const size_t slash = out.find_last_of("/\\");
            std::string tail = slash == std::string::npos
                ? out : out.substr(slash + 1);
            // Trim whitespace.
            const size_t b = tail.find_first_not_of(" \t\r\n");
            const size_t e = tail.find_last_not_of(" \t\r\n");
            if (b == std::string::npos) {
                report(L"Не удалось создать gist: " + ToWide(Trim(out)));
                return;
            }
            id = ToWide(tail.substr(b, e - b + 1));
            SetGistId(id);
        } else {
            // Update the existing gist's single file via the REST API.
            const std::string out = RunCapture(
                L"gh api gists/" + id +
                L" -X PATCH -F \"files[browser-sync.txt][content]=@" +
                payloadPath.wstring() + L"\"", 60000);
            if (out.find("\"id\"") == std::string::npos) {
                report(L"Gist обновить не удалось: " +
                       ToWide(Trim(out)));
                return;
            }
        }
        report(L"Синхронизация отправлена → gist " + id);
        return;
    }

    if (act == L"pull") {
        const std::wstring id = GistId();
        if (id.empty()) {
            report(L"Gist не привязан — сначала sync push.");
            return;
        }
        const std::string payload = RunCapture(
            L"gh gist view " + id +
            L" --filename browser-sync.txt --raw", 60000);
        if (payload.empty() || payload.find(kSyncMarkers[0]) ==
                                   std::string::npos) {
            report(L"Не удалось скачать gist (" + id + L").");
            return;
        }
        if (!ApplySyncPayload(payload)) {
            report(L"Формат gist-файла не распознан.");
            return;
        }
        LoadHistory();
        report(L"Синхронизация получена ← gist " + id +
               L" (история и закладки обновлены)");
        return;
    }

    report(L"Команды: sync push, sync pull, sync status, sync page.");
}

// browser://sync — what's in the gist vs local, per section.
FetchOutcome BuildSyncPage() {
    FetchOutcome out;
    out.ok = true;
    out.finalUrl = std::wstring(kInternalPrefix) + L"sync";
    out.title = "Синхронизация";
    const std::wstring id = GistId();
    std::string text =
        "Синхронизация через GitHub Gist (нужен gh CLI, gh auth login).\n"
        "Секции: история, закладки, заметки, ленты, очередь — MERGE "
        "(объединяются), вкладки и настройки — перезаписываются.\n\n";
    if (id.empty()) {
        text += "Gist не привязан. Выполните: sync push\n";
        out.text = NormalizeDisplayText(text);
        return out;
    }
    text += "Gist: " + ToUtf8(id) + "\n\n";

    // Remote payload → per-section line counts.
    const std::string payload = RunCapture(
        L"gh gist view " + id +
        L" --filename browser-sync.txt --raw", 45000);
    std::vector<size_t> remoteCount(kSyncSectionCount, 0);
    bool remoteOk = payload.find(kSyncMarkers[0]) != std::string::npos;
    if (remoteOk) {
        for (int i = 0; i < kSyncSectionCount; ++i) {
            const size_t m = payload.find(kSyncMarkers[i]);
            if (m == std::string::npos) continue;
            size_t begin = m + std::strlen(kSyncMarkers[i]);
            size_t end = payload.size();
            for (int j = i + 1; j < kSyncSectionCount; ++j) {
                const size_t nm = payload.find(kSyncMarkers[j], begin);
                if (nm != std::string::npos) { end = nm; break; }
            }
            size_t count = 0;
            for (size_t p = begin; p < end; ++p)
                if (payload[p] == '\n' &&
                    p > begin && payload[p - 1] != '\r' &&
                    p - begin > 1) ++count;
            remoteCount[i] = count;
        }
    }

    static const char* kNames[] = {
        "История", "Закладки", "Вкладки", "Настройки",
        "Заметки", "Ленты", "Очередь"};
    const fs::path* paths = SyncPaths();
    for (int i = 0; i < kSyncSectionCount; ++i) {
        const std::string local = ReadTextFile(paths[i]);
        size_t localCount = 0;
        for (char c : local) if (c == '\n') ++localCount;
        const std::string merge = SyncSectionMerges(i) ? "merge" : "replace";
        text += std::string(kNames[i]) + ": локально " +
                std::to_string(localCount) + ", в gist " +
                (remoteOk ? std::to_string(remoteCount[i]) : "?") +
                "  [" + merge + "]\n";
    }

    // Live diff: which URLs/lines exist in the gist but not locally.
    if (remoteOk) {
        std::vector<std::string> fresh;
        for (int i = 0; i < kSyncSectionCount && SyncSectionMerges(i);
             ++i) {
            const size_t m = payload.find(kSyncMarkers[i]);
            if (m == std::string::npos) continue;
            size_t begin = m + std::strlen(kSyncMarkers[i]);
            size_t end = payload.size();
            for (int j = i + 1; j < kSyncSectionCount; ++j) {
                const size_t nm = payload.find(kSyncMarkers[j], begin);
                if (nm != std::string::npos) { end = nm; break; }
            }
            const std::string local = ReadTextFile(paths[i]);
            size_t pos = begin;
            while (pos < end && fresh.size() < 20) {
                size_t nl = payload.find('\n', pos);
                if (nl == std::string::npos || nl > end) nl = end;
                std::string line = payload.substr(pos, nl - pos);
                while (!line.empty() && (line.back() == '\r' ||
                                         line.back() == ' '))
                    line.pop_back();
                pos = nl + 1;
                if (line.empty()) continue;
                // Dedupe key mirrors MergeLineContent.
                std::string key = line;
                if (i == 0) { // history: "ts\turl"
                    const size_t tab = key.find('\t');
                    if (tab != std::string::npos) key = key.substr(tab + 1);
                }
                if (local.find(key) == std::string::npos)
                    fresh.push_back(std::string(kNames[i]) + ": " + line);
            }
        }
        if (!fresh.empty()) {
            text += "\nНовое на gist (нет локально):\n";
            for (const auto& f : fresh)
                text += "  " + f + "\n";
        }
    }
    text += "\nКоманды: sync push (отправить), sync pull (получить + "
            "merge), sync status.\n";
    out.text = NormalizeDisplayText(text);
    return out;
}
