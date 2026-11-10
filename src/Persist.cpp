#include "Persist.hpp"
#include "Access.hpp"
#include "Bridge.hpp"
#include "ClipboardPlan.hpp"
#include "Sandbox.hpp"

#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/debug/log/Logger.hpp>

#include <wayland-server-core.h>

#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

using Hyprutils::OS::CFileDescriptor;

namespace Glowbuffer::Persist {

    // ── the kept clipboard ──────────────────────────────────────────────────────────────────
    CKeptSource::CKeptSource(std::vector<std::pair<std::string, std::shared_ptr<const std::string>>> data) : m_data(std::move(data)) {
        markUsed();
    }

    std::vector<std::string> CKeptSource::mimes() {
        std::vector<std::string> out;
        for (const auto& [mime, _] : m_data) {
            if (std::ranges::find(out, mime) == out.end())
                out.push_back(mime);
        }
        return out;
    }

    void CKeptSource::send(const std::string& mime, CFileDescriptor fd) {
        const auto IT = std::ranges::find_if(m_data, [&](const auto& d) { return d.first == mime; });
        if (IT == m_data.end())
            return; // fd closes: an empty read, like any source asked for a type it lacks
        // An image can be megabytes; a target that reads slowly must not stall the compositor.
        Bridge::writeLater(fd.take(), IT->second);
    }

    void CKeptSource::accepted(const std::string&) {}
    void CKeptSource::cancelled() {}
    void CKeptSource::error(uint32_t, const std::string&) {}

    // ── tracking ────────────────────────────────────────────────────────────────────────────
    namespace {
        struct SState {
            bool (*enabled)() = nullptr;
            CHyprSignalListener destroyListener;

            WP<IDataSource>         tracked;
            IDataSource*            dying = nullptr; // set by our destroy listener, never dereferenced
            std::shared_ptr<SCache> cache;

            // SeatManager holds the selection by weak pointer only: the kept source lives here
            // until something else is copied or the clipboard is cleared.
            SP<CKeptSource> kept;

            SStats stats; // compositor thread only
        };

        // Never freed: the image stays mapped (-z nodelete) and listeners may outlive a reload.
        SState* s = nullptr;

        bool on() {
            return s && s->enabled && s->enabled();
        }

        void untrack() {
            s->destroyListener.reset();
            s->tracked.reset();
            s->dying = nullptr;
            s->cache.reset();
        }

        void track(const SP<IDataSource>& source) {
            // Only an app's own wl_data_source. Clipboard managers (data-control) keep their
            // clipboards themselves, and Xwayland clears through XFixes, not a destroy signal.
            wl_client* const CLIENT = sourceClient(source.get());
            if (!CLIENT)
                return;

            const auto MIMES = source->mimes();
            const auto PLAN  = planPersist(MIMES);
            if (PLAN.empty()) {
                if (std::ranges::any_of(MIMES, [](const std::string& m) { return m == PASSWORD_HINT; }))
                    ++s->stats.skipped;
                return;
            }

            std::vector<Bridge::SRead> reads;
            for (const auto& r : PLAN) {
                int p[2];
                if (pipe2(p, O_CLOEXEC) != 0)
                    continue;
                source->send(r.mime, CFileDescriptor{p[1]});
                reads.push_back({.mime = r.mime, .fd = p[0], .maxBytes = r.maxBytes});
            }
            if (reads.empty())
                return;

            auto cache = std::make_shared<SCache>();
            // On the worker thread: fill the cache, nothing else.
            Bridge::readMany(std::move(reads), [cache, PLAN](std::vector<std::pair<std::string, std::string>> results) {
                std::scoped_lock l(cache->lock);
                for (auto& [mime, bytes] : results) {
                    // Zeroed when the last holder lets go: whatever the clipboard held (text a
                    // user copied from a terminal, say) does not linger in freed compositor
                    // memory. Best effort: earlier buffers of the read are not covered.
                    const std::shared_ptr<const std::string> SHARED(new std::string(std::move(bytes)), [](const std::string* s) {
                        explicit_bzero(const_cast<char*>(s->data()), s->size());
                        delete s;
                    });
                    for (const auto& r : PLAN) {
                        if (r.mime != mime)
                            continue;
                        for (const auto& served : r.served)
                            cache->data.emplace_back(served, SHARED);
                    }
                }
                cache->done = true;
            });

            s->tracked = source;
            s->cache   = cache;
            // Subscribed before SeatManager subscribes (beforeSet runs ahead of Hyprland's code),
            // so this runs first when the source dies, ahead of SeatManager's clear.
            s->destroyListener = source->m_events.destroy.listen([raw = source.get()] { s->dying = raw; });
            ++s->stats.read;
        }
    }

    SP<IDataSource> beforeSet(const SP<IDataSource>& next) {
        if (!s)
            return nullptr;
        SP<IDataSource> previous = s->kept;
        s->kept.reset();
        untrack();
        if (next && on())
            track(next);
        return previous;
    }

    SP<IDataSource> replacementForClear(const SP<IDataSource>& current) {
        if (!s)
            return nullptr;
        const bool APP_CLOSED = current && current.get() == s->dying && current == s->tracked.lock();
        const auto CACHE      = s->cache;
        untrack();
        s->kept.reset();

        if (!APP_CLOSED || !CACHE || !on())
            return nullptr; // a deliberate clear, or nothing kept: clear it

        std::vector<std::pair<std::string, std::shared_ptr<const std::string>>> data;
        {
            std::scoped_lock l(CACHE->lock);
            if (!CACHE->done)
                return nullptr; // the app closed before it even answered
            data = CACHE->data;
        }
        if (data.empty())
            return nullptr;

        s->kept = makeShared<CKeptSource>(std::move(data));
        ++s->stats.restored;
        Log::logger->log(Log::DEBUG, "glowbuffer", "clipboard kept after its app closed ({} formats)", s->kept->mimes().size());
        return s->kept;
    }

    void start(bool (*enabled)()) {
        if (!s)
            s = new SState();
        s->enabled = enabled;
    }

    void stop() {
        if (!s)
            return;
        untrack();
        // Unloading stops the workers that answer pastes from the kept bytes, and the hook that
        // would release them on the next copy: a kept clipboard would then paste empty and
        // stay in memory for good. Clear it instead (the hook is already gone, so this is
        // Hyprland's own clear), holding it alive across the call.
        if (s->kept) {
            const SP<IDataSource> HOLD = s->kept;
            if (g_pSeatManager->m_selection.currentSelection.lock() == HOLD)
                g_pSeatManager->setCurrentSelection(nullptr);
            s->kept.reset();
        }
    }

    SStats stats() {
        return s ? s->stats : SStats{};
    }

} // namespace Glowbuffer::Persist
