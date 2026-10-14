#include "Sandbox.hpp"

#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace Glowbuffer {

    static std::string_view trim(std::string_view s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
            s.remove_prefix(1);
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
            s.remove_suffix(1);
        return s;
    }

    std::optional<std::string> parseFlatpakAppId(std::string_view keyfile) {
        bool   inApplication = false;
        size_t pos           = 0;
        while (pos <= keyfile.size()) {
            const auto NL   = keyfile.find('\n', pos);
            const auto LINE = trim(keyfile.substr(pos, NL == std::string_view::npos ? std::string_view::npos : NL - pos));
            pos             = NL == std::string_view::npos ? keyfile.size() + 1 : NL + 1;

            if (LINE.empty() || LINE.starts_with('#'))
                continue;
            if (LINE.starts_with('[')) {
                inApplication = LINE == "[Application]";
                continue;
            }
            if (!inApplication)
                continue;

            const auto EQ = LINE.find('=');
            if (EQ == std::string_view::npos || trim(LINE.substr(0, EQ)) != "name")
                continue;
            const auto VALUE = trim(LINE.substr(EQ + 1));
            if (VALUE.empty())
                return std::nullopt;
            return std::string{VALUE};
        }
        return std::nullopt;
    }

    static std::string infoPath(pid_t pid) {
        return "/proc/" + std::to_string(pid) + "/root/.flatpak-info";
    }

    bool isFlatpakPid(pid_t pid) {
        return pid > 0 && access(infoPath(pid).c_str(), F_OK) == 0;
    }

    static bool sameFile(const std::string& a, const char* b) {
        struct stat sa{}, sb{};
        return stat(a.c_str(), &sa) == 0 && stat(b, &sb) == 0 && sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
    }

    bool isHostPid(pid_t pid) {
        if (pid <= 0)
            return false;
        const auto PROC = "/proc/" + std::to_string(pid);
        return sameFile(PROC + "/root", "/proc/self/root") && sameFile(PROC + "/ns/mnt", "/proc/self/ns/mnt");
    }

    std::optional<std::string> flatpakAppIdForPid(pid_t pid) {
        if (pid <= 0)
            return std::nullopt;

        // A process controls what sits at its own root: never block on a FIFO there, never
        // follow a symlink out of it, read only a regular file.
        const int FD = open(infoPath(pid).c_str(), O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK | O_NOFOLLOW);
        if (FD < 0)
            return std::nullopt;
        struct stat st{};
        if (fstat(FD, &st) != 0 || !S_ISREG(st.st_mode)) {
            close(FD);
            return std::nullopt;
        }

        // The keyfile is a few hundred bytes; anything past 64 KiB is not a .flatpak-info.
        std::string buf(64 * 1024, '\0');
        size_t      len = 0;
        while (len < buf.size()) {
            const ssize_t N = read(FD, buf.data() + len, buf.size() - len);
            if (N <= 0)
                break;
            len += static_cast<size_t>(N);
        }
        close(FD);
        buf.resize(len);
        return parseFlatpakAppId(buf);
    }

} // namespace Glowbuffer
