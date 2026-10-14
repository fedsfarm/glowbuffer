#pragma once

// Which clipboard and drag formats glowbuffer reads, adds or keeps. Pure logic, for
// tests/unit.cpp; main.cpp and Persist.cpp do the reading.

#include <cstddef>
#include <string>
#include <vector>

namespace Glowbuffer {

    inline constexpr const char* PORTAL_FILETRANSFER = "application/vnd.portal.filetransfer";
    // Where the file bridge accepts file lists from: file and archive managers, whose lists
    // name what the user picked. Window classes, space/comma separated, case-insensitive.
    // A browser is deliberately absent: web content can put any file:// URI in a drag, and
    // the bridge would hand that file to the sandboxed app it is dropped on.
    inline constexpr const char* DEFAULT_FILE_BRIDGE_SOURCES = "thunar nemo caja pcmanfm pcmanfm-qt nautilus org.gnome.nautilus dolphin org.kde.dolphin "
                                                              "spacefm file-roller org.gnome.fileroller ark org.kde.ark engrampa xarchiver";

    // KDE's convention, honoured by KeePassXC, Bitwarden and most password managers: a
    // clipboard carrying it holds a secret that clipboard managers must not store.
    inline constexpr const char* PASSWORD_HINT = "x-kde-passwordManagerHint";

    // ── keeping the clipboard after the app that copied closes ─────────────────────────────
    //
    // On Wayland the clipboard lives in the app that copied: close eog after "Copy" and there
    // is nothing to paste. glowbuffer reads a few formats the moment something is copied and,
    // if the app then goes away without anything replacing its clipboard, puts that copy back.
    // It reads only plain text, HTML, file lists and one image, never every format on offer:
    // reading everything is what makes clipboard-persistence tools break apps (expensive
    // conversions, apps that misbehave under many simultaneous reads).

    struct SPersistRead {
        std::string              mime;   // the one format read from the source
        std::vector<std::string> served; // every offered format answered with those bytes
        size_t                   maxBytes = 0;
    };

    // The reads to make for a new clipboard, or an empty plan for one that must not be kept
    // (a password manager's, or nothing worth keeping).
    std::vector<SPersistRead> planPersist(const std::vector<std::string>& mimes);

    // ── files from sandboxed apps to host apps ─────────────────────────────────────────────
    //
    // A sandboxed Qt app (Telegram) offers its files only as a FileTransfer portal key, no
    // text/uri-list. Host apps that read paths (Thunar, most file managers and editors) see
    // nothing they can take, so only the image formats some drags also carry ever land. For a
    // host reader glowbuffer adds a file list and fills it from FileTransfer.RetrieveFiles.
    bool offersOnlyPortalFiles(const std::vector<std::string>& mimes);

} // namespace Glowbuffer
