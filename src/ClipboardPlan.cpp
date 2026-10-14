#include "ClipboardPlan.hpp"
#include "PasteImage.hpp"
#include "UriList.hpp"

#include <algorithm>
#include <cctype>

namespace Glowbuffer {

    namespace {
        // Held in compositor memory for as long as the clipboard is kept. Text past a few MiB is
        // rare; a large photo copied as PNG can approach the image cap.
        constexpr size_t MAX_TEXT  = 4 * 1024 * 1024;
        constexpr size_t MAX_IMAGE = 64 * 1024 * 1024;

        // Plain text under its many names, best first. One read answers all of them.
        constexpr std::string_view TEXT_MIMES[] = {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "TEXT", "STRING"};

        bool offered(const std::vector<std::string>& mimes, std::string_view m) {
            return std::ranges::any_of(mimes, [m](const std::string& x) { return iequals(x, m); });
        }
    }

    std::vector<SPersistRead> planPersist(const std::vector<std::string>& mimes) {
        std::vector<SPersistRead> plan;
        if (offered(mimes, PASSWORD_HINT))
            return plan;

        SPersistRead text;
        for (const auto T : TEXT_MIMES) {
            for (const auto& m : mimes) {
                if (!iequals(m, T))
                    continue;
                if (text.mime.empty())
                    text.mime = m;
                text.served.push_back(m);
            }
        }
        if (!text.mime.empty()) {
            text.maxBytes = MAX_TEXT;
            plan.push_back(std::move(text));
        }

        for (const auto& m : mimes) {
            if (iequals(m, "text/html") || listKindForMime(m))
                plan.push_back({.mime = m, .served = {m}, .maxBytes = MAX_TEXT});
        }

        // one image is enough to paste; png when there is one (pickImageMime refuses when a
        // file list is offered, which does not matter here: keep the image too)
        std::vector<std::string> imagesOnly;
        std::ranges::copy_if(mimes, std::back_inserter(imagesOnly), [](const std::string& m) { return !listKindForMime(m); });
        if (const auto IMAGE = pickImageMime(imagesOnly); IMAGE)
            plan.push_back({.mime = *IMAGE, .served = {*IMAGE}, .maxBytes = MAX_IMAGE});

        return plan;
    }

    bool offersOnlyPortalFiles(const std::vector<std::string>& mimes) {
        if (!offered(mimes, PORTAL_FILETRANSFER))
            return false;
        return std::ranges::none_of(mimes, [](const std::string& m) { return listKindForMime(m).has_value(); });
    }

} // namespace Glowbuffer
