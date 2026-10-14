// glowbuffer — drag and drop and paste that work on Hyprland, sandboxed apps included.
//
// One function hook, on CWLDataOfferResource::sendData(). Hyprland calls it right after it
// creates a wl_data_offer, for drags (CWLDataDeviceProtocol::updateDrag) and for the clipboard
// (sendSelectionToDevice) alike, and both call sites call it out of line in the 0.56.2 binary,
// so the hook sees every offer. The replacement does what the original did, minus the early
// hardcoded MOVE action, and installs two request handlers on the offer:
//
//   set_actions  the action negotiation Hyprland never implemented (see Actions.hpp)
//   receive      the file bridge for sandboxed targets (see Bridge.hpp), images pasted into
//                file managers (PasteImage.hpp) and a sandboxed app's FileTransfer files
//                for host apps (ClipboardPlan.hpp); every other request is forwarded to
//                the source exactly as Hyprland forwards it
//   accept       only on drag offers that carry a format glowbuffer added, so the source
//                hears the format it actually has
//
// A second hook, on CSeatManager::setCurrentSelection, lets Persist.cpp keep the clipboard
// after the app that copied closes (and tell that apart from a deliberate clear).
//
// The handlers live in Hyprland's objects but their code lives in this image, so the image must
// outlive every offer it touched: CMakeLists.txt links with -z nodelete and PLUGIN_EXIT only
// disarms them (g_armed). Unloading stops new offers being hooked; it never unmaps the code an
// existing clipboard offer would call on the next paste.

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/plugins/HookSystem.hpp>
#include <hyprland/src/protocols/core/DataDevice.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/config/values/types/BoolValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/SharedDefs.hpp>

#include <wayland-server-core.h>
#include <linux/input-event-codes.h>

#include <algorithm>
#include <atomic>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <format>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <unistd.h>

#include "Access.hpp"
#include "Actions.hpp"
#include "Bridge.hpp"
#include "ClipboardPlan.hpp"
#include "Persist.hpp"
#include "PasteImage.hpp"
#include "Sandbox.hpp"
#include "UriList.hpp"

#ifndef GLOWBUFFER_VERSION
#define GLOWBUFFER_VERSION "0.1.0"
#endif

using namespace Glowbuffer;
using Hyprutils::OS::CFileDescriptor;

static_assert(MOD_SHIFT == HL_MODIFIER_SHIFT && MOD_CTRL == HL_MODIFIER_CTRL, "Hyprland renumbered its modifier bits; update Actions.hpp");
static_assert(ACTION_COPY == WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY && ACTION_MOVE == WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE &&
              ACTION_ASK == WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK);

// ── private member access ─────────────────────────────────────────────────────────────────
// CWLDataOfferResource keeps its wl_data_offer wrapper private (only CWLDataDeviceResource is
// a friend), and every request handler and event of the offer goes through it. An explicit
// template instantiation may name a private member; this is the standard-conforming way to
// reach it, without `#define private public` leaking into every Hyprland header.
namespace {
    template <typename Tag, typename Tag::type MEMBER>
    struct SRob {
        friend typename Tag::type robbed(Tag) {
            return MEMBER;
        }
    };
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-template-friend"
    struct SOfferResource {
        using type = SP<CWlDataOffer> CWLDataOfferResource::*;
        friend type robbed(SOfferResource);
    };
    // The source's own wrapper, for its client. CWLDataSourceResource::m_device is no help:
    // Hyprland fills it from the manager the source was created on, which is empty for a
    // client that created its source before (or without) its data device.
    struct SSourceResource {
        using type = SP<CWlDataSource> CWLDataSourceResource::*;
        friend type robbed(SSourceResource);
    };
#pragma GCC diagnostic pop
    template struct SRob<SOfferResource, &CWLDataOfferResource::m_resource>;
    template struct SRob<SSourceResource, &CWLDataSourceResource::m_resource>;

    SP<CWlDataOffer>& wlOffer(CWLDataOfferResource* offer) {
        return offer->*robbed(SOfferResource{});
    }

}

wl_client* Glowbuffer::sourceClient(IDataSource* source) {
    auto* wl = dynamic_cast<CWLDataSourceResource*>(source);
    if (!wl)
        return nullptr;
    const auto& res = wl->*robbed(SSourceResource{});
    return res && res->resource() ? wl_resource_get_client(res->resource()) : nullptr;
}

