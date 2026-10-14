#pragma once

// The file bridge: hands a sandboxed app the files a drag or a paste names, read-only, through
// the document portal, without widening its Flatpak permissions.
//
// It runs OFF the compositor thread. The compositor only creates a pipe, asks the source to
// write its list into it, and submits a job; a worker reads the list, exports the files with
// org.freedesktop.portal.Documents.AddFull and writes the rewritten list to the target. A
// D-Bus round trip on the compositor thread would freeze the whole desktop while the portal
// starts. Every failure falls back to forwarding the source's bytes unchanged, which is exactly
// what Hyprland does without the plugin.

#include "UriList.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include <sys/types.h>

namespace Glowbuffer::Bridge {

    struct SJob {
        int       fromSource = -1; // read end of the pipe the source writes its list into
        int       toTarget   = -1; // the fd the target passed to wl_data_offer.receive
        EListKind kind       = EListKind::URI_LIST;
        pid_t     targetPid  = 0;
        // A pidfd for targetPid (SO_PEERPIDFD), or -1. The app id is read through
        // /proc/<pid>; the grant goes ahead only if this still names a live process then, so
        // a reused pid can never receive another process's grant. Owned by the job.
        int targetPidfd = -1;
        // The source is always a host app (main.cpp checks): its paths are host paths.
    };

    // PLUGIN_INIT. The image is never unmapped (-z nodelete), so a reload after an unload runs
    // in the same image with the statics of the previous load, shutdown flag included.
    void start();

    // Takes ownership of every fd in the job, always: on refusal they are closed, which the
    // target reads as an empty transfer. Refused while too many transfers are in flight.
    void submit(SJob job);

    // An image pasted into a file manager (PasteImage.hpp): the worker saves the image the
    // source writes into `fromSource` under `dir` and answers the target with a file list.
    struct SSavedImage {
        std::mutex  lock;
        std::string path; // set once saved
    };
    struct SImageJob {
        int                          fromSource = -1;
        int                          toTarget   = -1;
        EListKind                    kind       = EListKind::GNOME_COPIED_FILES;
        std::string                  extension;
        std::string                  dir; // $XDG_RUNTIME_DIR/glowbuffer: tmpfs, private, gone at logout
        std::shared_ptr<SSavedImage> result;
    };
    void submitImage(SImageJob job);

    // A sandboxed app's FileTransfer key, turned into a file list for a host app: the worker
    // reads the key the source writes into `fromSource`, asks the portal for the files
    // (FileTransfer.RetrieveFiles answers a host caller with their paths) and writes the
    // list to the target. See ClipboardPlan.hpp.
    struct SPortalJob {
        int       fromSource = -1;
        int       toTarget   = -1;
        EListKind kind       = EListKind::URI_LIST;
    };
    void submitPortalList(SPortalJob job);

    // Clipboard keeping (Persist.cpp): read each pipe to EOF, up to its cap, then hand the
    // results to `done` ON THE WORKER THREAD. `done` must not touch compositor state.
    struct SRead {
        std::string mime;
        int         fd       = -1;
        size_t      maxBytes = 0;
    };
    using ReadDone = std::function<void(std::vector<std::pair<std::string, std::string>>)>;
    void readMany(std::vector<SRead> reads, ReadDone done);

    // Write `data` to `fd` (a paste target's pipe) off the compositor thread, then close it.
    void writeLater(int fd, std::shared_ptr<const std::string> data);

    // False while too many transfers are in flight; the caller then forwards directly.
    bool accepting();

    // Waits up to `wait` for in-flight transfers and refuses new ones. The worker code stays
    // mapped after unload (the plugin links with -z nodelete), so a transfer that outlives the
    // wait finishes safely; this only keeps PLUGIN_EXIT from returning mid-transfer.
    void shutdown(std::chrono::milliseconds wait);

    struct SStats {
        uint64_t    transfers   = 0; // lists that went through a worker
        uint64_t    exported    = 0; // files handed to an app through the document portal
        uint64_t    passthrough = 0; // lists forwarded unchanged (no files, host app, failure)
        uint64_t    failures    = 0; // portal or I/O errors, each also a passthrough
        uint64_t    images      = 0; // images saved for a file manager's paste
        uint64_t    portalLists = 0; // FileTransfer keys turned into file lists
        std::string lastError;
    };
    SStats stats();

} // namespace Glowbuffer::Bridge
