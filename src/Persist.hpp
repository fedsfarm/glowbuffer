#pragma once

// Keeping the clipboard after the app that copied closes. ClipboardPlan.hpp says what is
// read and why; this is the compositor side.
//
// Hyprland's SeatManager clears the clipboard from the source's destroy signal, with the same
// setCurrentSelection(nullptr) an app's deliberate "clear clipboard" makes. main.cpp hooks
// CSeatManager::setCurrentSelection and tells the two apart by order: for a new clipboard it
// calls beforeSet() BEFORE SeatManager subscribes to that source's destruction, so glowbuffer's
// destroy listener runs first and marks the source dying. A clear that arrives while the
// current source is dying is the app going away: replacementForClear() hands back the kept
// copy. A clear while it is alive is deliberate and stays a clear.

#include <hyprland/src/protocols/types/DataDevice.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <utility>
#include <vector>

namespace Glowbuffer::Persist {

    struct SCache {
        std::mutex lock;
        bool       done = false;
        // served format -> bytes; several served formats may share one read
        std::vector<std::pair<std::string, std::shared_ptr<const std::string>>> data;
    };

    // The clipboard glowbuffer puts back. It answers from memory, off the compositor thread.
    class CKeptSource : public IDataSource {
      public:
        explicit CKeptSource(std::vector<std::pair<std::string, std::shared_ptr<const std::string>>> data);

        virtual std::vector<std::string> mimes();
        virtual void                     send(const std::string& mime, Hyprutils::OS::CFileDescriptor fd);
        virtual void                     accepted(const std::string& mime);
        virtual void                     cancelled();
        virtual void                     error(uint32_t code, const std::string& msg);

      private:
        std::vector<std::pair<std::string, std::shared_ptr<const std::string>>> m_data;
    };

    void start(bool (*enabled)());
    void stop();

    // A new clipboard is about to be set (from the hook, before Hyprland's own code runs).
    // Starts reading it in advance when it is worth keeping. The returned holder owns the
    // previous kept clipboard: keep it alive until Hyprland has finished replacing it.
    [[nodiscard]] SP<IDataSource> beforeSet(const SP<IDataSource>& next);

    // The clipboard is about to be cleared. When that is its app going away and a copy was
    // read in time, returns the kept copy to set instead; otherwise nullptr (a real clear).
    SP<IDataSource> replacementForClear(const SP<IDataSource>& current);

    struct SStats {
        uint64_t read     = 0; // clipboards read in advance
        uint64_t restored = 0; // clipboards put back after their app closed
        uint64_t skipped  = 0; // password-manager clipboards left alone
    };
    SStats stats();

} // namespace Glowbuffer::Persist
