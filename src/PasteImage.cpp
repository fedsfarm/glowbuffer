#include "PasteImage.hpp"

#include <algorithm>
#include <cctype>

namespace Glowbuffer {

    static std::string lower(std::string_view s) {
        std::string out{s};
        std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
    }

    std::optional<std::string> pickImageMime(const std::vector<std::string>& mimes) {
        std::optional<std::string> first, png;
        for (const auto& m : mimes) {
            if (listKindForMime(m))
                return std::nullopt;
            const auto L = lower(m);
            if (L == "image/png") {
                if (!png)
                    png = m;
            } else if (!first && L.starts_with("image/"))
                first = m;
        }
        return png ? png : first;
    }

    std::string imageExtension(std::string_view mime) {
        auto L = lower(mime);
        if (const auto SEMI = L.find(';'); SEMI != std::string::npos)
            L.resize(SEMI);
        if (L == "image/jpeg" || L == "image/jpg" || L == "image/pjpeg")
            return "jpg";
        if (L == "image/svg+xml")
            return "svg";
        if (L == "image/x-icon" || L == "image/vnd.microsoft.icon")
            return "ico";
        if (L == "image/x-bmp" || L == "image/x-ms-bmp")
            return "bmp";

        std::string sub = L.starts_with("image/") ? L.substr(6) : std::string{};
        if (sub.starts_with("x-"))
            sub.erase(0, 2);
        std::erase_if(sub, [](unsigned char c) { return !std::isalnum(c); });
        return sub.empty() ? "img" : sub;
    }

    bool classInList(std::string_view windowClass, std::string_view list) {
        if (windowClass.empty())
            return false;
        const auto WANT = lower(windowClass);
        size_t     pos  = 0;
        while (pos < list.size()) {
            const auto END = list.find_first_of(" ,\t", pos);
            const auto TOK = list.substr(pos, END == std::string_view::npos ? std::string_view::npos : END - pos);
            if (!TOK.empty() && lower(TOK) == WANT)
                return true;
            if (END == std::string_view::npos)
                break;
            pos = END + 1;
        }
        return false;
    }

    std::string pastedImageName(std::time_t when, std::string_view extension) {
        std::tm tm{};
        localtime_r(&when, &tm);
        char stamp[32];
        std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H-%M-%S", &tm);
        return std::string{"Pasted image "} + stamp + "." + std::string{extension};
    }

    std::string fileListFor(EListKind kind, std::string_view path) {
        return fileList(kind, {std::string{path}});
    }

} // namespace Glowbuffer
