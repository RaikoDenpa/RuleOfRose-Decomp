#include "ror.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    struct File
    {
        FILE *f = nullptr;
        std::string path;
        uint32_t size = 0;
        uint32_t pos = 0;
        uint32_t ring = 0;
        uint32_t paused = 0;
        uint32_t ringBase = 0, readOff = 0, avail = 0, streamPos = 0, loopAt = 0;
        bool write = false;
    };

    File g_rx[64];
    File g_sce[64];

    fs::path discRoot()
    {
        static fs::path root = []
        {
            const char *env = std::getenv("ROR_DISC");
            fs::path p = (env && *env) ? fs::path(env) : PS2Runtime::getIoPaths().elfDirectory;
            ror::log("disc root %s", p.string().c_str());
            return p;
        }();
        return root;
    }

    std::string normalize(std::string p)
    {
        for (const char *dev : {"cdrom0:", "cdrom:", "host0:", "host:"})
        {
            const size_t n = std::strlen(dev);
            if (p.size() >= n && _strnicmp(p.c_str(), dev, n) == 0)
            {
                p = p.substr(n);
                break;
            }
        }
        if (const size_t semi = p.rfind(";1"); semi != std::string::npos && semi + 2 == p.size())
            p.resize(semi);
        for (char &c : p)
            if (c == '\\')
                c = '/';
        while (!p.empty() && p.front() == '/')
            p.erase(p.begin());
        return p;
    }

    bool resolve(const std::string &guestPath, fs::path &out)
    {
        static std::map<std::string, fs::path> cache;
        std::string key = normalize(guestPath);
        for (char &c : key)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (auto it = cache.find(key); it != cache.end())
        {
            out = it->second;
            return true;
        }
        fs::path cur = discRoot();
        size_t start = 0;
        while (start <= key.size())
        {
            const size_t slash = key.find('/', start);
            const std::string part = key.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
            start = slash == std::string::npos ? key.size() + 1 : slash + 1;
            if (part.empty() || part == ".")
                continue;
            std::error_code ec;
            bool found = false;
            for (const auto &e : fs::directory_iterator(cur, ec))
            {
                std::string name = e.path().filename().string();
                for (char &c : name)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (name == part)
                {
                    cur = e.path();
                    found = true;
                    break;
                }
            }
            if (!found)
                return false;
        }
        cache[key] = cur;
        out = cur;
        return true;
    }

    int openFile(File *table, int firstFd, const std::string &guestPath, const char *who)
    {
        fs::path host;
        if (!resolve(guestPath, host) || !fs::is_regular_file(host))
        {
            ror::log("%s %s: not on the disc", who, guestPath.c_str());
            return -1;
        }
        for (int fd = firstFd; fd < 64; ++fd)
        {
            if (table[fd].f)
                continue;
            FILE *f = _wfopen(host.c_str(), L"rb");
            if (!f)
                return -1;
            table[fd] = File{};
            table[fd].f = f;
            table[fd].path = guestPath;
            table[fd].size = static_cast<uint32_t>(fs::file_size(host));
            ror::log("%s %s -> fd %d (%u bytes)", who, guestPath.c_str(), fd, table[fd].size);
            return fd;
        }
        ror::log("%s %s: no free fd", who, guestPath.c_str());
        return -1;
    }

    File *get(File *table, int32_t fd) { return (fd >= 1 && fd < 64 && table[fd].f) ? &table[fd] : nullptr; }

    int32_t readFile(uint8_t *rdram, File *f, uint32_t buf, uint32_t size)
    {
        const uint32_t n = f->pos < f->size ? std::min(size, f->size - f->pos) : 0;
        uint8_t *dst = ror::ptr(rdram, buf, n);
        if (n && !dst)
        {
            ror::log("read %s: buffer %08x+%u is not guest RAM", f->path.c_str(), buf, n);
            return -1;
        }
        std::fseek(f->f, static_cast<long>(f->pos), SEEK_SET);
        const size_t got = n ? std::fread(dst, 1, n, f->f) : 0;
        f->pos += static_cast<uint32_t>(got);
        return static_cast<int32_t>(got);
    }

    void closeFile(File *f)
    {
        std::fclose(f->f);
        *f = File{};
    }
}

void ror_rxFileOpen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
{
    const std::string path = ror::str(rdram, ror::arg(ctx, 0));
    const uint32_t mode = ror::arg(ctx, 1);
    if (mode & 2)
    {
        ror::log("rxFileOpen %s mode %#x: writing is not supported", path.c_str(), mode);
        ror::ret(ctx, static_cast<uint32_t>(-1));
        return;
    }
    ror::ret(ctx, static_cast<uint32_t>(openFile(g_rx, 1, path, "rxFileOpen")));
}

