#include "ui_int.h"

namespace console_ui {
    // Console palette — swappable for the light theme (Ctrl+Shift+L).
    struct Palette {
        WORD text, muted, accent, bright, selected, hover;
    };
    WORD kText, kMuted, kAccent, kBright, kSelected, kHover;

    const Palette kDarkPalette{
        FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE,   // text
        FOREGROUND_INTENSITY,                                  // muted
        FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY, // accent
        FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE |
            FOREGROUND_INTENSITY,                              // bright
        (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE) |
            BACKGROUND_BLUE | BACKGROUND_GREEN,                 // selected
        (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE) |
            BACKGROUND_BLUE                                     // hover
    };
    // Light theme: black text everywhere on a light-gray cell background.
    const WORD kLightBg = BACKGROUND_RED | BACKGROUND_GREEN |
                          BACKGROUND_BLUE;                      // gray
    const Palette kLightPalette{
        0,                                                    // text (black)
        0,                                                    // muted (black)
        0,                                                    // accent (black)
        0,                                                    // bright (black)
        BACKGROUND_BLUE,                                      // selected
        BACKGROUND_GREEN | BACKGROUND_INTENSITY                // hover
    };
    bool g_lightTheme = false;

    void ApplyPalette() {
        const Palette& p = g_lightTheme ? kLightPalette : kDarkPalette;
        kText = p.text; kMuted = p.muted; kAccent = p.accent;
        kBright = p.bright; kSelected = p.selected; kHover = p.hover;
        WORD* fg[4] = {&kText, &kMuted, &kAccent, &kBright};
        if (g_lightTheme)
            for (WORD* c : fg) *c |= kLightBg;
        else
            for (WORD* c : fg) *c &= static_cast<WORD>(~kLightBg);
    }

    std::vector<ThemePack> g_themes;
    size_t g_themeIndex = 0;
    int g_density = 1;
    int g_scrollLines = 3; // wheel lines per notch (`scroll <n>` command)

    // Map #RRGGBB to the nearest of the 16 console colors.
    WORD ConsoleColorFromHex(const std::string& hex) {
        if (hex.size() != 7 || hex[0] != '#') return 0;
        auto hv = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        const int r = hv(hex[1]) * 16 + hv(hex[2]);
        const int g = hv(hex[3]) * 16 + hv(hex[4]);
        const int b = hv(hex[5]) * 16 + hv(hex[6]);
        if (r < 0 || g < 0 || b < 0) return 0;
        // Standard EGA-ish palette used by conhost.
        static const WORD kCols[16] = {
            0, FOREGROUND_BLUE, FOREGROUND_GREEN,
            FOREGROUND_BLUE | FOREGROUND_GREEN,
            FOREGROUND_RED, FOREGROUND_RED | FOREGROUND_BLUE,
            FOREGROUND_RED | FOREGROUND_GREEN,
            FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE,
            FOREGROUND_INTENSITY,
            FOREGROUND_BLUE | FOREGROUND_INTENSITY,
            FOREGROUND_GREEN | FOREGROUND_INTENSITY,
            FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY,
            FOREGROUND_RED | FOREGROUND_INTENSITY,
            FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY,
            FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY,
            FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE |
                FOREGROUND_INTENSITY };
        static const int kRgb[16][3] = {
            {0,0,0},{0,0,128},{0,128,0},{0,128,128},
            {128,0,0},{128,0,128},{128,128,0},{192,192,192},
            {128,128,128},{0,0,255},{0,255,0},{0,255,255},
            {255,0,0},{255,0,255},{255,255,0},{255,255,255} };
        int best = 7, bestD = INT32_MAX;
        for (int i = 0; i < 16; ++i) {
            const int dr = r - kRgb[i][0], dg = g - kRgb[i][1],
                      db = b - kRgb[i][2];
            const int d = dr * dr + dg * dg + db * db;
            if (d < bestD) { bestD = d; best = i; }
        }
        return kCols[best];
    }

    // Tiny JSON field reader — enough for our flat theme files.
    std::string JsonString(const std::string& js, const char* key) {
        const std::string pat = std::string("\"") + key + "\"";
        const size_t p = js.find(pat);
        if (p == std::string::npos) return {};
        const size_t colon = js.find(':', p + pat.size());
        const size_t q1 = js.find('"', colon + 1);
        if (q1 == std::string::npos) return {};
        const size_t q2 = js.find('"', q1 + 1);
        if (q2 == std::string::npos) return {};
        return js.substr(q1 + 1, q2 - q1 - 1);
    }

