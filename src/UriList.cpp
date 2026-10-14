#include "UriList.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace Glowbuffer {

    bool iequals(std::string_view a, std::string_view b) {
        return a.size() == b.size() &&
            std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower((unsigned char)x) == std::tolower((unsigned char)y); });
    }

    static bool istartsWith(std::string_view s, std::string_view prefix) {
        return s.size() >= prefix.size() && iequals(s.substr(0, prefix.size()), prefix);
    }

    std::optional<EListKind> listKindForMime(std::string_view mime) {
        // mime parameters ("text/uri-list;charset=utf-8") do not change the format
        if (const auto SEMI = mime.find(';'); SEMI != std::string_view::npos)
            mime = mime.substr(0, SEMI);
        while (!mime.empty() && mime.back() == ' ')
            mime.remove_suffix(1);

        if (iequals(mime, "text/uri-list") || iequals(mime, "application/x-kde4-urilist"))
            return EListKind::URI_LIST;
        if (iequals(mime, "x-special/gnome-copied-files"))
            return EListKind::GNOME_COPIED_FILES;
        return std::nullopt;
    }

    static int hexValue(char c) {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    }

    std::optional<std::string> fileUriToPath(std::string_view uri) {
        if (!istartsWith(uri, "file:") || uri.find('\0') != std::string_view::npos)
            return std::nullopt;
        uri.remove_prefix(5);

        if (uri.starts_with("//")) {
            uri.remove_prefix(2);
            const auto SLASH = uri.find('/');
            if (SLASH == std::string_view::npos)
                return std::nullopt;
            const auto HOST = uri.substr(0, SLASH);
            // A file on another machine is not ours to export.
            if (!HOST.empty() && !iequals(HOST, "localhost"))
                return std::nullopt;
            uri.remove_prefix(SLASH);
        }

        if (!uri.starts_with('/'))
            return std::nullopt;

        // A raw '#' or '?' is a fragment or a query, never part of a local path (GLib's
        // g_filename_from_uri refuses them too). Such a line is left exactly as it came.
        if (uri.find_first_of("#?") != std::string_view::npos)
            return std::nullopt;

        std::string path;
        path.reserve(uri.size());
        for (size_t i = 0; i < uri.size(); ++i) {
            if (uri[i] != '%') {
                path += uri[i];
                continue;
            }
            if (i + 2 >= uri.size())
                return std::nullopt;
            const int HI = hexValue(uri[i + 1]), LO = hexValue(uri[i + 2]);
            if (HI < 0 || LO < 0)
                return std::nullopt;
            const char C = static_cast<char>(HI * 16 + LO);
            if (C == '\0')
                return std::nullopt;
            path += C;
            i += 2;
        }
        return path;
    }

    // GLib's UNSAFE_PATH set: unreserved characters plus the sub-delimiters a path may carry
    // literally. Everything else, and every byte >= 0x80, is percent-encoded.
    static bool keepLiteral(unsigned char c) {
        if (std::isalnum(c) && c < 0x80)
            return true;
        switch (c) {
            case '-':
            case '.':
            case '_':
            case '~':
            case '!':
            case '$':
            case '&':
            case '\'':
            case '(':
            case ')':
            case '*':
            case '+':
            case ',':
            case ':':
            case '=':
            case '@':
            case '/': return true;
            default: return false;
        }
    }

    std::string pathToFileUri(std::string_view path) {
        static constexpr char HEX[] = "0123456789ABCDEF";
        std::string           out   = "file://";
        out.reserve(out.size() + path.size() * 3);
        for (const unsigned char C : path) {
            if (keepLiteral(C)) {
                out += static_cast<char>(C);
                continue;
            }
            out += '%';
            out += HEX[C >> 4];
            out += HEX[C & 0xF];
        }
        return out;
    }

    std::string fileList(EListKind kind, const std::vector<std::string>& paths) {
        std::string out = kind == EListKind::GNOME_COPIED_FILES ? "copy" : "";
        for (const auto& p : paths) {
            if (kind == EListKind::GNOME_COPIED_FILES)
                out += "\n" + pathToFileUri(p);
            else
                out += pathToFileUri(p) + "\r\n";
        }
        return out;
    }

    std::vector<SListLine> parseList(std::string_view data, EListKind kind) {
        std::vector<SListLine> lines;
        size_t                 pos = 0;
        while (pos < data.size()) {
            const auto NL = data.find('\n', pos);
            SListLine  line;
            if (NL == std::string_view::npos) {
                line.text = std::string{data.substr(pos)};
                pos       = data.size();
            } else {
                line.text = std::string{data.substr(pos, NL - pos)};
                line.eol  = "\n";
                pos       = NL + 1;
            }
            if (!line.eol.empty() && line.text.ends_with('\r')) {
                line.text.pop_back();
                line.eol = "\r\n";
            }

            const bool HEADER  = kind == EListKind::GNOME_COPIED_FILES && lines.empty() && (line.text == "copy" || line.text == "cut");
            const bool COMMENT = kind == EListKind::URI_LIST && line.text.starts_with('#');
            if (!HEADER && !COMMENT)
                line.path = fileUriToPath(line.text);

            lines.emplace_back(std::move(line));
        }
        return lines;
    }

    std::vector<std::string> listedPaths(const std::vector<SListLine>& lines) {
        std::vector<std::string>        paths;
        std::unordered_set<std::string> seen;
        for (const auto& l : lines) {
            if (l.path && seen.insert(*l.path).second)
                paths.push_back(*l.path);
        }
        return paths;
    }

} // namespace Glowbuffer
