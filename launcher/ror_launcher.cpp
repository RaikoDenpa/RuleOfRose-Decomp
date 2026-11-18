#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' "      \
                        "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace fs = std::filesystem;

namespace
{
    constexpr char kElfSha256[] = "71417965b8931cadcebc33f48439df3290822316e4c139d9c4c7a4b6d7fb7692";
    constexpr char kElfName[] = "SLES_542.18";
    constexpr uint32_t kSector = 2048;
    constexpr wchar_t kTitle[] = L"Rule of Rose - PC port (playtest)";

    fs::path g_root, g_game, g_user, g_runner;

    struct Entry
    {
        std::string path;
        uint32_t lba = 0, size = 0;
    };

    std::string sha256(const std::vector<uint8_t> &data)
    {
        BCRYPT_ALG_HANDLE alg = nullptr;
        uint8_t digest[32] = {};
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
            return {};
        BCryptHash(alg, nullptr, 0, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), digest,
                   sizeof digest);
        BCryptCloseAlgorithmProvider(alg, 0);
        std::string hex;
        char b[3];
        for (uint8_t v : digest)
        {
            std::snprintf(b, sizeof b, "%02x", v);
            hex += b;
        }
        return hex;
    }

    bool readFile(const fs::path &p, std::vector<uint8_t> &out)
    {
        FILE *f = _wfopen(p.c_str(), L"rb");
        if (!f)
            return false;
        _fseeki64(f, 0, SEEK_END);
        out.resize(static_cast<size_t>(_ftelli64(f)));
        _fseeki64(f, 0, SEEK_SET);
        const bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
        std::fclose(f);
        return ok;
    }

    bool readAt(FILE *f, uint64_t off, void *buf, size_t n)
    {
        return _fseeki64(f, static_cast<int64_t>(off), SEEK_SET) == 0 && std::fread(buf, 1, n, f) == n;
    }

    uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }

    bool walk(FILE *f, uint32_t lba, uint32_t size, const std::string &prefix, std::vector<Entry> &out, int depth)
    {
        if (depth > 16)
            return false;
        std::vector<uint8_t> dir(size);
        if (!readAt(f, static_cast<uint64_t>(lba) * kSector, dir.data(), size))
            return false;
        for (uint32_t i = 0; i < size;)
        {
            const uint8_t len = dir[i];
            if (len == 0)
            {
                i = (i / kSector + 1) * kSector;
                continue;
            }
            if (i + len > size || len < 34)
                return false;
            const uint8_t *r = &dir[i];
            const uint8_t nameLen = r[32];
            if (!(nameLen == 1 && (r[33] == 0 || r[33] == 1)))
            {
                std::string name(reinterpret_cast<const char *>(r + 33), nameLen);
                if (const size_t semi = name.find(';'); semi != std::string::npos)
                    name.resize(semi);
                if (!name.empty() && name.back() == '.')
                    name.pop_back();
                if (name.empty() || name.find("..") != std::string::npos || name.find_first_of("/\\:") != std::string::npos)
                    return false;
                const std::string path = prefix.empty() ? name : prefix + "/" + name;
                if (r[25] & 2)
                {
                    if (!walk(f, le32(r + 2), le32(r + 10), path, out, depth + 1))
                        return false;
                }
                else
                    out.push_back({path, le32(r + 2), le32(r + 10)});
            }
            i += len;
        }
        return true;
    }

    template <typename Progress>
    std::wstring extractIso(const fs::path &iso, const fs::path &dst, Progress progress)
    {
        FILE *f = _wfopen(iso.c_str(), L"rb");
        if (!f)
            return L"Cannot open " + iso.wstring();
        struct Closer
        {
            FILE *f;
            ~Closer() { std::fclose(f); }
        } closer{f};
        uint8_t pvd[kSector];
        if (!readAt(f, 16ull * kSector, pvd, kSector) || pvd[0] != 1 || std::memcmp(pvd + 1, "CD001", 5) != 0)
            return L"This file is not an ISO 9660 disc image with 2048-byte sectors (a .iso made from the DVD).";
        std::vector<Entry> files;
        if (!walk(f, le32(pvd + 156 + 2), le32(pvd + 156 + 10), "", files, 0))
            return L"The disc image's directory could not be read.";
        const Entry *elf = nullptr;
        uint64_t total = 0;
        for (const Entry &e : files)
        {
            total += e.size;
            if (e.path == kElfName)
                elf = &e;
        }
        std::vector<uint8_t> elfData(elf ? elf->size : 0);
        if (!elf || !readAt(f, static_cast<uint64_t>(elf->lba) * kSector, elfData.data(), elfData.size()) ||
            sha256(elfData) != kElfSha256)
            return L"This disc image is not the release this build needs: Rule of Rose, PAL, SLES-54218 (its "
                   L"SLES_542.18 must match). Other releases are not supported.";
        ULARGE_INTEGER freeBytes{};
        if (GetDiskFreeSpaceExW(dst.parent_path().c_str(), &freeBytes, nullptr, nullptr) &&
            freeBytes.QuadPart < total + (256ull << 20))
            return L"Not enough free disk space next to RuleOfRose.exe: the game's files need " +
                   std::to_wstring(total >> 20) + L" MB.";
        std::vector<uint8_t> buf(8u << 20);
        uint64_t done = 0;
        for (const Entry &e : files)
        {
            const fs::path out = dst / fs::path(e.path);
            std::error_code ec;
            fs::create_directories(out.parent_path(), ec);
            FILE *o = _wfopen(out.c_str(), L"wb");
            if (!o)
                return L"Cannot write " + out.wstring();
            for (uint64_t pos = 0; pos < e.size;)
            {
                const size_t n = static_cast<size_t>(std::min<uint64_t>(buf.size(), e.size - pos));
                if (!readAt(f, static_cast<uint64_t>(e.lba) * kSector + pos, buf.data(), n) ||
                    std::fwrite(buf.data(), 1, n, o) != n)
                {
                    std::fclose(o);
                    return L"Reading the disc image or writing " + out.wstring() + L" failed (disk full?).";
                }
                pos += n;
                done += n;
                progress(total ? static_cast<int>(done * 1000 / total) : 1000);
            }
            std::fclose(o);
        }
        return {};
    }

    bool gameReady()
    {
        std::vector<uint8_t> elf;
        return readFile(g_game / kElfName, elf) && sha256(elf) == kElfSha256;
    }

    template <typename Progress>
    std::wstring installFrom(const fs::path &iso, Progress progress)
    {
        const fs::path tmp = g_root / L"game.partial";
        std::error_code ec;
        fs::remove_all(tmp, ec);
        std::wstring err = extractIso(iso, tmp, progress);
        if (!err.empty())
        {
            fs::remove_all(tmp, ec);
            return err;
        }
        fs::remove_all(g_game, ec);
        fs::rename(tmp, g_game, ec);
        if (ec || !gameReady())
            return L"The extracted files could not be put in place: " + g_game.wstring();
        return {};
    }

    struct Settings
    {
        bool fullscreen = false;
        int width = 1280, height = 896;
        std::string aspect = "4:3";
        std::string renderer = "auto", filtering = "ps2";
        int upscale = 2;
        bool skipIntro = true, movies = true;
        int volume = 100;
        int refresh = 50;
        bool pauseUnfocused = true, rumble = true;
        bool fxaa = false;
        int cas = 0;
        int deinterlace = 0;
        int tvShader = 0;
    };

    std::map<std::string, std::string> readIni(const fs::path &p)
    {
        std::map<std::string, std::string> m;
        std::ifstream in(p);
        std::string section;
        for (std::string line; std::getline(in, line);)
        {
            if (const size_t c = line.find_first_of("#;"); c != std::string::npos)
                line.resize(c);
            auto trim = [](std::string s)
            {
                s.erase(0, s.find_first_not_of(" \t\r"));
                s.erase(s.find_last_not_of(" \t\r") + 1);
                return s;
            };
            line = trim(line);
            if (line.size() > 2 && line.front() == '[' && line.back() == ']')
            {
                section = line.substr(1, line.size() - 2);
                continue;
            }
            const size_t eq = line.find('=');
            if (eq != std::string::npos)
                m[(section.empty() ? "" : section + "/") + trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
        }
        return m;
    }

    Settings loadSettings()
    {
        Settings s;
        const auto port = readIni(g_root / L"port.ini"), gs = readIni(g_root / L"gs.ini");
        auto num = [](const std::map<std::string, std::string> &m, const char *k, int def)
        {
            const auto it = m.find(k);
            return it == m.end() ? def : std::atoi(it->second.c_str());
        };
        auto str = [](const std::map<std::string, std::string> &m, const char *k, const std::string &def)
        {
            const auto it = m.find(k);
            return it == m.end() || it->second.empty() ? def : it->second;
        };
        s.fullscreen = num(port, "fullscreen", 0) == 1;
        s.width = num(port, "window_width", s.width);
        s.height = num(port, "window_height", s.height);
        s.volume = std::min(100, std::max(0, num(port, "volume", 100)));
        s.skipIntro = num(port, "skip_intro", 1) == 1;
        s.movies = num(port, "movies", 1) == 1;
        s.renderer = str(gs, "gs/renderer", s.renderer);
        s.upscale = std::max(1, num(gs, "gs/upscale", s.upscale));
        s.filtering = str(gs, "gs/texture_filtering", s.filtering);
        s.aspect = str(gs, "EmuCore/GS/AspectRatio", s.aspect);
        s.refresh = num(port, "refresh", 50) == 60 ? 60 : 50;
        s.pauseUnfocused = num(port, "pause_unfocused", 1) == 1;
        s.rumble = num(port, "rumble", 1) == 1;
        const std::string fxaa = str(gs, "EmuCore/GS/fxaa", "false");
        s.fxaa = fxaa == "true" || fxaa == "1";
        s.cas = num(gs, "EmuCore/GS/CASMode", 0) != 0 ? num(gs, "EmuCore/GS/CASSharpness", 50) : 0;
        s.deinterlace = num(gs, "EmuCore/GS/deinterlace_mode", 0);
        s.tvShader = num(gs, "EmuCore/GS/TVShader", 0);
        return s;
    }

    void saveSettings(const Settings &s)
    {
        std::ofstream port(g_root / L"port.ini");
        port << "# Rule of Rose PC port: options (written by RuleOfRose.exe; 0 = off, 1 = on)\n"
             << "skip_intro = " << (s.skipIntro ? 1 : 0) << "\n"
             << "movies = " << (s.movies ? 1 : 0) << "\n"
             << "fullscreen = " << (s.fullscreen ? 1 : 0) << "\n"
             << "window_width = " << s.width << "\n"
             << "window_height = " << s.height << "\n"
             << "volume = " << s.volume << "\n"
             << "refresh = " << s.refresh << "\n"
             << "pause_unfocused = " << (s.pauseUnfocused ? 1 : 0) << "\n"
             << "rumble = " << (s.rumble ? 1 : 0) << "\n"
             << "widescreen = " << (s.aspect == "16:9" ? 1 : 0) << "\n";
        std::ofstream gs(g_root / L"gs.ini");
        gs << "# Graphics (written by RuleOfRose.exe). [gs]: renderer = auto | d3d11 | d3d12 | vulkan | opengl;\n"
           << "# upscale = internal resolution multiplier; texture_filtering = ps2 | forced | nearest.\n"
           << "# Other sections go to PCSX2's GS as they are (e.g. [EmuCore/GS] AspectRatio).\n"
           << "[gs]\n"
           << "renderer = " << s.renderer << "\n"
           << "upscale = " << s.upscale << "\n"
           << "texture_filtering = " << s.filtering << "\n"
           << "\n[EmuCore/GS]\n"
           << "AspectRatio = " << s.aspect << "\n"
           << "fxaa = " << (s.fxaa ? "true" : "false") << "\n"
           << "CASMode = " << (s.cas > 0 ? 1 : 0) << "\n"
           << "CASSharpness = " << (s.cas > 0 ? s.cas : 50) << "\n"
           << "deinterlace_mode = " << s.deinterlace << "\n"
           << "TVShader = " << s.tvShader << "\n";
    }

    void setDefault(const wchar_t *name, const std::wstring &value)
    {
        wchar_t cur[8];
        if (GetEnvironmentVariableW(name, cur, 8) == 0)
            SetEnvironmentVariableW(name, value.c_str());
    }

    HANDLE startGame(const Settings &s, fs::path &logs, std::wstring &err)
    {
        wchar_t stamp[32];
        const std::time_t now = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &now);
        std::wcsftime(stamp, std::size(stamp), L"%Y%m%d-%H%M%S", &tm);
        logs = g_user / L"logs" / stamp;
        std::error_code ec;
        fs::create_directories(logs, ec);
        fs::create_directories(g_user / L"memcard", ec);
        setDefault(L"ROR_PAD_CONFIG", (g_root / L"pad.ini").wstring());
        setDefault(L"ROR_GS_INI", (g_root / L"gs.ini").wstring());
        setDefault(L"ROR_PORT_INI", (g_root / L"port.ini").wstring());
        setDefault(L"ROR_MC_DIR", (g_user / L"memcard").wstring());
        SetEnvironmentVariableW(L"ROR_LOG", (logs / L"ror.log").wstring().c_str());
        if (!s.movies)
            setDefault(L"ROR_MOVIES", L"0");
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE out = CreateFileW((logs / L"stdout.txt").c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
        HANDLE errh = CreateFileW((logs / L"stderr.txt").c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        STARTUPINFOW si{};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = nullptr;
        si.hStdOutput = out;
        si.hStdError = errh;
        std::wstring cmd = L"\"" + g_runner.wstring() + L"\" \"" + (g_game / kElfName).wstring() + L"\"";
        PROCESS_INFORMATION pi{};
        const BOOL ok = CreateProcessW(g_runner.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                       g_user.c_str(), &si, &pi);
        CloseHandle(out);
        CloseHandle(errh);
        if (!ok)
        {
            err = L"Could not start bin\\ps2EntryRunner.exe (error " + std::to_wstring(GetLastError()) + L").";
            return nullptr;
        }
        CloseHandle(pi.hThread);
        return pi.hProcess;
    }

    enum : int
    {
        IdMode = 100, IdSize, IdAspect, IdRefresh, IdRenderer, IdUpscale, IdFilter, IdFxaa, IdCas, IdDeinterlace,
        IdTvShader, IdSkipIntro, IdMovies, IdPause, IdRumble, IdVolume, IdVolumeText, IdControls, IdSaves, IdLogs,
        IdDisc, IdPlay, IdQuit, IdStatus, IdProgress
    };
    constexpr UINT WmProgress = WM_APP + 1, WmExtracted = WM_APP + 2, WmGameEnded = WM_APP + 3;

    struct Choice
    {
        const wchar_t *label;
        const char *value;
    };
    const Choice kModes[] = {{L"Window", "0"}, {L"Fullscreen", "1"}};
    const Choice kSizes[] = {{L"640 x 448 (1x)", "640x448"}, {L"960 x 672 (1.5x)", "960x672"},
                             {L"1280 x 896 (2x)", "1280x896"}, {L"1600 x 1120 (2.5x)", "1600x1120"},
                             {L"1920 x 1344 (3x)", "1920x1344"}};
    const Choice kAspects[] = {{L"4:3 (as on the PS2)", "4:3"}, {L"16:9 widescreen", "16:9"},
                               {L"Stretch to the window", "Stretch"}};
    const Choice kRefreshes[] = {{L"25 fps (PAL, the original speed)", "50"}, {L"30 fps (60 Hz, the NTSC speed)", "60"}};
    const Choice kFxaa[] = {{L"Off", "0"}, {L"FXAA", "1"}};
    const Choice kCas[] = {{L"Off", "0"}, {L"Light", "30"}, {L"Medium", "50"}, {L"Strong", "80"}};
    const Choice kDeinterlace[] = {{L"Automatic", "0"}, {L"Off", "1"}, {L"Blend", "6"}, {L"Bob", "4"},
                                   {L"Adaptive", "8"}};
    const Choice kTvShaders[] = {{L"None", "0"}, {L"Scanlines", "1"}, {L"CRT (Lottes)", "5"}};
    const Choice kRenderers[] = {{L"Automatic", "auto"}, {L"Direct3D 11", "d3d11"}, {L"Direct3D 12", "d3d12"},
                                 {L"Vulkan", "vulkan"}, {L"OpenGL", "opengl"}};
    const Choice kUpscales[] = {{L"Native (1x)", "1"}, {L"2x", "2"}, {L"3x", "3"}, {L"4x", "4"}};
    const Choice kFilters[] = {{L"As on the PS2", "ps2"}, {L"Smooth (bilinear everywhere)", "forced"},
                               {L"Sharp (no filtering)", "nearest"}};

    HWND g_wnd = nullptr;
    HFONT g_font = nullptr, g_bold = nullptr;
    int g_dpi = 96;
    Settings g_settings;
    std::wstring g_isoArg;
    bool g_busy = false;
    fs::path g_sessionLogs;

    int S(int v) { return MulDiv(v, g_dpi, 96); }
    HWND item(int id) { return GetDlgItem(g_wnd, id); }

    HWND addTo(HWND parent, const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id,
               HFONT font = nullptr)
    {
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), parent,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font ? font : g_font), TRUE);
        return c;
    }

    HWND add(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id, HFONT font = nullptr)
    {
        return addTo(g_wnd, cls, text, style, x, y, w, h, id, font);
    }

    template <size_t N>
    void combo(int x, int y, const wchar_t *label, int id, const Choice (&choices)[N], const std::string &current)
    {
        add(L"STATIC", label, SS_LEFT, x, y + 4, 130, 20, 0);
        HWND c = add(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, x + 135, y, 230, 200, id);
        int sel = 0;
        for (size_t i = 0; i < N; ++i)
        {
            SendMessageW(c, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(choices[i].label));
            if (current == choices[i].value)
                sel = static_cast<int>(i);
        }
        SendMessageW(c, CB_SETCURSEL, sel, 0);
    }

    template <size_t N>
    std::string picked(int id, const Choice (&choices)[N])
    {
        const LRESULT i = SendMessageW(item(id), CB_GETCURSEL, 0, 0);
        return choices[(i >= 0 && static_cast<size_t>(i) < N) ? i : 0].value;
    }

    bool checked(int id) { return SendMessageW(item(id), BM_GETCHECK, 0, 0) == BST_CHECKED; }

    void readControls()
    {
        Settings &s = g_settings;
        s.fullscreen = picked(IdMode, kModes) == "1";
        const std::string size = picked(IdSize, kSizes);
        std::sscanf(size.c_str(), "%dx%d", &s.width, &s.height);
        s.aspect = picked(IdAspect, kAspects);
        s.renderer = picked(IdRenderer, kRenderers);
        s.upscale = std::atoi(picked(IdUpscale, kUpscales).c_str());
        s.filtering = picked(IdFilter, kFilters);
        s.refresh = std::atoi(picked(IdRefresh, kRefreshes).c_str());
        s.fxaa = picked(IdFxaa, kFxaa) == "1";
        s.cas = std::atoi(picked(IdCas, kCas).c_str());
        s.deinterlace = std::atoi(picked(IdDeinterlace, kDeinterlace).c_str());
        s.tvShader = std::atoi(picked(IdTvShader, kTvShaders).c_str());
        s.skipIntro = checked(IdSkipIntro);
        s.movies = checked(IdMovies);
        s.pauseUnfocused = checked(IdPause);
        s.rumble = checked(IdRumble);
        s.volume = static_cast<int>(SendMessageW(item(IdVolume), TBM_GETPOS, 0, 0));
    }

    void showVolume()
    {
        const int v = static_cast<int>(SendMessageW(item(IdVolume), TBM_GETPOS, 0, 0));
        SetWindowTextW(item(IdVolumeText), (std::to_wstring(v) + L" %").c_str());
    }

    void status(const std::wstring &text) { SetWindowTextW(item(IdStatus), text.c_str()); }

    void showGameState()
    {
        status(gameReady() ? L"Game files: ready (PAL, SLES-54218)."
                           : L"Game files: not set up yet. Play asks for your disc image (.iso of the PAL "
                             L"release, SLES-54218) and copies its files once (about 2.6 GB).");
    }

    void setBusy(bool busy)
    {
        g_busy = busy;
        for (int id = IdMode; id <= IdQuit; ++id)
            if (HWND c = item(id))
                EnableWindow(c, !busy);
    }

    void openFolder(const fs::path &p)
    {
        std::error_code ec;
        fs::create_directories(p, ec);
        ShellExecuteW(g_wnd, L"explore", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    std::wstring pickIso()
    {
        wchar_t file[MAX_PATH * 4] = {};
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof ofn;
        ofn.hwndOwner = g_wnd;
        ofn.lpstrFilter = L"PS2 disc image (*.iso)\0*.iso\0All files\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = static_cast<DWORD>(std::size(file));
        ofn.lpstrTitle = L"Rule of Rose: choose your disc image (PAL, SLES-54218)";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        return GetOpenFileNameW(&ofn) ? std::wstring(file) : std::wstring();
    }

    void beginInstall(const std::wstring &iso, bool thenPlay)
    {
        setBusy(true);
        ShowWindow(item(IdProgress), SW_SHOW);
        SendMessageW(item(IdProgress), PBM_SETPOS, 0, 0);
        status(L"Copying the game's files from the disc image...");
        std::thread([iso, thenPlay]
                    {
                        int last = -1;
                        std::wstring err = installFrom(iso, [&](int permille)
                                                       {
                                                           if (permille != last)
                                                           {
                                                               last = permille;
                                                               PostMessageW(g_wnd, WmProgress, permille, 0);
                                                           }
                                                       });
                        PostMessageW(g_wnd, WmExtracted, thenPlay,
                                     reinterpret_cast<LPARAM>(err.empty() ? nullptr : new std::wstring(err)));
                    })
            .detach();
    }

    void play()
    {
        readControls();
        saveSettings(g_settings);
        if (!gameReady())
        {
            std::wstring iso = g_isoArg.empty() ? pickIso() : g_isoArg;
            g_isoArg.clear();
            if (!iso.empty())
                beginInstall(iso, true);
            return;
        }
        std::wstring err;
        HANDLE proc = startGame(g_settings, g_sessionLogs, err);
        if (!proc)
        {
            MessageBoxW(g_wnd, err.c_str(), kTitle, MB_OK | MB_ICONERROR);
            return;
        }
        setBusy(true);
        status(L"The game is running.");
        ShowWindow(g_wnd, SW_HIDE);
        std::thread([proc]
                    {
                        WaitForSingleObject(proc, INFINITE);
                        DWORD code = 0;
                        GetExitCodeProcess(proc, &code);
                        CloseHandle(proc);
                        PostMessageW(g_wnd, WmGameEnded, code, 0);
                    })
            .detach();
    }

    void build()
    {
        const Settings &s = g_settings;
        add(L"STATIC", L"Rule of Rose", SS_LEFT, 16, 12, 400, 26, 0, g_bold);
        add(L"STATIC", L"Unofficial PC port - playtest build", SS_LEFT, 16, 38, 400, 20, 0);

        add(L"BUTTON", L"Display", BS_GROUPBOX, 12, 64, 396, 142, 0);
        combo(24, 84, L"Mode:", IdMode, kModes, s.fullscreen ? "1" : "0");
        combo(24, 114, L"Window size:", IdSize, kSizes, std::to_string(s.width) + "x" + std::to_string(s.height));
        combo(24, 144, L"Picture:", IdAspect, kAspects, s.aspect);
        combo(24, 174, L"Frame rate:", IdRefresh, kRefreshes, std::to_string(s.refresh));

        add(L"BUTTON", L"Graphics", BS_GROUPBOX, 12, 212, 396, 232, 0);
        combo(24, 232, L"Renderer:", IdRenderer, kRenderers, s.renderer);
        combo(24, 262, L"Resolution:", IdUpscale, kUpscales, std::to_string(s.upscale));
        combo(24, 292, L"Texture filtering:", IdFilter, kFilters, s.filtering);
        combo(24, 322, L"Anti-aliasing:", IdFxaa, kFxaa, s.fxaa ? "1" : "0");
        combo(24, 352, L"Sharpening:", IdCas, kCas, std::to_string(s.cas));
        combo(24, 382, L"Deinterlacing:", IdDeinterlace, kDeinterlace, std::to_string(s.deinterlace));
        combo(24, 412, L"Screen filter:", IdTvShader, kTvShaders, std::to_string(s.tvShader));

        constexpr int R = 420;
        add(L"BUTTON", L"Game", BS_GROUPBOX, R, 64, 396, 160, 0);
        add(L"BUTTON", L"Skip the intro (logos and intro movie at launch)", BS_AUTOCHECKBOX | WS_TABSTOP, R + 12, 84,
            370, 20, IdSkipIntro);
        add(L"BUTTON", L"Play movies", BS_AUTOCHECKBOX | WS_TABSTOP, R + 12, 108, 370, 20, IdMovies);
        add(L"BUTTON", L"Pause when the window is in the background", BS_AUTOCHECKBOX | WS_TABSTOP, R + 12, 132, 370,
            20, IdPause);
        add(L"BUTTON", L"Controller vibration (also the game's own option)", BS_AUTOCHECKBOX | WS_TABSTOP, R + 12,
            156, 370, 20, IdRumble);
        SendMessageW(item(IdSkipIntro), BM_SETCHECK, s.skipIntro ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(item(IdMovies), BM_SETCHECK, s.movies ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(item(IdPause), BM_SETCHECK, s.pauseUnfocused ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(item(IdRumble), BM_SETCHECK, s.rumble ? BST_CHECKED : BST_UNCHECKED, 0);
        add(L"STATIC", L"Volume:", SS_LEFT, R + 12, 188, 60, 20, 0);
        HWND vol = add(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, R + 72, 184, 260, 26, IdVolume);
        SendMessageW(vol, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendMessageW(vol, TBM_SETPAGESIZE, 0, 10);
        SendMessageW(vol, TBM_SETPOS, TRUE, s.volume);
        add(L"STATIC", L"", SS_LEFT, R + 338, 188, 50, 20, IdVolumeText);
        showVolume();

        add(L"BUTTON", L"Controls...", BS_PUSHBUTTON | WS_TABSTOP, R, 234, 94, 28, IdControls);
        add(L"BUTTON", L"Saves", BS_PUSHBUTTON | WS_TABSTOP, R + 100, 234, 94, 28, IdSaves);
        add(L"BUTTON", L"Logs", BS_PUSHBUTTON | WS_TABSTOP, R + 200, 234, 94, 28, IdLogs);
        add(L"BUTTON", L"Disc image...", BS_PUSHBUTTON | WS_TABSTOP, R + 300, 234, 96, 28, IdDisc);
        add(L"STATIC", L"In the game: F11 fullscreen, hold F to fast-forward, Start skips movies. 16:9 widens the "
                       L"3D view; menus and captions are stretched.",
            SS_LEFT, R + 4, 272, 392, 50, 0);

        add(L"STATIC", L"", SS_LEFT, R + 4, 330, 392, 50, IdStatus);
        HWND bar = add(PROGRESS_CLASSW, L"", PBS_SMOOTH, R, 384, 396, 16, IdProgress);
        SendMessageW(bar, PBM_SETRANGE32, 0, 1000);
        ShowWindow(bar, SW_HIDE);
        add(L"BUTTON", L"Play", BS_DEFPUSHBUTTON | WS_TABSTOP, R + 196, 410, 112, 34, IdPlay, g_bold);
        add(L"BUTTON", L"Quit", BS_PUSHBUTTON | WS_TABSTOP, R + 316, 410, 80, 34, IdQuit);
        showGameState();
    }

    struct PadRow
    {
        const char *entry;
        const char *dir;
        const wchar_t *label;
    };
    const PadRow kPadRows[] = {
        {"cross", nullptr, L"Cross (act, attack)"}, {"circle", nullptr, L"Circle (back)"},
        {"square", nullptr, L"Square"}, {"triangle", nullptr, L"Triangle"}, {"l1", nullptr, L"L1"},
        {"r1", nullptr, L"R1 (stance)"}, {"l2", nullptr, L"L2"}, {"r2", nullptr, L"R2"}, {"l3", nullptr, L"L3"},
        {"r3", nullptr, L"R3"}, {"start", nullptr, L"Start (items)"}, {"select", nullptr, L"Select"},
        {"up", nullptr, L"D-pad up"}, {"down", nullptr, L"D-pad down"}, {"left", nullptr, L"D-pad left"},
        {"right", nullptr, L"D-pad right"},
        {"stick5", "key-", L"Left stick up (move)"}, {"stick5", "key+", L"Left stick down"},
        {"stick4", "key-", L"Left stick left"}, {"stick4", "key+", L"Left stick right"},
        {"stick3", "key-", L"Right stick up"}, {"stick3", "key+", L"Right stick down"},
        {"stick2", "key-", L"Right stick left"}, {"stick2", "key+", L"Right stick right"},
        {"fast_forward", nullptr, L"Fast-forward (hold)"}};
    constexpr int kPadRowCount = static_cast<int>(std::size(kPadRows));
    constexpr int kFirstStickRow = 16;

    const char kDefaultPad[] = "select = key:TAB pad:MIDDLE_LEFT\n"
                               "l3 = pad:LEFT_THUMB\n"
                               "r3 = pad:RIGHT_THUMB\n"
                               "start = key:ENTER pad:MIDDLE_RIGHT\n"
                               "up = key:UP pad:LEFT_FACE_UP\n"
                               "right = key:RIGHT pad:LEFT_FACE_RIGHT\n"
                               "down = key:DOWN pad:LEFT_FACE_DOWN\n"
                               "left = key:LEFT pad:LEFT_FACE_LEFT\n"
                               "l2 = key:ONE pad:LEFT_TRIGGER_2\n"
                               "r2 = key:THREE pad:RIGHT_TRIGGER_2\n"
                               "l1 = key:Q pad:LEFT_TRIGGER_1\n"
                               "r1 = key:E pad:RIGHT_TRIGGER_1\n"
                               "triangle = key:V pad:RIGHT_FACE_UP\n"
                               "circle = key:C key:BACKSPACE pad:RIGHT_FACE_RIGHT\n"
                               "cross = key:X key:SPACE pad:RIGHT_FACE_DOWN\n"
                               "square = key:Z pad:RIGHT_FACE_LEFT\n"
                               "fast_forward = key:F\n"
                               "stick2 = axis:RIGHT_X key-:J key+:L\n"
                               "stick3 = axis:RIGHT_Y key-:I key+:K\n"
                               "stick4 = axis:LEFT_X key-:A key+:D\n"
                               "stick5 = axis:LEFT_Y key-:W key+:S\n";
    const char *const kPadOrder[] = {"select", "l3", "r3", "start", "up", "right", "down", "left", "l2", "r2", "l1",
                                     "r1", "triangle", "circle", "cross", "square", "fast_forward", "stick2",
                                     "stick3", "stick4", "stick5"};

    const Choice kPadButtonChoices[] = {
        {L"(none)", ""}, {L"A / Cross", "RIGHT_FACE_DOWN"}, {L"B / Circle", "RIGHT_FACE_RIGHT"},
        {L"X / Square", "RIGHT_FACE_LEFT"}, {L"Y / Triangle", "RIGHT_FACE_UP"}, {L"D-pad up", "LEFT_FACE_UP"},
        {L"D-pad down", "LEFT_FACE_DOWN"}, {L"D-pad left", "LEFT_FACE_LEFT"}, {L"D-pad right", "LEFT_FACE_RIGHT"},
        {L"LB / L1", "LEFT_TRIGGER_1"}, {L"RB / R1", "RIGHT_TRIGGER_1"}, {L"LT / L2", "LEFT_TRIGGER_2"},
        {L"RT / R2", "RIGHT_TRIGGER_2"}, {L"Left stick click", "LEFT_THUMB"}, {L"Right stick click", "RIGHT_THUMB"},
        {L"Back / Select", "MIDDLE_LEFT"}, {L"Start", "MIDDLE_RIGHT"}, {L"Guide", "MIDDLE"}};

    using PadMap = std::map<std::string, std::vector<std::string>>;

    PadMap parsePad(std::istream &in)
    {
        PadMap m;
        for (std::string line; std::getline(in, line);)
        {
            if (const size_t c = line.find('#'); c != std::string::npos)
                line.resize(c);
            const size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            std::istringstream name(line.substr(0, eq)), tokens(line.substr(eq + 1));
            std::string n, t;
            name >> n;
            if (n.empty())
                continue;
            std::vector<std::string> &v = m[n];
            v.clear();
            while (tokens >> t)
                v.push_back(t);
        }
        return m;
    }

    PadMap defaultPad()
    {
        std::istringstream in(kDefaultPad);
        return parsePad(in);
    }

    PadMap loadPad()
    {
        std::ifstream in(g_root / L"pad.ini");
        PadMap m = in ? parsePad(in) : PadMap{};
        return m.empty() ? defaultPad() : m;
    }

    std::string keyName(WPARAM vk, LPARAM lp)
    {
        const bool ext = (lp >> 24) & 1;
        static const char *const kDigits[] = {"ZERO", "ONE", "TWO", "THREE", "FOUR", "FIVE", "SIX", "SEVEN", "EIGHT",
                                              "NINE"};
        if (vk >= 'A' && vk <= 'Z')
            return std::string(1, static_cast<char>(vk));
        if (vk >= '0' && vk <= '9')
            return kDigits[vk - '0'];
        if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
            return "KP_" + std::to_string(vk - VK_NUMPAD0);
        if (vk >= VK_F1 && vk <= VK_F12)
            return vk == VK_F11 ? std::string() : "F" + std::to_string(vk - VK_F1 + 1);
        switch (vk)
        {
        case VK_SPACE: return "SPACE";
        case VK_RETURN: return ext ? "KP_ENTER" : "ENTER";
        case VK_TAB: return "TAB";
        case VK_BACK: return "BACKSPACE";
        case VK_UP: return "UP";
        case VK_DOWN: return "DOWN";
        case VK_LEFT: return "LEFT";
        case VK_RIGHT: return "RIGHT";
        case VK_SHIFT:
            return MapVirtualKeyW((lp >> 16) & 0xff, MAPVK_VSC_TO_VK_EX) == VK_RSHIFT ? "RIGHT_SHIFT" : "LEFT_SHIFT";
        case VK_CONTROL: return ext ? "RIGHT_CONTROL" : "LEFT_CONTROL";
        case VK_MENU: return ext ? "RIGHT_ALT" : "LEFT_ALT";
        case VK_INSERT: return "INSERT";
        case VK_DELETE: return "DELETE";
        case VK_HOME: return "HOME";
        case VK_END: return "END";
        case VK_PRIOR: return "PAGE_UP";
        case VK_NEXT: return "PAGE_DOWN";
        case VK_OEM_COMMA: return "COMMA";
        case VK_OEM_PERIOD: return "PERIOD";
        case VK_OEM_2: return "SLASH";
        case VK_OEM_1: return "SEMICOLON";
        case VK_OEM_MINUS: return "MINUS";
        case VK_OEM_PLUS: return "EQUAL";
        }
        return {};
    }

    std::wstring keyLabel(const std::string &name)
    {
        if (name.empty())
            return L"-";
        std::wstring out;
        bool upper = true;
        for (char c : name.rfind("KP_", 0) == 0 ? "Num " + name.substr(3) : name)
        {
            if (c == '_')
            {
                out += L' ';
                upper = true;
                continue;
            }
            out += static_cast<wchar_t>(upper ? c : std::tolower(static_cast<unsigned char>(c)));
            upper = false;
        }
        return out;
    }

    struct PadSlot
    {
        std::string key[2], pad;
    };

    enum : int
    {
        IdCtlFirst = 2000,
        IdCtlDefaults = 2900, IdCtlCancel, IdCtlSave
    };

    HWND g_ctl = nullptr;
    PadMap g_padMap;
    std::vector<PadSlot> g_slots;
    int g_capture = -1;

    void slotsFrom(const PadMap &m)
    {
        g_slots.assign(kPadRowCount, {});
        for (int i = 0; i < kPadRowCount; ++i)
        {
            const PadRow &r = kPadRows[i];
            const auto it = m.find(r.entry);
            if (it == m.end())
                continue;
            const std::string keyPrefix = r.dir ? std::string(r.dir) + ":" : "key:";
            int k = 0;
            for (const std::string &t : it->second)
            {
                if (t.rfind(keyPrefix, 0) == 0 && k < 2)
                    g_slots[i].key[k++] = t.substr(keyPrefix.size());
                else if (!r.dir && t.rfind("pad:", 0) == 0 && g_slots[i].pad.empty())
                    g_slots[i].pad = t.substr(4);
            }
        }
    }

    void showSlots()
    {
        for (int i = 0; i < kPadRowCount; ++i)
        {
            for (int k = 0; k < 2; ++k)
                SetWindowTextW(GetDlgItem(g_ctl, IdCtlFirst + 3 * i + k), keyLabel(g_slots[i].key[k]).c_str());
            if (HWND c = GetDlgItem(g_ctl, IdCtlFirst + 3 * i + 2); c && !kPadRows[i].dir)
            {
                int sel = 0;
                for (size_t j = 0; j < std::size(kPadButtonChoices); ++j)
                    if (g_slots[i].pad == kPadButtonChoices[j].value)
                        sel = static_cast<int>(j);
                SendMessageW(c, CB_SETCURSEL, sel, 0);
            }
        }
    }

    bool savePad()
    {
        PadMap m = g_padMap;
        for (int i = 0; i < kPadRowCount; ++i)
        {
            const PadRow &r = kPadRows[i];
            std::vector<std::string> &v = m[r.entry];
            const std::string keyPrefix = r.dir ? std::string(r.dir) + ":" : "key:";
            v.erase(std::remove_if(v.begin(), v.end(),
                                   [&](const std::string &t)
                                   { return t.rfind(keyPrefix, 0) == 0 || (!r.dir && t.rfind("pad:", 0) == 0); }),
                    v.end());
            for (const std::string &k : g_slots[i].key)
                if (!k.empty())
                    v.push_back(keyPrefix + k);
            if (!r.dir)
            {
                const LRESULT sel = SendMessageW(GetDlgItem(g_ctl, IdCtlFirst + 3 * i + 2), CB_GETCURSEL, 0, 0);
                if (sel > 0 && static_cast<size_t>(sel) < std::size(kPadButtonChoices))
                    v.push_back(std::string("pad:") + kPadButtonChoices[sel].value);
            }
        }
        std::ofstream out(g_root / L"pad.ini");
        if (!out)
            return false;
        out << "# Rule of Rose pad mapping (written by RuleOfRose.exe's Controls window).\n"
            << "# button = key:NAME (raylib KEY_ names) pad:NAME (GAMEPAD_BUTTON_ names); stickN = axis:NAME key-:NAME "
               "key+:NAME\n\n";
        auto line = [&](const std::string &name, const std::vector<std::string> &v)
        {
            out << name << " =";
            for (const std::string &t : v)
                out << ' ' << t;
            out << '\n';
        };
        for (const char *name : kPadOrder)
            if (const auto it = m.find(name); it != m.end())
                line(name, it->second);
        for (const auto &[name, v] : m)
            if (std::find_if(std::begin(kPadOrder), std::end(kPadOrder), [&](const char *n) { return name == n; }) ==
                std::end(kPadOrder))
                line(name, v);
        return static_cast<bool>(out);
    }

    void closeControls()
    {
        EnableWindow(g_wnd, TRUE);
        DestroyWindow(g_ctl);
        g_ctl = nullptr;
        g_capture = -1;
        SetForegroundWindow(g_wnd);
    }

    LRESULT CALLBACK keyBoxProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR)
    {
        const int ctlId = static_cast<int>(id);
        switch (msg)
        {
        case WM_GETDLGCODE:
            if (g_capture == ctlId)
                return DLGC_WANTALLKEYS;
            break;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (g_capture == ctlId)
            {
                const int row = (ctlId - IdCtlFirst) / 3, k = (ctlId - IdCtlFirst) % 3;
                std::string name;
                if (wp != VK_ESCAPE)
                {
                    name = keyName(wp, lp);
                    if (name.empty())
                    {
                        MessageBeep(MB_ICONWARNING);
                        return 0;
                    }
                }
                g_slots[row].key[k] = name;
                g_capture = -1;
                SetWindowTextW(hwnd, keyLabel(name).c_str());
                return 0;
            }
            break;
        case WM_CHAR:
        case WM_SYSCHAR:
            if (g_capture == ctlId)
                return 0;
            break;
        case WM_KILLFOCUS:
            if (g_capture == ctlId)
            {
                const int row = (ctlId - IdCtlFirst) / 3, k = (ctlId - IdCtlFirst) % 3;
                g_capture = -1;
                SetWindowTextW(hwnd, keyLabel(g_slots[row].key[k]).c_str());
            }
            break;
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, keyBoxProc, id);
            break;
        }
        return DefSubclassProc(hwnd, msg, wp, lp);
    }

    LRESULT CALLBACK controlsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_COMMAND:
        {
            const int id = LOWORD(wp);
            if (id >= IdCtlFirst && id < IdCtlFirst + 3 * kPadRowCount && (id - IdCtlFirst) % 3 < 2 &&
                HIWORD(wp) == BN_CLICKED)
            {
                if (g_capture >= 0 && g_capture != id)
                    SendMessageW(GetDlgItem(hwnd, g_capture), WM_KILLFOCUS, 0, 0);
                g_capture = id;
                SetWindowTextW(GetDlgItem(hwnd, id), L"press a key...");
                SetFocus(GetDlgItem(hwnd, id));
                return 0;
            }
            if (id == IdCtlDefaults)
            {
                slotsFrom(defaultPad());
                showSlots();
                return 0;
            }
            if (id == IdCtlCancel || id == IDCANCEL)
            {
                closeControls();
                return 0;
            }
            if (id == IdCtlSave)
            {
                if (!savePad())
                    MessageBoxW(hwnd, L"pad.ini could not be written.", kTitle, MB_OK | MB_ICONERROR);
                closeControls();
                return 0;
            }
            break;
        }
        case WM_CTLCOLORSTATIC:
        {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
        }
        case WM_CLOSE:
            closeControls();
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void openControls()
    {
        if (g_ctl)
        {
            SetForegroundWindow(g_ctl);
            return;
        }
        static bool registered = false;
        if (!registered)
        {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof wc;
            wc.lpfnWndProc = controlsProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
            wc.lpszClassName = L"RorControls";
            RegisterClassExW(&wc);
            registered = true;
        }
        g_padMap = loadPad();
        slotsFrom(g_padMap);
        const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
        RECT r{0, 0, S(916), S(556)};
        AdjustWindowRect(&r, style, FALSE);
        RECT owner{};
        GetWindowRect(g_wnd, &owner);
        const int w = r.right - r.left, h = r.bottom - r.top;
        g_ctl = CreateWindowExW(WS_EX_DLGMODALFRAME, L"RorControls", L"Controls", style,
                                std::max(0L, (owner.left + owner.right - w) / 2),
                                std::max(0L, (owner.top + owner.bottom - h) / 2), w, h, g_wnd, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
        for (int col = 0; col < 2; ++col)
        {
            const int x = 12 + col * 456;
            addTo(g_ctl, L"STATIC", col ? L"Sticks, other" : L"PS2 buttons", SS_LEFT, x, 12, 128, 20, 0, g_bold);
            addTo(g_ctl, L"STATIC", L"Key", SS_LEFT, x + 132, 16, 80, 18, 0);
            addTo(g_ctl, L"STATIC", L"Key 2", SS_LEFT, x + 218, 16, 80, 18, 0);
            addTo(g_ctl, L"STATIC", L"Gamepad", SS_LEFT, x + 304, 16, 120, 18, 0);
        }
        for (int i = 0; i < kPadRowCount; ++i)
        {
            const bool right = i >= kFirstStickRow;
            const int x = 12 + (right ? 456 : 0), y = 40 + (right ? i - kFirstStickRow : i) * 28;
            const PadRow &row = kPadRows[i];
            addTo(g_ctl, L"STATIC", row.label, SS_LEFT, x, y + 4, 128, 20, 0);
            for (int k = 0; k < 2; ++k)
            {
                HWND b = addTo(g_ctl, L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, x + 132 + 86 * k, y, 80, 24,
                               IdCtlFirst + 3 * i + k);
                SetWindowSubclass(b, keyBoxProc, static_cast<UINT_PTR>(IdCtlFirst + 3 * i + k), 0);
            }
            if (row.dir)
                addTo(g_ctl, L"STATIC", row.entry[5] >= '4' ? L"left stick" : L"right stick", SS_LEFT, x + 304, y + 4,
                      120, 20, 0);
            else
            {
                HWND c = addTo(g_ctl, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, x + 304, y, 136,
                               300, IdCtlFirst + 3 * i + 2);
                for (const Choice &ch : kPadButtonChoices)
                    SendMessageW(c, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(ch.label));
            }
        }
        addTo(g_ctl, L"STATIC", L"Click a key box, then press the key (Esc clears it). The gamepad's sticks move "
                                L"and turn as before; F11 stays fullscreen.",
              SS_LEFT, 468, 300, 436, 40, 0);
        addTo(g_ctl, L"BUTTON", L"Defaults", BS_PUSHBUTTON | WS_TABSTOP, 468, 512, 96, 30, IdCtlDefaults);
        addTo(g_ctl, L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, 704, 512, 96, 30, IdCtlCancel);
        addTo(g_ctl, L"BUTTON", L"Save", BS_DEFPUSHBUTTON | WS_TABSTOP, 808, 512, 96, 30, IdCtlSave);
        showSlots();
        EnableWindow(g_wnd, FALSE);
        ShowWindow(g_ctl, SW_SHOW);
    }

    LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_COMMAND:
            switch (LOWORD(wp))
            {
            case IdPlay:
                if (!g_busy)
                    play();
                return 0;
            case IdQuit:
                readControls();
                saveSettings(g_settings);
                DestroyWindow(hwnd);
                return 0;
            case IdControls:
                openControls();
                return 0;
            case IdSaves:
                openFolder(g_user / L"memcard");
                return 0;
            case IdLogs:
                openFolder(g_user / L"logs");
                return 0;
            case IdDisc:
            {
                if (gameReady() && MessageBoxW(hwnd, L"The game's files are already set up. Copy them again from "
                                                     L"another disc image?", kTitle, MB_YESNO | MB_ICONQUESTION) != IDYES)
                    return 0;
                const std::wstring iso = pickIso();
                if (!iso.empty())
                    beginInstall(iso, false);
                return 0;
            }
            }
            break;
        case WM_HSCROLL:
            if (reinterpret_cast<HWND>(lp) == item(IdVolume))
                showVolume();
            return 0;
        case WmProgress:
            SendMessageW(item(IdProgress), PBM_SETPOS, wp, 0);
            status(L"Copying the game's files from the disc image... " + std::to_wstring(wp / 10) + L" %");
            return 0;
        case WmExtracted:
        {
            setBusy(false);
            ShowWindow(item(IdProgress), SW_HIDE);
            std::wstring *err = reinterpret_cast<std::wstring *>(lp);
            showGameState();
            if (err)
            {
                MessageBoxW(hwnd, err->c_str(), kTitle, MB_OK | MB_ICONERROR);
                delete err;
            }
            else if (wp)
                play();
            return 0;
        }
        case WmGameEnded:
            setBusy(false);
            showGameState();
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
            if (wp != 0)
            {
                const std::wstring text = L"The game ended unexpectedly (code " + std::to_wstring(wp) +
                                          L"). Please send the folder\n\n" + g_sessionLogs.wstring() +
                                          L"\n\nwith a note of what you were doing. Open it now?";
                if (MessageBoxW(hwnd, text.c_str(), kTitle, MB_YESNO | MB_ICONWARNING) == IDYES)
                    openFolder(g_sessionLogs);
            }
            return 0;
        case WM_CTLCOLORSTATIC:
        {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
        }
        case WM_CLOSE:
            if (g_busy)
                return 0;
            readControls();
            saveSettings(g_settings);
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int show)
{
    SetProcessDPIAware();
    wchar_t self[MAX_PATH * 4] = {};
    GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
    g_root = fs::path(self).parent_path();
    g_game = g_root / L"game";
    g_user = g_root / L"user";
    g_runner = g_root / L"bin" / L"ps2EntryRunner.exe";
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool reset = false, playNow = false;
    for (int i = 1; argv && i < argc; ++i)
    {
        const std::wstring a = argv[i];
        if (a == L"--iso" && i + 1 < argc)
            g_isoArg = argv[++i];
        else if (a == L"--reset")
            reset = true;
        else if (a == L"--play")
            playNow = true;
        else if (a.size() > 4 && _wcsicmp(a.c_str() + a.size() - 4, L".iso") == 0)
            g_isoArg = a;
    }
    if (!fs::exists(g_runner))
    {
        MessageBoxW(nullptr, L"bin\\ps2EntryRunner.exe is missing: unpack the whole package, keeping its folders.",
                    kTitle, MB_OK | MB_ICONERROR);
        return 1;
    }
    g_settings = loadSettings();
    if (reset)
    {
        std::error_code ec;
        fs::remove(g_game / kElfName, ec);
    }

    if (playNow)
    {
        if (!gameReady())
        {
            const std::wstring err = g_isoArg.empty() ? L"no game files and no --iso" : installFrom(g_isoArg, [](int) {});
            if (!err.empty())
            {
                MessageBoxW(nullptr, err.c_str(), kTitle, MB_OK | MB_ICONERROR);
                return 1;
            }
        }
        saveSettings(g_settings);
        fs::path logs;
        std::wstring err;
        HANDLE proc = startGame(g_settings, logs, err);
        if (!proc)
            return 1;
        WaitForSingleObject(proc, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(proc, &code);
        CloseHandle(proc);
        return static_cast<int>(code);
    }

    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&icc);
    HDC screen = GetDC(nullptr);
    g_dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(nullptr, screen);
    g_font = CreateFontW(-MulDiv(9, g_dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_bold = CreateFontW(-MulDiv(12, g_dpi, 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = L"RorLauncher";
    RegisterClassExW(&wc);
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r{0, 0, S(828), S(456)};
    AdjustWindowRect(&r, style, FALSE);
    const int w = r.right - r.left, h = r.bottom - r.top;
    g_wnd = CreateWindowExW(0, wc.lpszClassName, kTitle, style, (GetSystemMetrics(SM_CXSCREEN) - w) / 2,
                            (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h, nullptr, nullptr, inst, nullptr);
    build();
    ShowWindow(g_wnd, show);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0)
    {
        if (!(g_ctl && IsDialogMessageW(g_ctl, &m)) && !IsDialogMessageW(g_wnd, &m))
        {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    return 0;
}
