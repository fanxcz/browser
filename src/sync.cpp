// sync.cpp — cloud sync of history/bookmarks/tabs/settings via GitHub Gist
// (requires the `gh` CLI, logged in). Split out of app.cpp.

#include "browser.h"

#include <algorithm>
#include <cstring>

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
    "==HISTORY==", "==BOOKMARKS==", "==TABS==", "==SETTINGS=="};
const fs::path* SyncPaths() {
    static const fs::path paths[] = {
        DataDirPath() / L"history.txt",
        DataDirPath() / L"bookmarks.txt",
        DataDirPath() / L"tabs.txt",
        DataDirPath() / L"settings.txt",
    };
    return paths;
}

std::string BuildSyncPayload() {
    const fs::path* paths = SyncPaths();
    std::string payload;
    for (int i = 0; i < 4; ++i) {
        payload += kSyncMarkers[i];
        payload += '\n';
        payload += ReadTextFile(paths[i]);
        if (payload.empty() || payload.back() != '\n') payload += '\n';
    }
    return payload;
}

// Split the payload back into the four data files.
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
        WriteTextFile(paths[i], payload.substr(begin, end - begin) + "\n");
    }
    return true;
}

} // namespace

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

    report(L"Команды: sync push, sync pull, sync status.");
}
