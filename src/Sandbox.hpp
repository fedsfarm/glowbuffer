#pragma once

// Which clients live in a Flatpak sandbox, and as which app.
//
// Flatpak puts a keyfile at /.flatpak-info inside every sandbox, and the host can read it
// through /proc/<pid>/root. xdg-desktop-portal identifies its callers the same way, so the
// app id found here is the one the document portal grants files to.

#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>

namespace Glowbuffer {

    // The [Application] name= value of a .flatpak-info keyfile.
    std::optional<std::string> parseFlatpakAppId(std::string_view keyfile);

    // Cheap enough for the compositor thread: one access() through procfs.
    bool isFlatpakPid(pid_t pid);

    // Positive evidence that a process is NOT sandboxed: its root directory AND its mount
    // namespace are the compositor's own. Absence of /.flatpak-info is not enough to trust a
    // process's paths (an unreadable /proc entry or a sandbox other than Flatpak would pass),
    // and the root alone is not either: firejail, `unshare -m`, `bwrap --bind / /` and
    // systemd's ProtectHome keep the host root but mount their own /home or /tmp over it.
    // Anything this cannot prove is treated as sandboxed.
    bool isHostPid(pid_t pid);

    // The app id of a sandboxed process; nullopt for a host process or an unreadable keyfile.
    std::optional<std::string> flatpakAppIdForPid(pid_t pid);

} // namespace Glowbuffer