// ── state ─────────────────────────────────────────────────────────────────────────────────
namespace {
    using origSendData     = void (*)(CWLDataOfferResource*);
    using origSetSelection = void (*)(CSeatManager*, SP<IDataSource>);

    // One per drag offer, owned by the offer's own request handlers (they capture it), so it
    // dies with the wl_data_offer and never needs cleaning up.
    struct SOfferState {
        WP<CWLDataOfferResource> offer;
        uint32_t              version         = 0;
        uint32_t              targetActions   = 0;
        uint32_t              targetPreferred = 0;
        uint32_t              announced       = ACTION_NONE; // last wl_data_offer.action sent
        // After the drop the action is settled: only a target resolving "ask" may still pick
        // one (protocol), and a modifier pressed while the target reads must not turn a copy
        // into a move behind the user's back (the source would delete the originals).
        bool dropped = false;
    };

    struct SGlobal {
        HANDLE         handle        = nullptr;
        CFunctionHook* hook          = nullptr; // CWLDataOfferResource::sendData
        CFunctionHook* selectionHook = nullptr; // CSeatManager::setCurrentSelection

        SP<Config::Values::CBoolValue>   negotiate, modifiers, fileBridge, pasteImages, persist;
        SP<Config::Values::CStringValue> pasteImageApps, fileBridgeSources;

        CHyprSignalListener   keyListener;
        // the drop: Hyprland ends a drag on any pointer button release, touch up or tablet tip up
        CHyprSignalListener buttonListener, touchListener, tipListener;
        SP<SHyprCtlCommand>   ctl;

        // The offer under the pointer in the drag in progress, and what its source was last told.
        std::weak_ptr<SOfferState> current;
        WP<IDataSource>            announcedSource;
        uint32_t                   sourceAnnounced = ACTION_NONE;

        // the last image saved for a file manager's paste (pasteImage)
        struct {
            WP<IDataSource>                      source;
            std::string                          mime;
            std::shared_ptr<Bridge::SSavedImage> result;
        } savedImage;

        uint64_t offersNegotiated = 0;
        uint64_t actionsSent      = 0;
    };

    // Never freed: request handlers installed on live offers read it after PLUGIN_EXIT.
    SGlobal*          g = nullptr;
    std::atomic<bool> g_armed{false};

    bool enabled(const SP<Config::Values::CBoolValue>& v) {
        return g_armed.load() && v && v->value();
    }
}

// ── action negotiation ────────────────────────────────────────────────────────────────────
namespace {
    // Shift and Control as they are right now. getModsFromAllKBs() is the state BEFORE the key
    // being processed, so a key event passes the key it carries to fold in.
    uint32_t heldModifiers(const IKeyboard::SKeyEvent* pending = nullptr) {
        uint32_t mods = g_pInputManager ? g_pInputManager->getModsFromAllKBs() : 0;
        if (!pending)
            return mods;
        uint32_t bit = 0;
        switch (pending->keycode) {
            case KEY_LEFTSHIFT:
            case KEY_RIGHTSHIFT: bit = MOD_SHIFT; break;
            case KEY_LEFTCTRL:
            case KEY_RIGHTCTRL: bit = MOD_CTRL; break;
            default: return mods;
        }
        return pending->state == WL_KEYBOARD_KEY_STATE_PRESSED ? (mods | bit) : (mods & ~bit);
    }

