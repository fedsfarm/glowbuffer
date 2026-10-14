#include "Bridge.hpp"
#include "PasteImage.hpp"
#include "Sandbox.hpp"

#include <systemd/sd-bus.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <ctime>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <poll.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace Glowbuffer::Bridge {

    namespace {

        using Clock = std::chrono::steady_clock;

        // Qt gives a drop source one second of silence before it gives up on the pipe
        // (QWaylandMimeData::readData). Reading the source and asking the portal normally
        // takes milliseconds; these budgets only bound a source or a portal that hangs.
        constexpr auto     READ_BUDGET       = std::chrono::milliseconds(2000);
        constexpr auto     WRITE_BUDGET      = std::chrono::milliseconds(2000);
        constexpr uint64_t DBUS_TIMEOUT_USEC = 2'000'000;
        // A file list is a few hundred bytes per file; 4 MiB is tens of thousands of files.
        constexpr size_t MAX_LIST = 4 * 1024 * 1024;
        // An image can be large and a browser can be slow to encode it.
        constexpr size_t MAX_IMAGE    = 64 * 1024 * 1024;
        // The paste-image directory is a tmpfs (RAM). A client that keeps asking a file manager
        // window for its image (or claims to be one: window classes are not authenticated)
        // must not be able to fill it: past this, the oldest saved images go first.
        constexpr size_t IMAGE_DIR_QUOTA = 256 * 1024 * 1024;
        constexpr auto   IMAGE_BUDGET = std::chrono::milliseconds(5000);
        // Saved images are copied by the file manager the moment it reads the list; an hour
        // covers a slow copy, and the directory is a tmpfs emptied at logout anyway.
        constexpr auto IMAGE_KEEP = std::chrono::hours(1);
        // Worker threads in flight, per kind. Every path that starts one is reachable by any
        // client asking for data, so each kind is capped; past the cap the target gets an
        // empty transfer. Heavy work buffers up to MAX_IMAGE in compositor memory.
        constexpr int MAX_LIGHT = 16; // file lists, portal keys, writing kept clipboards
        constexpr int MAX_HEAVY = 2;  // reading clipboards to keep, saving pasted images

        constexpr const char* PORTAL_DEST  = "org.freedesktop.portal.Documents";
        constexpr const char* PORTAL_PATH  = "/org/freedesktop/portal/documents";
        constexpr const char* PORTAL_IFACE = "org.freedesktop.portal.Documents";

        // AddFull flags, from org.freedesktop.portal.Documents.xml
        constexpr uint32_t FLAG_REUSE_EXISTING   = 1;
        constexpr uint32_t FLAG_AS_NEEDED_BY_APP = 4; // skip files the app can already read
        constexpr uint32_t FLAG_EXPORT_DIRECTORY = 8;

        std::mutex              g_lock;
        std::condition_variable g_idle;
        int                     g_light    = 0;
        int                     g_heavy    = 0;
        bool                    g_stopping = false;
        SStats                  g_stats;

        void record(const std::string& error, bool exported, uint64_t files) {
            std::scoped_lock l(g_lock);
            ++g_stats.transfers;
            if (!error.empty()) {
                ++g_stats.failures;
                g_stats.lastError = error;
            }
            if (exported)
                g_stats.exported += files;
            else
                ++g_stats.passthrough;
        }

        void closeFd(int fd) {
            if (fd >= 0)
                close(fd);
        }

        // A pidfd still naming a live process (signal 0 checks without sending). Without a
        // pidfd there is nothing to check against: the kernel was too old for SO_PEERPIDFD.
        bool stillAlive(int pidfd) {
            if (pidfd < 0)
                return true;
            return syscall(SYS_pidfd_send_signal, pidfd, 0, nullptr, 0) == 0;
        }

        struct SFd {
            int fd = -1;
            explicit SFd(int f) : fd(f) {}
            ~SFd() {
                if (fd >= 0)
                    close(fd);
            }
            SFd(const SFd&)            = delete;
            SFd& operator=(const SFd&) = delete;
        };

        bool readAll(int fd, std::string& out, std::chrono::milliseconds budget = READ_BUDGET, size_t max = MAX_LIST) {
            const auto END = Clock::now() + budget;
            char       buf[16384];
            while (true) {
                const auto LEFT = std::chrono::duration_cast<std::chrono::milliseconds>(END - Clock::now()).count();
                if (LEFT <= 0)
                    return false;
                pollfd    p{.fd = fd, .events = POLLIN, .revents = 0};
                const int R = poll(&p, 1, static_cast<int>(LEFT));
                if (R < 0 && errno == EINTR)
                    continue;
                if (R <= 0)
                    return false;
                const ssize_t N = read(fd, buf, sizeof buf);
                if (N < 0 && (errno == EINTR || errno == EAGAIN))
                    continue;
                if (N < 0)
                    return false;
                if (N == 0)
                    return true;
                out.append(buf, static_cast<size_t>(N));
                if (out.size() > max)
                    return false;
            }
        }

        bool writeAll(int fd, std::string_view data) {
            // Our copy of the target's write end is ours alone once the target closed its own,
            // which every toolkit does right after wl_data_offer.receive.
            const int FL = fcntl(fd, F_GETFL);
            if (FL >= 0)
                fcntl(fd, F_SETFL, FL | O_NONBLOCK);

            const auto END = Clock::now() + WRITE_BUDGET;
            while (!data.empty()) {
                const auto LEFT = std::chrono::duration_cast<std::chrono::milliseconds>(END - Clock::now()).count();
                if (LEFT <= 0)
                    return false;
                pollfd    p{.fd = fd, .events = POLLOUT, .revents = 0};
                const int R = poll(&p, 1, static_cast<int>(LEFT));
                if (R < 0 && errno == EINTR)
                    continue;
                if (R <= 0)
                    return false;
                const ssize_t N = write(fd, data.data(), data.size());
                if (N < 0 && (errno == EINTR || errno == EAGAIN))
                    continue;
                if (N < 0) {
                    // EPIPE: the target stopped reading. SIGPIPE is blocked on this thread
                    // (see run()), so the signal is pending here, not killing the compositor;
                    // take it off the queue.
                    if (errno == EPIPE) {
                        sigset_t        pipe;
                        const timespec  ZERO{};
                        sigemptyset(&pipe);
                        sigaddset(&pipe, SIGPIPE);
                        sigtimedwait(&pipe, nullptr, &ZERO);
                    }
                    return false;
                }
                data.remove_prefix(static_cast<size_t>(N));
            }
            return true;
        }

        // The name the document portal will show the file under: the basename of the inode's
        // real path. That differs from the listed name when the list names a symlink.
        std::string realName(int fd) {
            char          buf[PATH_MAX];
            const auto    LINK = "/proc/self/fd/" + std::to_string(fd);
            const ssize_t N    = readlink(LINK.c_str(), buf, sizeof buf - 1);
            if (N <= 0)
                return {};
            std::string path{buf, static_cast<size_t>(N)};
            if (path.ends_with(" (deleted)"))
                return {};
            while (path.size() > 1 && path.ends_with('/'))
                path.pop_back();
            const auto SLASH = path.rfind('/');
            return SLASH == std::string::npos ? path : path.substr(SLASH + 1);
        }

        struct SMsgUnref {
            void operator()(sd_bus_message* m) const {
                sd_bus_message_unref(m);
            }
        };
        using UPMsg = std::unique_ptr<sd_bus_message, SMsgUnref>;

        struct SBusClose {
            void operator()(sd_bus* b) const {
                sd_bus_flush_close_unref(b);
            }
        };
        using UPBus = std::unique_ptr<sd_bus, SBusClose>;

        std::string busError(int r, const sd_bus_error& e) {
            if (e.message)
                return e.message;
            return std::strerror(-r);
        }

        std::string readMountpoint(sd_bus_message* m) {
            const void* bytes = nullptr;
            size_t      len   = 0;
            if (sd_bus_message_read_array(m, 'y', &bytes, &len) < 0 || !bytes)
                return {};
            const auto* S = static_cast<const char*>(bytes);
            return std::string{S, strnlen(S, len)};
        }

        // AddFull(ah o_path_fds, u flags, s app_id, as permissions) -> (as doc_ids, a{sv} extra_out)
        bool addFull(sd_bus* bus, const std::vector<int>& fds, uint32_t flags, const std::string& app, std::vector<std::string>& ids, std::string& mountpoint,
                     std::string& err) {
            sd_bus_message* raw = nullptr;
            int             r   = sd_bus_message_new_method_call(bus, &raw, PORTAL_DEST, PORTAL_PATH, PORTAL_IFACE, "AddFull");
            UPMsg           call{raw};
            if (r < 0) {
                err = std::string{"AddFull: "} + std::strerror(-r);
                return false;
            }

            r = sd_bus_message_open_container(call.get(), 'a', "h");
            for (const int FD : fds) {
                if (r >= 0)
                    r = sd_bus_message_append(call.get(), "h", FD); // sd-bus dups the fd
            }
            if (r >= 0)
                r = sd_bus_message_close_container(call.get());
            if (r >= 0)
                r = sd_bus_message_append(call.get(), "us", flags, app.c_str());
            // Read-only, always. The drop hands the app one file to read, not a write handle on
            // the user's home: that is the "keep the sandbox's permissions" half of the job.
            if (r >= 0)
                r = sd_bus_message_append(call.get(), "as", 1, "read");
            if (r < 0) {
                err = std::string{"AddFull: building the call: "} + std::strerror(-r);
                return false;
            }

            sd_bus_error e = SD_BUS_ERROR_NULL;
            raw            = nullptr;
            r              = sd_bus_call(bus, call.get(), DBUS_TIMEOUT_USEC, &e, &raw);
            UPMsg reply{raw};
            if (r < 0) {
                err = "AddFull: " + busError(r, e);
                sd_bus_error_free(&e);
                return false;
            }
            sd_bus_error_free(&e);

            char** strv = nullptr;
            if (sd_bus_message_read_strv(reply.get(), &strv) < 0 || !strv) {
                err = "AddFull: unreadable reply";
                return false;
            }
            for (char** s = strv; *s; ++s) {
                ids.emplace_back(*s);
                free(*s);
            }
            free(strv);

            // extra_out carries the mountpoint, sparing a GetMountPoint round trip.
            if (sd_bus_message_enter_container(reply.get(), 'a', "{sv}") > 0) {
                while (sd_bus_message_enter_container(reply.get(), 'e', "sv") > 0) {
                    const char* key = nullptr;
                    if (sd_bus_message_read(reply.get(), "s", &key) < 0)
                        break;
                    if (key && std::strcmp(key, "mountpoint") == 0 && sd_bus_message_enter_container(reply.get(), 'v', "ay") > 0) {
                        mountpoint = readMountpoint(reply.get());
                        sd_bus_message_exit_container(reply.get());
                    } else
                        sd_bus_message_skip(reply.get(), "v");
                    sd_bus_message_exit_container(reply.get());
                }
                sd_bus_message_exit_container(reply.get());
            }

            if (ids.size() != fds.size()) {
                err = "AddFull: reply has " + std::to_string(ids.size()) + " ids for " + std::to_string(fds.size()) + " files";
                return false;
            }
            return true;
        }

        std::string getMountpoint(sd_bus* bus) {
            sd_bus_error    e   = SD_BUS_ERROR_NULL;
            sd_bus_message* raw = nullptr;
            const int       R   = sd_bus_call_method(bus, PORTAL_DEST, PORTAL_PATH, PORTAL_IFACE, "GetMountPoint", &e, &raw, "");
            UPMsg           reply{raw};
            sd_bus_error_free(&e);
            return R < 0 ? std::string{} : readMountpoint(reply.get());
        }

        struct SOpened {
            std::string path; // as listed
            int         fd = -1;
            std::string name;
        };

        // A kernel passes at most SCM_MAX_FD (253) descriptors in one message, and D-Bus
        // brokers cap them too: a 300-file drag in one AddFull fails whole. Batches stay
        // under it, and only one batch of fds is open at a time.
        constexpr size_t ADDFULL_BATCH = 200;

        // path as listed -> path the app can open.
        // Paths come from a host app (main.cpp refuses any other source), so they are opened as
        // the host sees them. Never through another process's /proc/<pid>/root: the rest of
        // such a path, its ".." and its absolute symlinks, resolve in the compositor's root,
        // which let a sandboxed source name any host file.
        std::unordered_map<std::string, std::string> exportToApp(const std::vector<std::string>& paths, const std::string& app, std::string& err) {
            std::unordered_map<std::string, std::string> mapping;
            UPBus                                        bus;
            std::string                                  mountpoint;

            const auto EXPORT = [&](std::vector<SOpened>& group, uint32_t flags) {
                if (group.empty() || !err.empty())
                    return;
                if (!bus) {
                    sd_bus*   raw = nullptr;
                    const int R   = sd_bus_open_user(&raw);
                    bus.reset(raw);
                    if (R < 0) {
                        err = std::string{"session bus: "} + std::strerror(-R);
                        return;
                    }
                }
                std::vector<int> fds;
                for (const auto& o : group)
                    fds.push_back(o.fd);
                std::vector<std::string> ids;
                if (!addFull(bus.get(), fds, flags, app, ids, mountpoint, err))
                    return;
                if (mountpoint.empty())
                    mountpoint = getMountpoint(bus.get());
                if (mountpoint.empty()) {
                    err = "document portal reported no mountpoint";
                    return;
                }
                for (size_t i = 0; i < group.size(); ++i) {
                    // An empty id: the app can already read this file where it is
                    // (FLAG_AS_NEEDED_BY_APP), so its path stays as listed.
                    if (!ids[i].empty())
                        mapping[group[i].path] = mountpoint + "/" + ids[i] + "/" + group[i].name;
                }
            };

            for (size_t at = 0; at < paths.size() && err.empty(); at += ADDFULL_BATCH) {
                std::vector<SOpened> files, dirs;
                for (size_t i = at; i < std::min(paths.size(), at + ADDFULL_BATCH); ++i) {
                    const int FD = open(paths[i].c_str(), O_PATH | O_CLOEXEC);
                    if (FD < 0)
                        continue;
                    struct stat st{};
                    const auto  NAME = realName(FD);
                    if (fstat(FD, &st) != 0 || NAME.empty() || !(S_ISREG(st.st_mode) || S_ISDIR(st.st_mode))) {
                        close(FD);
                        continue;
                    }
                    (S_ISDIR(st.st_mode) ? dirs : files).push_back({.path = paths[i], .fd = FD, .name = NAME});
                }
                EXPORT(files, FLAG_REUSE_EXISTING | FLAG_AS_NEEDED_BY_APP);
                EXPORT(dirs, FLAG_REUSE_EXISTING | FLAG_AS_NEEDED_BY_APP | FLAG_EXPORT_DIRECTORY);
                for (const auto& o : files)
                    close(o.fd);
                for (const auto& o : dirs)
                    close(o.fd);
            }
            return mapping;
        }

        // The rest of a list too slow or too large to rewrite, copied through untouched after
        // what was already read, so the target still gets the source's bytes, all of them.
        void forwardRest(int from, int to, std::string_view already) {
            if (!writeAll(to, already))
                return;
            const auto END = Clock::now() + std::chrono::seconds(60);
            char       buf[65536];
            while (Clock::now() < END) {
                pollfd    p{.fd = from, .events = POLLIN, .revents = 0};
                const int R = poll(&p, 1, 1000);
                if (R < 0 && errno == EINTR)
                    continue;
                if (R < 0)
                    return;
                if (R == 0)
                    continue;
                const ssize_t N = read(from, buf, sizeof buf);
                if (N < 0 && (errno == EINTR || errno == EAGAIN))
                    continue;
                if (N <= 0 || !writeAll(to, std::string_view{buf, static_cast<size_t>(N)}))
                    return;
            }
        }

        void blockSigpipe() {
            // A target that closes its pipe early would raise SIGPIPE on write(), and
            // SIGPIPE's default action ends the process: the compositor. Block it on this
            // thread only.
            sigset_t pipe;
            sigemptyset(&pipe);
            sigaddset(&pipe, SIGPIPE);
            pthread_sigmask(SIG_BLOCK, &pipe, nullptr);
        }

        void run(SJob job) {
            blockSigpipe();

            SFd         from{job.fromSource}, to{job.toTarget}, pidfd{job.targetPidfd};
            std::string data, err;
            const bool  COMPLETE = readAll(from.fd, data);
            std::string out      = data;
            uint64_t    files    = 0;

            if (!COMPLETE) {
                forwardRest(from.fd, to.fd, data);
                record("a file list slower than 2 s or over 4 MiB was forwarded unchanged", false, 0);
                return;
            }
            {
                const auto LINES = parseList(data, job.kind);
                const auto PATHS = listedPaths(LINES);
                if (!PATHS.empty()) {
                    const auto APP = flatpakAppIdForPid(job.targetPid);
                    if (APP && !stillAlive(pidfd.fd))
                        err = "target pid " + std::to_string(job.targetPid) + " exited before the grant";
                    else if (APP) {
                        const auto MAPPING = exportToApp(PATHS, *APP, err);
                        files              = MAPPING.size();
                        if (files)
                            out = serializeList(LINES, [&](const std::string& p) -> std::optional<std::string> {
                                const auto IT = MAPPING.find(p);
                                return IT == MAPPING.end() ? std::nullopt : std::optional<std::string>{IT->second};
                            });
                    } else
                        err = "target pid " + std::to_string(job.targetPid) + " has no readable .flatpak-info";
                }
            }

            if (!writeAll(to.fd, out) && err.empty())
                err = "target stopped reading";
            record(err, files > 0, files);
        }

        // The directory, opened without following a symlink and only if it is ours and private.
        // Everything below works relative to this fd, so a directory swapped for a symlink after
        // the check changes nothing.
        int openPrivateDir(const std::string& dir, std::string& err) {
            if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
                err = "cannot create " + dir + ": " + std::strerror(errno);
                return -1;
            }
            const int FD = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (FD < 0) {
                err = "cannot open " + dir + " (a symlink?): " + std::strerror(errno);
                return -1;
            }
            struct stat st{};
            if (fstat(FD, &st) != 0 || st.st_uid != getuid() || (st.st_mode & 077) != 0) {
                close(FD);
                err = dir + " is not a private directory of this user; not saving images there";
                return -1;
            }
            return FD;
        }

        // Drop images older than IMAGE_KEEP, then the oldest until `incoming` more bytes fit the
        // quota. Regular files only, never followed. False when it cannot fit at all.
        bool makeRoom(int dirfd, size_t incoming) {
            struct SEntry {
                std::string name;
                time_t      mtime;
                size_t      size;
            };
            std::vector<SEntry> entries;
            const int           DUP = dup(dirfd);
            if (DUP < 0)
                return false;
            DIR* d = fdopendir(DUP);
            if (!d) {
                close(DUP);
                return false;
            }
            rewinddir(d);
            const time_t NOW = std::time(nullptr);
            const auto   KEEP = std::chrono::duration_cast<std::chrono::seconds>(IMAGE_KEEP).count();
            size_t       total = 0;
            while (const dirent* e = readdir(d)) {
                struct stat st{};
                if (fstatat(dirfd, e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(st.st_mode))
                    continue;
                if (NOW - st.st_mtime > KEEP) {
                    unlinkat(dirfd, e->d_name, 0);
                    continue;
                }
                entries.push_back({e->d_name, st.st_mtime, static_cast<size_t>(st.st_size)});
                total += static_cast<size_t>(st.st_size);
            }
            closedir(d);

            std::ranges::sort(entries, {}, &SEntry::mtime);
            for (const auto& e : entries) {
                if (total + incoming <= IMAGE_DIR_QUOTA)
                    break;
                if (unlinkat(dirfd, e.name.c_str(), 0) == 0)
                    total -= e.size;
            }
            return total + incoming <= IMAGE_DIR_QUOTA;
        }

        // Create the file exclusively: two pastes in the same second get "(2)", never each
        // other's bytes, and never a planted symlink's target.
        std::string saveImage(const std::string& dir, const std::string& ext, const std::string& data, std::string& err) {
            SFd dirfd{openPrivateDir(dir, err)};
            if (dirfd.fd < 0)
                return {};
            if (!makeRoom(dirfd.fd, data.size())) {
                err = "no room for the pasted image in " + dir;
                return {};
            }
            const auto BASE = pastedImageName(std::time(nullptr), ext);
            const auto DOT  = BASE.rfind('.');
            for (int n = 1; n <= 20; ++n) {
                const auto NAME = n == 1 ? BASE : BASE.substr(0, DOT) + " (" + std::to_string(n) + ")" + BASE.substr(DOT);
                const auto PATH = dir + "/" + NAME;
                // 0644: the file manager may copy the mode along; the directory is 0700.
                const int FD = openat(dirfd.fd, NAME.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
                if (FD < 0 && errno == EEXIST)
                    continue;
                if (FD < 0) {
                    err = "cannot create " + PATH + ": " + std::strerror(errno);
                    return {};
                }
                size_t off = 0;
                while (off < data.size()) {
                    const ssize_t W = write(FD, data.data() + off, data.size() - off);
                    if (W < 0 && errno == EINTR)
                        continue;
                    if (W <= 0)
                        break;
                    off += static_cast<size_t>(W);
                }
                const bool OK = off == data.size() && close(FD) == 0;
                if (!OK) {
                    unlinkat(dirfd.fd, NAME.c_str(), 0);
                    err = "cannot write " + PATH;
                    return {};
                }
                return PATH;
            }
            err = "too many pasted images this second";
            return {};
        }

        void runImage(SImageJob job) {
            blockSigpipe();
            SFd         from{job.fromSource}, to{job.toTarget};
            std::string data, err, out;
            if (!readAll(from.fd, data, IMAGE_BUDGET, MAX_IMAGE) || data.empty())
                err = "the clipboard image could not be read (empty, over 64 MiB, or too slow)";
            else if (const auto PATH = saveImage(job.dir, job.extension, data, err); !PATH.empty()) {
                out = fileListFor(job.kind, PATH);
                if (job.result) {
                    std::scoped_lock l(job.result->lock);
                    job.result->path = PATH;
                }
            }

            // On failure the file manager gets an empty list: the same "nothing to paste" it
            // showed before glowbuffer, not a half-written file.
            writeAll(to.fd, out);

            std::scoped_lock l(g_lock);
            if (err.empty())
                ++g_stats.images;
            else {
                ++g_stats.failures;
                g_stats.lastError = err;
            }
        }

        // FileTransfer.RetrieveFiles(s key, a{sv} options) -> (as files)
        std::vector<std::string> retrieveFiles(const std::string& key, std::string& err) {
            std::vector<std::string> files;
            sd_bus*                  rawBus = nullptr;
            if (const int R = sd_bus_open_user(&rawBus); R < 0) {
                err = std::string{"session bus: "} + std::strerror(-R);
                return files;
            }
            UPBus           bus{rawBus};
            sd_bus_error    e   = SD_BUS_ERROR_NULL;
            sd_bus_message* raw = nullptr;
            const int       R   = sd_bus_call_method(bus.get(), PORTAL_DEST, PORTAL_PATH, "org.freedesktop.portal.FileTransfer", "RetrieveFiles", &e, &raw, "sa{sv}",
                                                     key.c_str(), 0);
            UPMsg           reply{raw};
            if (R < 0) {
                err = "RetrieveFiles: " + busError(R, e);
                sd_bus_error_free(&e);
                return files;
            }
            sd_bus_error_free(&e);
            char** strv = nullptr;
            if (sd_bus_message_read_strv(reply.get(), &strv) < 0 || !strv) {
                err = "RetrieveFiles: unreadable reply";
                return files;
            }
            for (char** s = strv; *s; ++s) {
                files.emplace_back(*s);
                free(*s);
            }
            free(strv);
            return files;
        }

        void runPortalList(SPortalJob job) {
            blockSigpipe();
            SFd         from{job.fromSource}, to{job.toTarget};
            std::string key, err, out;
            if (!readAll(from.fd, key) || key.empty())
                err = "the FileTransfer key could not be read";
            else {
                // Qt writes the key as text; trailing NULs or newlines are not part of it
                while (!key.empty() && (key.back() == '\0' || key.back() == '\n' || key.back() == '\r'))
                    key.pop_back();
                const auto FILES = retrieveFiles(key, err);
                if (!FILES.empty())
                    out = fileList(job.kind, FILES);
                else if (err.empty())
                    err = "the FileTransfer key named no files";
            }
            writeAll(to.fd, out);

            std::scoped_lock l(g_lock);
            if (err.empty())
                ++g_stats.portalLists;
            else {
                ++g_stats.failures;
                g_stats.lastError = err;
            }
        }

        enum class EWork : uint8_t {
            LIGHT,
            HEAVY,
        };

        // Start `work` on a detached thread unless its kind is at its cap (or shutting down);
        // on refusal every fd in `fds` is closed, which the other end reads as an empty transfer.
        template <typename F>
        bool spawn(EWork kind, const std::vector<int>& fds, F&& work) {
            const bool HEAVY = kind == EWork::HEAVY;
            {
                std::scoped_lock l(g_lock);
                int&       active = HEAVY ? g_heavy : g_light;
                if (g_stopping || active >= (HEAVY ? MAX_HEAVY : MAX_LIGHT)) {
                    for (const int FD : fds)
                        closeFd(FD);
                    return false;
                }
                ++active;
            }
            try {
                std::thread([HEAVY, work = std::forward<F>(work)]() mutable {
                    // An exception escaping a thread function is std::terminate: the whole
                    // compositor, for a bad_alloc on a 64 MiB image.
                    try {
                        work();
                    } catch (const std::exception& e) {
                        std::scoped_lock l(g_lock);
                        ++g_stats.failures;
                        g_stats.lastError = std::string{"worker failed: "} + e.what();
                    } catch (...) {
                        std::scoped_lock l(g_lock);
                        ++g_stats.failures;
                        g_stats.lastError = "worker failed";
                    }
                    std::scoped_lock l(g_lock);
                    --(HEAVY ? g_heavy : g_light);
                    g_idle.notify_all();
                }).detach();
                return true;
            } catch (...) {
                for (const int FD : fds)
                    closeFd(FD);
                std::scoped_lock l(g_lock);
                --(HEAVY ? g_heavy : g_light);
                ++g_stats.failures;
                g_stats.lastError = "could not start a worker thread";
                return false;
            }
        }

    } // namespace

    void start() {
        std::scoped_lock l(g_lock);
        g_stopping = false;
    }

    bool accepting() {
        std::scoped_lock l(g_lock);
        return !g_stopping && g_light < MAX_LIGHT;
    }

    void submit(SJob job) {
        spawn(EWork::LIGHT, {job.fromSource, job.toTarget, job.targetPidfd}, [job] { run(job); });
    }

    void submitImage(SImageJob job) {
        spawn(EWork::HEAVY, {job.fromSource, job.toTarget}, [job] { runImage(job); });
    }

    void submitPortalList(SPortalJob job) {
        spawn(EWork::LIGHT, {job.fromSource, job.toTarget}, [job] { runPortalList(job); });
    }

    void readMany(std::vector<SRead> reads, ReadDone done) {
        std::vector<int> fds;
        for (const auto& r : reads)
            fds.push_back(r.fd);
        spawn(EWork::HEAVY, fds, [reads = std::move(reads), done = std::move(done)]() mutable {
            blockSigpipe();
            std::vector<std::pair<std::string, std::string>> results;
            for (auto& r : reads) {
                SFd         fd{r.fd};
                std::string data;
                // Budget per format: a source answers them one by one, in order.
                if (readAll(fd.fd, data, IMAGE_BUDGET, r.maxBytes) && !data.empty())
                    results.emplace_back(r.mime, std::move(data));
            }
            done(std::move(results));
        });
    }

    void writeLater(int fd, std::shared_ptr<const std::string> data) {
        spawn(EWork::LIGHT, {fd}, [fd, data = std::move(data)] {
            blockSigpipe();
            SFd to{fd};
            writeAll(to.fd, data ? std::string_view{*data} : std::string_view{});
        });
    }

    void shutdown(std::chrono::milliseconds wait) {
        std::unique_lock l(g_lock);
        g_stopping = true;
        g_idle.wait_for(l, wait, [] { return g_light == 0 && g_heavy == 0; });
    }

    SStats stats() {
        std::scoped_lock l(g_lock);
        return g_stats;
    }

} // namespace Glowbuffer::Bridge