void ror_rxFileOpenStream(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
{
    const int fd = openFile(g_rx, 1, ror::str(rdram, ror::arg(ctx, 0)), "rxFileOpenStream");
    if (fd > 0)
    {
        g_rx[fd].ring = ror::arg(ctx, 2);
        g_rx[fd].ringBase = ror::arg(ctx, 1);
    }
    ror::ret(ctx, static_cast<uint32_t>(fd));
}

void ror_rxFileClose(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    if (File *f = get(g_rx, ror::sarg(ctx, 0)))
        closeFile(f);
    ror::ret(ctx, 0);
}

void ror_rxFileRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
{
    File *f = get(g_rx, ror::sarg(ctx, 0));
    ror::ret(ctx, static_cast<uint32_t>(f ? readFile(rdram, f, ror::arg(ctx, 1), ror::arg(ctx, 2)) : -1));
}

void ror_rxFileWrite(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    ror::log("rxFileWrite fd %d: the disc is read-only", ror::sarg(ctx, 0));
    ror::ret(ctx, static_cast<uint32_t>(-1));
}

void ror_rxFileSeek(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    File *f = get(g_rx, ror::sarg(ctx, 0));
    if (!f)
    {
        ror::ret(ctx, static_cast<uint32_t>(-1));
        return;
    }
    const int32_t off = ror::sarg(ctx, 1);
    switch (ror::sarg(ctx, 2))
    {
    case 0:
        f->pos = static_cast<uint32_t>(off);
        break;
    case 1:
        f->pos += static_cast<uint32_t>(off);
        break;
    case 2:
        f->pos = f->size - static_cast<uint32_t>(off);
        break;
    default:
        break;
    }
    ror::ret(ctx, f->pos);
}

void ror_rxFileSync(uint8_t *, R5900Context *ctx, PS2Runtime *) { ror::ret(ctx, 0); }

void ror_rxStreamCmd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
{
    File *f = get(g_rx, ror::sarg(ctx, 0));
    const uint32_t count = ror::arg(ctx, 2);
    const uint8_t *cmd = ror::ptr(rdram, ror::arg(ctx, 1), 20 * std::max<uint32_t>(count, 1));
    if (f && cmd && count >= 1 && cmd[4] == 1)
        std::memcpy(&f->loopAt, cmd, 4);
    ror::ret(ctx, count);
}

namespace
{
    void fillRing(uint8_t *rdram, File *f)
    {
        if (!f->ringBase || !f->ring || f->paused || !ror::soundOn())
            return;
        while (f->avail < f->ring)
        {
            const uint32_t write = (f->readOff + f->avail) % f->ring;
            const uint32_t end = f->loopAt && f->loopAt <= f->size ? f->loopAt : f->size;
            if (f->streamPos >= end)
            {
                if (!f->loopAt || end == 0)
                    return;
                f->streamPos = 0;
            }
            const uint32_t n = std::min({f->ring - f->avail, f->ring - write, end - f->streamPos});
            uint8_t *dst = ror::ptr(rdram, f->ringBase + write, n);
            if (!dst)
                return;
            std::fseek(f->f, static_cast<long>(f->streamPos), SEEK_SET);
            const size_t got = std::fread(dst, 1, n, f->f);
            if (got == 0)
                return;
            f->streamPos += static_cast<uint32_t>(got);
            f->avail += static_cast<uint32_t>(got);
        }
    }
}

void ror_rxStreamPause(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    File *f = get(g_rx, ror::sarg(ctx, 0));
    uint32_t prev = 0;
    if (f)
    {
        prev = f->paused;
        f->paused = ror::arg(ctx, 1) & 0xff;
    }
    ror::ret(ctx, prev);
}

void ror_rxStreamReadPtr(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const File *f = get(g_rx, ror::sarg(ctx, 0));
    ror::ret(ctx, f ? f->ringBase + f->readOff : 0);
}

void ror_rxStreamReadPos(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const File *f = get(g_rx, ror::sarg(ctx, 0));
    ror::ret(ctx, f ? f->readOff : static_cast<uint32_t>(-1));
}

void ror_rxStreamRingSize(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    File *f = get(g_rx, ror::sarg(ctx, 0));
    ror::ret(ctx, f ? f->ring : 0);
}

void ror_rxStreamAvail(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
{
    File *f = get(g_rx, ror::sarg(ctx, 0));
    if (f)
        fillRing(rdram, f);
    ror::ret(ctx, f ? f->avail : 0);
}

void ror_rxStreamConsume(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    File *f = get(g_rx, ror::sarg(ctx, 0));
    const int32_t n = ror::sarg(ctx, 1);
    if (f && f->ring && n > 0)
    {
        const uint32_t used = std::min(static_cast<uint32_t>(n), f->avail);
        f->readOff = (f->readOff + used) % f->ring;
        f->avail -= used;
    }
    ror::ret(ctx, 0);
}

void ror_rxFileSizeByPath(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
{
    const std::string path = ror::str(rdram, ror::arg(ctx, 0));
    fs::path host;
    std::error_code ec;
    const bool ok = resolve(path, host) && fs::is_regular_file(host, ec);
    const uint32_t size = ok ? static_cast<uint32_t>(fs::file_size(host, ec)) : static_cast<uint32_t>(-1);
    ror::log("rxFileSize %s -> %d", path.c_str(), static_cast<int32_t>(size));
    ror::ret(ctx, size);
}

namespace
{
    int openWriteFile(const std::string &guestPath, uint32_t flags)
    {
        const bool host = _strnicmp(guestPath.c_str(), "host", 4) == 0;
        if (!host)
        {
            ror::log("sceOpen %s flags %#x: writing is only for host: paths", guestPath.c_str(), flags);
            return -1;
        }
        const fs::path out = fs::current_path() / "host" / fs::path(normalize(guestPath)).filename();
        std::error_code ec;
        fs::create_directories(out.parent_path(), ec);
        for (int fd = 3; fd < 64; ++fd)
        {
            if (g_sce[fd].f)
                continue;
            FILE *f = _wfopen(out.c_str(), (flags & 0x400) || !fs::exists(out, ec) ? L"w+b" : L"r+b");
            if (!f)
                return -1;
            g_sce[fd] = File{};
            g_sce[fd].f = f;
            g_sce[fd].path = guestPath;
            g_sce[fd].write = true;
            std::fseek(f, 0, SEEK_END);
            g_sce[fd].size = static_cast<uint32_t>(std::ftell(f));
            ror::log("sceOpen %s flags %#x -> fd %d, writing %s", guestPath.c_str(), flags, fd, out.string().c_str());
            return fd;
        }
        return -1;
    }
}

void ror_sceOpen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
{
    const std::string path = ror::str(rdram, ror::arg(ctx, 0));
    const uint32_t flags = ror::arg(ctx, 1);
    if ((flags & 3) != 1)
    {
        ror::ret(ctx, static_cast<uint32_t>(openWriteFile(path, flags)));
        return;
    }
    ror::ret(ctx, static_cast<uint32_t>(openFile(g_sce, 3, path, "sceOpen")));
}

void ror_sceClose(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    File *f = get(g_sce, ror::sarg(ctx, 0));
    if (f)
        closeFile(f);
    ror::ret(ctx, f ? 0u : static_cast<uint32_t>(-1));
}

void ror_sceLseek(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    File *f = get(g_sce, ror::sarg(ctx, 0));
    if (!f)
    {
        ror::ret(ctx, static_cast<uint32_t>(-1));
        return;
    }
    const int32_t off = ror::sarg(ctx, 1);
    const int32_t whence = ror::sarg(ctx, 2);
    const int64_t base = whence == 0 ? 0 : whence == 1 ? f->pos : f->size;
    f->pos = static_cast<uint32_t>(std::max<int64_t>(0, base + off));
    ror::ret(ctx, f->pos);
}

void ror_sceRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
{
    File *f = get(g_sce, ror::sarg(ctx, 0));
    ror::ret(ctx, static_cast<uint32_t>(f ? readFile(rdram, f, ror::arg(ctx, 1), ror::arg(ctx, 2)) : -1));
}

void ror_sceWrite(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
{
    const int32_t fd = ror::sarg(ctx, 0);
    const uint32_t n = ror::arg(ctx, 2);
    if (fd == 1 || fd == 2)
    {
        const uint8_t *p = ror::ptr(rdram, ror::arg(ctx, 1), n);
        std::string s = p ? std::string(reinterpret_cast<const char *>(p), n) : std::string();
        while (!s.empty() && (s.back() == '\n' || s.back() == '\0'))
            s.pop_back();
        ror::log("tty%d: %s", fd, s.c_str());
        ror::ret(ctx, n);
        return;
    }
    File *f = get(g_sce, fd);
    const uint8_t *src = f && f->write ? ror::ptr(rdram, ror::arg(ctx, 1), n) : nullptr;
    if (!f || !f->write || (!src && n))
    {
        ror::log("sceWrite fd %d (%u bytes): not a file opened for writing", fd, n);
        ror::ret(ctx, static_cast<uint32_t>(-1));
        return;
    }
    std::fseek(f->f, static_cast<long>(f->pos), SEEK_SET);
    const size_t put = n ? std::fwrite(src, 1, n, f->f) : 0;
    f->pos += static_cast<uint32_t>(put);
    f->size = std::max(f->size, f->pos);
    ror::ret(ctx, static_cast<uint32_t>(put));
}

std::string ror::discFile(const std::string &guestPath)
{
    fs::path p;
    return resolve(guestPath, p) ? p.string() : std::string();
}