    void negotiate(const std::shared_ptr<SOfferState>& st, uint32_t mods) {
        const auto OFFER = st ? st->offer.lock() : nullptr;
        if (!OFFER || OFFER->m_dead)
            return;
        const auto SOURCE = OFFER->m_source.lock();
        auto&      res    = wlOffer(OFFER.get());
        if (!SOURCE || !res || !res->resource())
            return;

        const uint32_t ACTION = chooseAction({
            .sourceActions   = SOURCE->actions(),
            .targetActions   = st->targetActions,
            .targetPreferred = st->targetPreferred,
            .targetVersion   = st->version,
            .forced          = enabled(g->modifiers) ? compositorAction(mods) : ACTION_NONE,
        });

        if (st->version >= OFFER_ACTIONS_SINCE_VERSION && ACTION != st->announced) {
            res->sendAction(ACTION);
            st->announced = ACTION;
            ++g->actionsSent;
        }

        // wl_data_source.action: the source learns what the drop will do (its cursor, and
        // whether a finished MOVE means it should delete). Hyprland never sent this at all.
        if (g->announcedSource.lock() != SOURCE) {
            g->announcedSource = SOURCE;
            g->sourceAnnounced = ACTION_NONE;
        }
        if (ACTION != g->sourceAnnounced) {
            SOURCE->sendDndAction(static_cast<wl_data_device_manager_dnd_action>(ACTION));
            g->sourceAnnounced = ACTION;
        }
    }

    void onDragEnd() {
        if (const auto ST = g->current.lock(); ST)
            ST->dropped = true;
        g->current.reset();
    }

    void onKey(const IKeyboard::SKeyEvent& e) {
        if (!enabled(g->negotiate) || !enabled(g->modifiers))
            return;
        if (e.keycode != KEY_LEFTSHIFT && e.keycode != KEY_RIGHTSHIFT && e.keycode != KEY_LEFTCTRL && e.keycode != KEY_RIGHTCTRL)
            return;
        if (const auto ST = g->current.lock(); ST)
            negotiate(ST, heldModifiers(&e));
    }

    // Hyprland's sendData, minus the action it sent before enter. Offers, source_actions and
    // their order are unchanged.
    void sendOffersAndNegotiate(CWLDataOfferResource* self, const SP<IDataSource>& source, SP<CWlDataOffer>& res) {
        const uint32_t ACTIONS = source->actions();
        if (static_cast<uint32_t>(res->version()) >= OFFER_ACTIONS_SINCE_VERSION && ACTIONS > 0)
            res->sendSourceActions(ACTIONS);

        const auto MIMES = source->mimes();
        for (const auto& m : MIMES)
            res->sendOffer(m.c_str());

        if (!source->hasDnd())
            return;

        auto st     = std::make_shared<SOfferState>();
        st->offer   = self->m_self;
        st->version = static_cast<uint32_t>(res->version());
        res->setSetActions([st](CWlDataOffer*, uint32_t actions, uint32_t preferred) {
            if (!g_armed.load())
                return;
            if (st->dropped && st->announced != ACTION_ASK)
                return;
            st->targetActions   = actions;
            st->targetPreferred = preferred;
            negotiate(st, heldModifiers());
        });

        g->current = st;
        ++g->offersNegotiated;
        // Before the target answers this is NONE for a v3 target (nothing agreed yet, and the
        // source hears so) and COPY for an older one, which cannot negotiate at all.
        negotiate(st, heldModifiers());

        Log::logger->log(Log::DEBUG, "[glowbuffer] drag offer {:x}: {} mimes, source actions {}, target v{}", (uintptr_t)self, MIMES.size(), ACTIONS, st->version);
    }
}

// ── file bridge ───────────────────────────────────────────────────────────────────────────
namespace {
    pid_t clientPid(wl_client* client) {
        pid_t pid = 0;
        if (client)
            wl_client_get_credentials(client, &pid, nullptr, nullptr);
        return pid;
    }

    // The class of the window a client shows, "" for a client with none (or an Xwayland one,
    // whose selection never reaches a wl_data_offer).
    std::string windowClass(wl_client* client) {
        for (const auto& w : Desktop::windowState()->windows()) {
            if (!w || w->m_isX11)
                continue;
            const auto SURFACE = w->resource();
            if (SURFACE && SURFACE->client() == client)
                return w->m_initialClass.empty() ? w->m_class : w->m_initialClass;
        }
        return {};
    }

    // A pidfd for the client's process, from the socket (Linux 6.5+), or -1.
    int peerPidfd(wl_client* client) {
#ifndef SO_PEERPIDFD
#define SO_PEERPIDFD 77
#endif
        int       pidfd = -1;
        socklen_t len   = sizeof pidfd;
        if (getsockopt(wl_client_get_fd(client), SOL_SOCKET, SO_PEERPIDFD, &pidfd, &len) != 0)
            return -1;
        return pidfd;
    }

