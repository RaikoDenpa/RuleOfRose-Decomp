#include "ror.h"

#include <cstdlib>
#include <fstream>
#include <map>
#include <string>

namespace
{
    constexpr uint32_t kIntroSeen = 0x2ac1cc;

    const std::map<std::string, std::string> &options()
    {
        static const std::map<std::string, std::string> opts = []
        {
            std::map<std::string, std::string> m;
            const char *env = std::getenv("ROR_PORT_INI");
            const std::string path = env && *env ? env : "port.ini";
            std::ifstream in(path);
            for (std::string line; std::getline(in, line);)
            {
                if (const size_t hash = line.find('#'); hash != std::string::npos)
                    line.resize(hash);
                const size_t eq = line.find('=');
                if (eq == std::string::npos)
                    continue;
                auto trim = [](std::string s)
                {
                    s.erase(0, s.find_first_not_of(" \t\r"));
                    s.erase(s.find_last_not_of(" \t\r") + 1);
                    return s;
                };
                m[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
            }
            if (in.is_open() || !m.empty())
                for (const auto &[k, v] : m)
                    ror::log("option %s = %s (%s)", k.c_str(), v.c_str(), path.c_str());
            return m;
        }();
        return opts;
    }

    bool flag(const char *name)
    {
        const auto it = options().find(name);
        return it != options().end() && !it->second.empty() && it->second[0] == '1';
    }
}

namespace ror
{
    std::string option(const char *name, const char *fallback)
    {
        const auto it = options().find(name);
        return it != options().end() && !it->second.empty() ? it->second : std::string(fallback);
    }

    int optionInt(const char *name, int fallback)
    {
        const std::string v = option(name, "");
        char *end = nullptr;
        const long n = v.empty() ? fallback : std::strtol(v.c_str(), &end, 10);
        return v.empty() || (end && *end) ? fallback : static_cast<int>(n);
    }

    void optionsVblank(uint8_t *rdram)
    {
        static const bool skipIntro = flag("skip_intro");
        if (skipIntro && frame() < 400 && rd32(rdram, kIntroSeen) == 0)
            wr32(rdram, kIntroSeen, 1);
    }
}
