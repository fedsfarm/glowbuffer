#pragma once

// Paste an image into a file manager as a new file.
//
// A screenshot or a browser's "Copy image" puts image/png on the clipboard and nothing else.
// Nautilus and Dolphin paste that as "Pasted image.png"; Thunar, Nemo, Caja and PCManFM
// paste file lists only, see none, and report "There is nothing on the clipboard to paste".
// For those apps, and only those (a chat app offered a file list would attach a file instead
// of pasting the picture), glowbuffer adds a file list to an image-only clipboard and writes
// the image to a file when the file manager asks for it. Pure logic here, for tests/unit.cpp.

#include "UriList.hpp"

#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Glowbuffer {

    // The default set: file managers that paste file lists only. Window classes, matched
    // case-insensitively, separated by spaces or commas.
    inline constexpr const char* DEFAULT_PASTE_IMAGE_APPS = "thunar nemo caja pcmanfm pcmanfm-qt";

    // The image type to save when the clipboard holds an image and no file list: image/png
    // when offered, else the first image/* type, spelled exactly as offered (a source answers
    // only the strings it offered). nullopt when there is a file list already (the file
    // manager pastes that) or no image at all.
    std::optional<std::string> pickImageMime(const std::vector<std::string>& mimes);

    // "image/jpeg" -> "jpg", "image/svg+xml" -> "svg"; a sanitized subtype otherwise.
    std::string imageExtension(std::string_view mime);

    // Whether a window class is in a space/comma separated list, ignoring case.
    bool classInList(std::string_view windowClass, std::string_view list);

    // "Pasted image 2026-10-14 23-20-01.png", local time. No ':' (a path separator elsewhere).
    std::string pastedImageName(std::time_t when, std::string_view extension);

    // The list a file manager reads for one pasted file, in the format it asked for.
    std::string fileListFor(EListKind kind, std::string_view path);

} // namespace Glowbuffer