    // isHostPid for a client, with its pid pinned: SO_PEERCRED is the pid at connect time, and
    // a process that passed its socket on and exited leaves that pid free for reuse. While the
    // pidfd names a live process the pid is still that process.
    bool isHostClient(wl_client* client) {
        const int  PIDFD = peerPidfd(client);
        const bool HOST  = isHostPid(clientPid(client)) && (PIDFD < 0 || syscall(SYS_pidfd_send_signal, PIDFD, 0, nullptr, 0) == 0);
        if (PIDFD >= 0)
            close(PIDFD);
        return HOST;
    }

    // A format glowbuffer adds to an offer, and what answers it.
    struct SInject {
        enum EKind : uint8_t {
            IMAGE_AS_FILE,  // an image-only clipboard, for a file manager (PasteImage.hpp)
            PORTAL_AS_LIST, // a sandboxed app's FileTransfer key, for a host app (ClipboardPlan.hpp)
        } kind;
        std::string              sourceMime; // what the source really offers and is read
        std::vector<std::string> offered;    // the formats added
    };

    std::optional<SInject> injectionFor(const SP<IDataSource>& source, wl_client* target) {
        const auto MIMES = source->mimes();

        // Files from a sandboxed app's key: the real files, original names and quality, so this
        // wins over the image copy the same drag may carry.
        // Real paths go only to a reader proven to be a host app.
        if (enabled(g->fileBridge) && offersOnlyPortalFiles(MIMES) && isHostClient(target)) {
            const auto KEY = std::ranges::find_if(MIMES, [](const std::string& m) { return m == PORTAL_FILETRANSFER; });
            if (KEY != MIMES.end())
                return SInject{.kind       = SInject::PORTAL_AS_LIST,
                               .sourceMime = *KEY,
                               .offered    = source->hasDnd() ? std::vector<std::string>{"text/uri-list"} :
                                                                std::vector<std::string>{"x-special/gnome-copied-files", "text/uri-list"}};
        }

        if (!source->hasDnd() && enabled(g->pasteImages) && g->pasteImageApps) {
            const auto IMAGE = pickImageMime(MIMES);
            if (IMAGE && classInList(windowClass(target), g->pasteImageApps->value()))
                return SInject{.kind = SInject::IMAGE_AS_FILE, .sourceMime = *IMAGE, .offered = {"x-special/gnome-copied-files", "text/uri-list"}};
        }
        return std::nullopt;
    }

    // A file list for a host app, from the FileTransfer key a sandboxed source writes.
    void portalList(const SP<IDataSource>& source, const std::string& keyMime, EListKind kind, CFileDescriptor& targetFd) {
        int p[2];
        if (pipe2(p, O_CLOEXEC) != 0)
            return; // targetFd closes: an empty list, as before
        source->send(keyMime, CFileDescriptor{p[1]});
        Bridge::submitPortalList({.fromSource = p[0], .toTarget = targetFd.take(), .kind = kind});
    }

    // Answer a file manager's request for a file list with a saved copy of the image. One
    // file per clipboard: a file manager that reads the list again (pasting twice, a
    // can-paste probe) gets the same file, not a re-encoded copy eating the quota.
    void pasteImage(const SP<IDataSource>& source, const std::string& imageMime, EListKind kind, CFileDescriptor& targetFd) {
        auto& saved = g->savedImage;
        if (saved.result && saved.source.lock() == source && saved.mime == imageMime) {
            std::string path;
            {
                std::scoped_lock l(saved.result->lock);
                path = saved.result->path;
            }
            if (!path.empty() && access(path.c_str(), F_OK) == 0) {
                Bridge::writeLater(targetFd.take(), std::make_shared<const std::string>(fileListFor(kind, path)));
                return;
            }
        }

        const char* runtime = std::getenv("XDG_RUNTIME_DIR");
        int         p[2];
        if (!runtime || !*runtime || pipe2(p, O_CLOEXEC) != 0)
            return; // targetFd closes: the file manager reads an empty list, as before
        source->send(imageMime, CFileDescriptor{p[1]});
        auto result = std::make_shared<Bridge::SSavedImage>();
        saved       = {.source = source, .mime = imageMime, .result = result};
        Bridge::submitImage({
            .fromSource = p[0],
            .toTarget   = targetFd.take(),
            .kind       = kind,
            .extension  = imageExtension(imageMime),
            .dir        = std::string{runtime} + "/glowbuffer",
            .result     = result,
        });
    }

