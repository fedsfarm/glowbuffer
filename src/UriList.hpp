#pragma once

// Parsing and rewriting the file lists a drag or a paste carries. Pure logic, no Hyprland,
// so tests/unit.cpp covers it directly.
//
// Why rewrite at all: a Flatpak app with no filesystem permission receives
// file:///home/you/Videos/clip.mp4 and cannot open it. Apps that speak the FileTransfer
// portal avoid that when the *source* offers application/vnd.portal.filetransfer, but most
// sources do not (Thunar's drags offer text/uri-list alone, so do browsers' download bars,
// terminals and most X11-era apps). The bridge (Bridge.cpp) exports each listed file to the
// receiving app through the document portal and hands it the portal path instead, which is
// what the FileTransfer portal would have produced. Only file:// entries change; links,
// comments and line endings pass through byte for byte.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Glowbuffer {

    enum class EListKind {
        URI_LIST,            // text/uri-list (RFC 2483), application/x-kde4-urilist
        GNOME_COPIED_FILES,  // x-special/gnome-copied-files: "copy" or "cut", then one URI per line
    };

    // ASCII case-insensitive equality: mime types and window classes.
    bool iequals(std::string_view a, std::string_view b);

    // The list format of a mime type, or nullopt for anything glowbuffer leaves alone.
    std::optional<EListKind> listKindForMime(std::string_view mime);

    // file:///a%20b, file://localhost/a%20b and file:/a%20b -> "/a b". nullopt for other schemes,
    // remote hosts, relative paths, malformed escapes and embedded NULs, raw or escaped (a
    // NUL would cut the path short at open() and name a different file than the list shows).
    std::optional<std::string> fileUriToPath(std::string_view uri);

    // "/a b" -> file:///a%20b, escaping exactly what GLib's g_filename_to_uri() escapes, so a
    // rewritten list reads the way GTK and Qt write their own.
    std::string pathToFileUri(std::string_view path);

    // A list naming `paths`, in the format a reader asked for: gnome-copied-files is "copy"
    // and one URI per line; text/uri-list is one URI per CRLF-terminated line (RFC 2483).
    std::string fileList(EListKind kind, const std::vector<std::string>& paths);

    struct SListLine {
        std::string                text; // the line without its terminator
        std::string                eol;  // "\r\n", "\n" or "" (last line, unterminated)
        std::optional<std::string> path; // set when the line is a local file URI
    };

    std::vector<SListLine> parseList(std::string_view data, EListKind kind);

    // Every distinct local path the list names, in order of first appearance.
    std::vector<std::string> listedPaths(const std::vector<SListLine>& lines);

    // The list again, with each file line whose path `map` replaces rewritten to the new path.
    // Lines `map` does not cover come out exactly as they went in.
    template <typename F>
    std::string serializeList(const std::vector<SListLine>& lines, F&& map) {
        std::string out;
        for (const auto& l : lines) {
            std::optional<std::string> replacement;
            if (l.path)
                replacement = map(*l.path);
            out += replacement ? pathToFileUri(*replacement) : l.text;
            out += l.eol;
        }
        return out;
    }

} // namespace Glowbuffer