    void LoadThemePacks() {
        g_themes.clear();
        g_themes.push_back({L"Тёмная", false, 0, 1});
        g_themes.push_back({L"Светлая", true, 0, 1});
        g_themes.push_back({L"Контраст", false,
                            FOREGROUND_RED | FOREGROUND_GREEN |
                                FOREGROUND_BLUE | FOREGROUND_INTENSITY, 0});
        g_themes.push_back({L"Комфорт", true, 0, 2});
        std::error_code ec;
        const fs::path dir = DataDirPath() / L"themes";
        if (fs::exists(dir, ec)) {
            for (const auto& e :
                 fs::directory_iterator(dir, ec)) {
                if (e.path().extension() != L".json") continue;
                std::ifstream in(e.path(), std::ios::binary);
                std::string js((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
                ThemePack p;
                p.name = ToWide(JsonString(js, "name"));
                if (p.name.empty())
                    p.name = ToWide(e.path().stem().string());
                const std::string mode = JsonString(js, "mode");
                p.light = mode == "light";
                p.accent = ConsoleColorFromHex(JsonString(js, "accent"));
                const std::string den = JsonString(js, "density");
                p.density = den == "compact" ? 0
                            : den == "spacious" ? 2 : 1;
                g_themes.push_back(std::move(p));
            }
        }
        if (g_themes.empty())
            g_themes.push_back({L"Тёмная", false, 0, 1});
    }

    void ApplyTheme(size_t index) {
        if (g_themes.empty()) return;
        g_themeIndex = index % g_themes.size();
        const ThemePack& p = g_themes[g_themeIndex];
        g_lightTheme = p.light;
        ApplyPalette();
        if (p.accent && !p.light) kAccent = p.accent;
        g_density = p.density;
    }

    void LoadThemeSetting() {
        std::wstring wanted = L"dark";
        {
            std::wifstream in(DataDirPath() / L"settings.txt");
            std::wstring line;
            while (std::getline(in, line)) {
                if (line.rfind(L"theme=", 0) == 0) {
                    wanted = line.substr(6);
                    break;
                }
            }
        }
        for (size_t i = 0; i < g_themes.size(); ++i) {
            const std::string nm = ToUtf8(g_themes[i].name);
            if (wanted == L"light" && g_themes[i].light &&
                g_themes[i].accent == 0) { ApplyTheme(i); return; }
            if (wanted == L"dark" && !g_themes[i].light &&
                g_themes[i].accent == 0) { ApplyTheme(i); return; }
            if (ToWide(nm) == wanted) { ApplyTheme(i); return; }
        }
        ApplyTheme(0);
    }

    // theme save <name>: persist the current theme as data/themes/<name>.json.
    bool SaveThemeAs(const std::wstring& name) {
        if (name.empty() || g_themes.empty()) return false;
        const ThemePack& p = g_themes[g_themeIndex];
        std::error_code ec;
        const fs::path dir = DataDirPath() / L"themes";
        fs::create_directories(dir, ec);
        const fs::path path = dir / (name + L".json");
        // Map the console accent attribute back to a hex approximation.
        auto accentHex = [](WORD attr) -> std::string {
            if (!attr) return {};
            // FOREGROUND_* bits are the low nibble; INTENSITY adds 8.
            const int idx = (attr & 0x0F) | ((attr & FOREGROUND_INTENSITY) ? 8 : 0);
            static const char* kStd[16] = {
                "#000000", "#0000aa", "#00aa00", "#00aaaa",
                "#aa0000", "#aa00aa", "#aa5500", "#aaaaaa",
                "#555555", "#5555ff", "#55ff55", "#55ffff",
                "#ff5555", "#ff55ff", "#ffff55", "#ffffff" };
            return kStd[idx & 0xF];
        };
        std::wofstream out(path, std::ios::trunc);
        out << L"{\n  \"name\": \"" << name << L"\",\n"
            << L"  \"mode\": \"" << (p.light ? L"light" : L"dark") << L"\",\n";
        if (p.accent) out << L"  \"accent\": \"" << ToWide(accentHex(p.accent)) << L"\",\n";
        out << L"  \"density\": \""
            << (p.density == 0 ? L"compact" : p.density == 2 ? L"spacious" : L"normal")
            << L"\"\n}\n";
        if (!out) return false;
        // Make it active immediately and persist the choice.
        LoadThemePacks();
        for (size_t i = 0; i < g_themes.size(); ++i)
            if (g_themes[i].name == name) { ApplyTheme(i); break; }
        SaveThemeSetting();
        return true;
    }

    // Per-site theme override (data/sites/<host>.theme or a `theme` rule).
    // Restores the saved theme when navigating away from an overridden page.
    void ApplySiteTheme(const std::wstring& url) {
        static bool hadOverride = false;
        const std::wstring name = SiteThemeFor(url);
        if (name.empty()) {
            if (hadOverride) { hadOverride = false; LoadThemeSetting(); }
            return;
        }
        LoadThemePacks();
        for (size_t i = 0; i < g_themes.size(); ++i) {
            if (g_themes[i].name == name) {
                hadOverride = true;
                ApplyTheme(i);
                return;
            }
        }
    }

    // theme pull <name> — download a theme JSON from the project site.
    bool PullTheme(const std::wstring& name) {
        if (name.empty()) return false;
        const std::wstring url =
            L"https://fanxcz.github.io/browser/themes/" + name +
            L".json";
        Response r = Request(url, CacheMode::Bypass);
        if (!r.ok) return false;
        const std::string js(r.body.begin(), r.body.end());
        if (js.find("\"name\"") == std::string::npos &&
            js.find("\"mode\"") == std::string::npos)
            return false;
        std::error_code ec;
        const fs::path dir = DataDirPath() / L"themes";
        fs::create_directories(dir, ec);
        std::ofstream out(dir / (name + L".json"),
                          std::ios::binary | std::ios::trunc);
        out << js;
        if (!out) return false;
        LoadThemePacks();
        return true;
    }

    void SaveThemeSetting() {
        // Rewrite settings.txt preserving other keys (e.g. downloads=).
        std::vector<std::wstring> lines;
        {
            std::wifstream in(DataDirPath() / L"settings.txt");
            std::wstring line;
            while (std::getline(in, line))
                if (line.rfind(L"theme=", 0) != 0) lines.push_back(line);
        }
        const std::string nm = ToUtf8(g_themes[g_themeIndex].name);
        lines.push_back(L"theme=" + ToWide(nm));
        std::wofstream out(DataDirPath() / L"settings.txt",
                           std::ios::trunc);
        for (const auto& l : lines) out << l << L'\n';
    }

}