    // Hand the transfer to a worker when the target is a Flatpak app reading a file list from
    // another client. Returns false to let the caller forward it the way Hyprland does.
    bool bridge(CWlDataOffer* r, const SP<IDataSource>& source, const std::string& mime, CFileDescriptor& targetFd) {
        if (!enabled(g->fileBridge))
            return false;
        const auto KIND = listKindForMime(mime);
        if (!KIND)
            return false;

        const pid_t TARGET = clientPid(r->client());
        if (!isFlatpakPid(TARGET))
            return false;

        // The source must be a host app proven to be one (its root is ours), and a file or
        // archive manager (file_bridge_sources), whose lists name what the user picked:
        //  * a sandboxed source's paths resolve in its own mount namespace, and opening them
        //    from here through /proc/<pid>/root lets its "..", and its absolute symlinks,
        //    reach any host file: it could hand another app the user's SSH key;
        //  * a browser's drag carries whatever URIs web content put in it, file:// included;
        //  * Xwayland, data-control sources and kept clipboards have no live process to check.
        // Everything else is forwarded unchanged, as Hyprland forwards it.
        wl_client* const CLIENT = sourceClient(source.get()); // nullptr for every non-app source
        if (!CLIENT || CLIENT == r->client())
            return false;
        if (!isHostClient(CLIENT))
            return false;
        if (!g->fileBridgeSources || !classInList(windowClass(CLIENT), g->fileBridgeSources->value()))
            return false;

        if (!Bridge::accepting())
            return false;

        int p[2];
        if (pipe2(p, O_CLOEXEC) != 0)
            return false;

        // The source writes into our pipe; Hyprland's wrapper closes our copy of the write end
        // once the fd is sent, so the worker sees EOF when the source is done.
        source->send(mime, CFileDescriptor{p[1]});
        Bridge::submit({
            .fromSource      = p[0],
            .toTarget        = targetFd.take(),
            .kind            = *KIND,
            .targetPid       = TARGET,
            .targetPidfd     = peerPidfd(r->client()),
        });
        return true;
    }

    // Hyprland's receive handler, line for line, with the bridge in front of the forward.
    // `self` is captured raw exactly as Hyprland captures `this`: the handler is owned by the
    // offer's own resource and cannot outlive it.
    void installReceive(CWLDataOfferResource* self, SP<CWlDataOffer>& res, std::optional<SInject> inject) {
        res->setReceive([self, inject](CWlDataOffer* r, const char* mime, int fd) {
            CFileDescriptor sendFd{fd};
            const auto      SOURCE = self->m_source.lock();
            if (!SOURCE) {
                Log::logger->log(Log::WARN, "[glowbuffer] Possible bug: Receive on an offer w/o a source");
                return;
            }
            if (self->m_dead) {
                Log::logger->log(Log::WARN, "[glowbuffer] Possible bug: Receive on an offer that's dead");
                return;
            }

            const std::string MIME = mime ? mime : "";
            Log::logger->log(Log::DEBUG, "[glowbuffer] offer {:x} asks for {}", (uintptr_t)self, MIME);

            // a format glowbuffer added: the source has only `sourceMime`, so that is what is
            // accepted and read
            if (const auto KIND = listKindForMime(MIME); inject && KIND && std::ranges::find(inject->offered, MIME) != inject->offered.end()) {
                if (!self->m_accepted)
                    SOURCE->accepted(inject->sourceMime);
                self->m_recvd = true;
                if (inject->kind == SInject::IMAGE_AS_FILE)
                    pasteImage(SOURCE, inject->sourceMime, *KIND, sendFd);
                else
                    portalList(SOURCE, inject->sourceMime, *KIND, sendFd);
                return;
            }

            if (!self->m_accepted) {
                Log::logger->log(Log::WARN, "[glowbuffer] Offer was never accepted, sending accept first");
                SOURCE->accepted(MIME);
            }

            self->m_recvd = true;
            if (bridge(r, SOURCE, MIME, sendFd))
                return;
            SOURCE->send(MIME, std::move(sendFd));
        });
    }

    // Hyprland's accept handler, with the added formats translated to the one the source has:
    // a source told a target accepts a format it never offered logs a compositor bug and keeps
    // showing a no-drop cursor.
    void installAccept(CWLDataOfferResource* self, SP<CWlDataOffer>& res, SInject inject) {
        res->setAccept([self, inject](CWlDataOffer*, uint32_t, const char* mime) {
            const auto SOURCE = self->m_source.lock();
            if (!SOURCE) {
                Log::logger->log(Log::WARN, "[glowbuffer] Possible bug: Accept on an offer w/o a source");
                return;
            }
            if (self->m_dead) {
                Log::logger->log(Log::WARN, "[glowbuffer] Possible bug: Accept on an offer that's dead");
                return;
            }
            std::string m = mime ? mime : "";
            if (std::ranges::find(inject.offered, m) != inject.offered.end())
                m = inject.sourceMime;
            SOURCE->accepted(m);
            self->m_accepted = mime;
        });
    }

    void hkSendData(CWLDataOfferResource* self) {
        const auto ORIGINAL = g && g->hook ? reinterpret_cast<origSendData>(g->hook->m_original) : nullptr;
        if (!g_armed.load() || !ORIGINAL) {
            if (ORIGINAL)
                ORIGINAL(self);
            return;
        }

        const auto SOURCE = self->m_source.lock();
        auto&      res    = wlOffer(self);
        if (!SOURCE || !res || !res->resource()) {
            ORIGINAL(self);
            return;
        }

        const auto INJECT = injectionFor(SOURCE, res->client());

        if (enabled(g->negotiate))
            sendOffersAndNegotiate(self, SOURCE, res);
        else
            ORIGINAL(self);

        // after the source's own types, so a toolkit that takes the first match it knows
        // still meets them first; offers are only read until the selection event that
        // sendSelectionToDevice() sends right after this
        if (INJECT) {
            for (const auto& m : INJECT->offered)
                res->sendOffer(m.c_str());
            if (SOURCE->hasDnd())
                installAccept(self, res, *INJECT);
            Log::logger->log(Log::DEBUG, "[glowbuffer] offer {:x}: {} also offered as a file list", (uintptr_t)self, INJECT->sourceMime);
        }

        if (enabled(g->fileBridge) || INJECT)
            installReceive(self, res, INJECT);
    }
}

// ── clipboard kept after its app closes ───────────────────────────────────────────────────
namespace {
    void hkSetCurrentSelection(CSeatManager* self, SP<IDataSource> source) {
        const auto ORIGINAL = reinterpret_cast<origSetSelection>(g->selectionHook->m_original);
        // Whatever is the clipboard now stays alive until Hyprland is done with it: Hyprland
        // calls cancelled() on it through a weak pointer, and a kept clipboard lives only here.
        const SP<IDataSource> HOLD = self->m_selection.currentSelection.lock();

        if (!g_armed.load()) {
            ORIGINAL(self, source);
            return;
        }
        if (!source) {
            const auto KEPT = Persist::replacementForClear(HOLD);
            ORIGINAL(self, KEPT ? KEPT : SP<IDataSource>{});
            return;
        }
        if (source == HOLD) {
            ORIGINAL(self, source); // Hyprland warns about the duplicate and ignores it
            return;
        }
        // before Hyprland subscribes to the new source's destruction: see Persist.hpp
        const auto PREVIOUS = Persist::beforeSet(source);
        ORIGINAL(self, source);
    }
}

// ── hyprctl glowbuffer ────────────────────────────────────────────────────────────────────
namespace {
    std::string jsonEscape(const std::string& s) {
        std::string out;
        for (const char c : s) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20)
                        out += std::format("\\u{:04x}", c);
                    else
                        out += c;
            }
        }
        return out;
    }

    std::string status(eHyprCtlOutputFormat format, std::string) {
        const auto S      = Bridge::stats();
        const auto P      = Persist::stats();
        const bool HOOKED = g_armed.load() && g->hook && g->selectionHook;
        const auto ON     = [](const SP<Config::Values::CBoolValue>& v) { return enabled(v); };

        if (format == FORMAT_JSON)
            return std::format(R"({{"version":"{}","hooked":{},"negotiate_actions":{},"modifiers":{},"file_bridge":{},"paste_images":{},"persist_clipboard":{},)"
                               R"("offers_negotiated":{},"actions_sent":{},"transfers":{},"files_exported":{},"passthrough":{},"portal_lists":{},"images_pasted":{},)"
                               R"("clipboards_read":{},"clipboards_kept":{},"password_clipboards_skipped":{},"failures":{},"last_error":"{}"}})",
                               GLOWBUFFER_VERSION, HOOKED, ON(g->negotiate), ON(g->modifiers), ON(g->fileBridge), ON(g->pasteImages), ON(g->persist), g->offersNegotiated,
                               g->actionsSent, S.transfers, S.exported, S.passthrough, S.portalLists, S.images, P.read, P.restored, P.skipped, S.failures,
                               jsonEscape(S.lastError));

        return std::format("glowbuffer {}\n"
                           "hook:              {}\n"
                           "negotiate_actions: {}\n"
                           "modifiers:         {}\n"
                           "file_bridge:       {}\n"
                           "paste_images:      {}\n"
                           "persist_clipboard: {}\n"
                           "drag offers:       {} negotiated, {} action events sent\n"
                           "file lists:        {} bridged, {} files exported, {} forwarded unchanged\n"
                           "portal file lists: {}\n"
                           "images pasted:     {}\n"
                           "clipboards:        {} read, {} kept after their app closed, {} password ones left alone\n"
                           "failures:          {}\n"
                           "last error:        {}\n",
                           GLOWBUFFER_VERSION, HOOKED ? "active" : "NOT ACTIVE", ON(g->negotiate), ON(g->modifiers), ON(g->fileBridge), ON(g->pasteImages),
                           ON(g->persist), g->offersNegotiated, g->actionsSent, S.transfers, S.exported, S.passthrough, S.portalLists, S.images, P.read, P.restored,
                           P.skipped, S.failures, S.lastError.empty() ? "none" : S.lastError);
    }
}

// ── plugin entry points ───────────────────────────────────────────────────────────────────
// Do NOT change this function.
APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();
    if (HASH != CLIENT_HASH) {
        HyprlandAPI::addNotification(handle, "[glowbuffer] Failure in initialization: Version mismatch (headers ver is not equal to running hyprland ver)",
                                     CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[glowbuffer] Version mismatch");
    }

    // A reload after an unload runs in this same image (-z nodelete): reuse the state block the
    // previous load's handlers still point at instead of leaking a second one.
    if (!g)
        g = new SGlobal();
    g->handle          = handle;
    g->current.reset();
    g->announcedSource.reset();
    g->sourceAnnounced = ACTION_NONE;

    g->negotiate  = makeShared<Config::Values::CBoolValue>("plugin:glowbuffer:negotiate_actions",
                                                          "Negotiate drag-and-drop actions with the drop target instead of forcing move", true);
    g->modifiers  = makeShared<Config::Values::CBoolValue>("plugin:glowbuffer:modifiers", "Shift forces move and Control forces copy while dragging", true);
    g->fileBridge = makeShared<Config::Values::CBoolValue>("plugin:glowbuffer:file_bridge",
                                                           "Export dragged and pasted files to sandboxed apps through the document portal", true);
    g->pasteImages    = makeShared<Config::Values::CBoolValue>("plugin:glowbuffer:paste_images",
                                                            "Paste a clipboard image into a file manager as a new file", true);
    g->pasteImageApps = makeShared<Config::Values::CStringValue>("plugin:glowbuffer:paste_image_apps",
                                                                 "Window classes that get images as files (file managers that cannot paste images)",
                                                                 DEFAULT_PASTE_IMAGE_APPS);
    g->fileBridgeSources = makeShared<Config::Values::CStringValue>(
        "plugin:glowbuffer:file_bridge_sources", "Window classes whose file lists the file bridge shares with sandboxed apps", DEFAULT_FILE_BRIDGE_SOURCES);
    g->persist = makeShared<Config::Values::CBoolValue>("plugin:glowbuffer:persist_clipboard", "Keep the clipboard after the app that copied closes", true);
    for (const SP<Config::Values::IValue>& v : {SP<Config::Values::IValue>(g->negotiate), SP<Config::Values::IValue>(g->modifiers),
                                                SP<Config::Values::IValue>(g->fileBridge), SP<Config::Values::IValue>(g->pasteImages),
                                                SP<Config::Values::IValue>(g->pasteImageApps), SP<Config::Values::IValue>(g->persist),
                                                SP<Config::Values::IValue>(g->fileBridgeSources)}) {
        if (!HyprlandAPI::addConfigValueV2(handle, v))
            Log::logger->log(Log::ERR, "[glowbuffer] could not register config value {}", v->name());
    }

    for (const auto& FN : HyprlandAPI::findFunctionsByName(handle, "sendData")) {
        if (FN.demangled.find("CWLDataOfferResource::sendData") == std::string::npos)
            continue;
        g->hook = HyprlandAPI::createFunctionHook(handle, FN.address, reinterpret_cast<const void*>(&hkSendData));
        break;
    }
    if (!g->hook || !g->hook->hook()) {
        g->hook = nullptr;
        HyprlandAPI::addNotification(handle, "[glowbuffer] could not hook CWLDataOfferResource::sendData; drag and drop is unchanged", CHyprColor{1.0, 0.2, 0.2, 1.0},
                                     8000);
        Log::logger->log(Log::ERR, "[glowbuffer] could not hook CWLDataOfferResource::sendData");
    }

    for (const auto& FN : HyprlandAPI::findFunctionsByName(handle, "setCurrentSelection")) {
        if (FN.demangled.find("CSeatManager::setCurrentSelection(") == std::string::npos)
            continue;
        g->selectionHook = HyprlandAPI::createFunctionHook(handle, FN.address, reinterpret_cast<const void*>(&hkSetCurrentSelection));
        break;
    }
    if (!g->selectionHook || !g->selectionHook->hook()) {
        g->selectionHook = nullptr;
        Log::logger->log(Log::ERR, "[glowbuffer] could not hook CSeatManager::setCurrentSelection; the clipboard will not outlive its app");
    }

    g->keyListener    = Event::bus()->m_events.input.keyboard.key.listen([](IKeyboard::SKeyEvent e, Event::SCallbackInfo&) { onKey(e); });
    g->buttonListener = Event::bus()->m_events.input.mouse.button.listen([](IPointer::SButtonEvent e, Event::SCallbackInfo&) {
        if (e.state == WL_POINTER_BUTTON_STATE_RELEASED)
            onDragEnd();
    });
    g->touchListener = Event::bus()->m_events.input.touch.up.listen([](ITouch::SUpEvent, Event::SCallbackInfo&) { onDragEnd(); });
    g->tipListener   = Event::bus()->m_events.input.tablet.tip.listen([](CTablet::STipEvent e, Event::SCallbackInfo&) {
        if (!e.in)
            onDragEnd();
    });

    g->ctl = HyprlandAPI::registerHyprCtlCommand(handle, SHyprCtlCommand{.name = "glowbuffer", .exact = true, .fn = status});

    Bridge::start();
    Persist::start([] { return enabled(g->persist); });
    g_armed.store(true);
    HyprlandAPI::reloadConfig(); // last, so the values above populate

    return {"glowbuffer", "Drag and drop and paste that work, sandboxed apps included", "Vergil", GLOWBUFFER_VERSION};
}

APICALL EXPORT void PLUGIN_EXIT() {
    // First: every handler installed on a live offer checks this and falls back to plain
    // forwarding, so nothing below can be re-entered half torn down.
    g_armed.store(false);
    if (!g)
        return;

    if (g->hook) {
        HyprlandAPI::removeFunctionHook(g->handle, g->hook);
        g->hook = nullptr;
    }
    if (g->selectionHook) {
        HyprlandAPI::removeFunctionHook(g->handle, g->selectionHook);
        g->selectionHook = nullptr;
    }
    g->keyListener.reset();
    g->buttonListener.reset();
    g->touchListener.reset();
    g->tipListener.reset();
    if (g->ctl) {
        HyprlandAPI::unregisterHyprCtlCommand(g->handle, g->ctl);
        g->ctl.reset();
    }
    g->current.reset();
    Persist::stop();

    Bridge::shutdown(std::chrono::milliseconds(3000));
}
