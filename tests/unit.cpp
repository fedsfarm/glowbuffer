// Pure-logic tests: action negotiation, file list parsing and rewriting, .flatpak-info parsing.
// No compositor, no wayland. tests/run.sh builds and runs this with a plain g++.

#include "Actions.hpp"
#include "ClipboardPlan.hpp"
#include "PasteImage.hpp"
#include "Sandbox.hpp"
#include "UriList.hpp"

#include <cstdio>
#include <map>
#include <unistd.h>
#include <string>

using namespace Glowbuffer;

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(cond)                                                                                                                                                      \
    do {                                                                                                                                                                 \
        ++g_checks;                                                                                                                                                      \
        if (!(cond)) {                                                                                                                                                   \
            ++g_failures;                                                                                                                                                \
            std::fprintf(stderr, "%s:%d: FAILED: %s\n", __FILE__, __LINE__, #cond);                                                                                      \
        }                                                                                                                                                                \
    } while (0)

#define CHECK_EQ(a, b)                                                                                                                                                   \
    do {                                                                                                                                                                 \
        ++g_checks;                                                                                                                                                      \
        const auto A_ = (a);                                                                                                                                             \
        const auto B_ = (b);                                                                                                                                             \
        if (!(A_ == B_)) {                                                                                                                                               \
            ++g_failures;                                                                                                                                                \
            std::fprintf(stderr, "%s:%d: FAILED: %s == %s\n", __FILE__, __LINE__, #a, #b);                                                                              \
        }                                                                                                                                                                \
    } while (0)

static void testActions() {
    // Thunar's drag: copy|move|ask. The bug report case: a GTK4 target that takes copy only.
    // Hyprland 0.56 forces MOVE here; the protocol answer is COPY.
    CHECK_EQ(chooseAction({.sourceActions = ACTION_ALL, .targetActions = ACTION_COPY, .targetPreferred = ACTION_COPY}), ACTION_COPY);

    // nothing announced before the target speaks
    CHECK_EQ(chooseAction({.sourceActions = ACTION_ALL}), ACTION_NONE);

    // the target's preference wins when the source allows it
    CHECK_EQ(chooseAction({.sourceActions = ACTION_ALL, .targetActions = ACTION_COPY | ACTION_MOVE, .targetPreferred = ACTION_MOVE}), ACTION_MOVE);
    CHECK_EQ(chooseAction({.sourceActions = ACTION_ALL, .targetActions = ACTION_ALL, .targetPreferred = ACTION_ASK}), ACTION_ASK);

    // a preference the source does not allow falls back to bit order
    CHECK_EQ(chooseAction({.sourceActions = ACTION_COPY, .targetActions = ACTION_COPY | ACTION_MOVE, .targetPreferred = ACTION_MOVE}), ACTION_COPY);

    // no preference: bit order, copy first
    CHECK_EQ(chooseAction({.sourceActions = ACTION_ALL, .targetActions = ACTION_MOVE | ACTION_ASK}), ACTION_MOVE);

    // nothing in common: refused
    CHECK_EQ(chooseAction({.sourceActions = ACTION_MOVE, .targetActions = ACTION_COPY, .targetPreferred = ACTION_COPY}), ACTION_NONE);

    // a source that never called set_actions behaves as copy-only (wlroots)
    CHECK_EQ(chooseAction({.sourceActions = 0, .targetActions = ACTION_COPY | ACTION_MOVE, .targetPreferred = ACTION_MOVE}), ACTION_COPY);

    // an offer older than v3 cannot negotiate: copy, whatever it "sent"
    CHECK_EQ(chooseAction({.sourceActions = ACTION_ALL, .targetActions = 0, .targetVersion = 2}), ACTION_COPY);
    CHECK_EQ(chooseAction({.sourceActions = ACTION_MOVE, .targetVersion = 2}), ACTION_NONE);

    // modifiers: Shift forces move, Control forces copy, when both sides allow it
    CHECK_EQ(compositorAction(MOD_SHIFT), ACTION_MOVE);
    CHECK_EQ(compositorAction(MOD_CTRL), ACTION_COPY);
    CHECK_EQ(compositorAction(MOD_SHIFT | MOD_CTRL), ACTION_NONE);
    CHECK_EQ(compositorAction(0), ACTION_NONE);
    CHECK_EQ(chooseAction({.sourceActions = ACTION_ALL, .targetActions = ACTION_ALL, .targetPreferred = ACTION_COPY, .forced = ACTION_MOVE}), ACTION_MOVE);
    CHECK_EQ(chooseAction({.sourceActions = ACTION_ALL, .targetActions = ACTION_COPY, .targetPreferred = ACTION_COPY, .forced = ACTION_MOVE}), ACTION_COPY);
    CHECK_EQ(chooseAction({.sourceActions = ACTION_COPY, .targetActions = ACTION_ALL, .targetPreferred = ACTION_MOVE, .forced = ACTION_COPY}), ACTION_COPY);

    // a malformed multi-bit preference never yields a multi-bit action
    CHECK_EQ(chooseAction({.sourceActions = ACTION_ALL, .targetActions = ACTION_ALL, .targetPreferred = ACTION_MOVE | ACTION_ASK}), ACTION_MOVE);
    // bits outside the protocol are ignored
    CHECK_EQ(chooseAction({.sourceActions = 0x80 | ACTION_COPY, .targetActions = 0x80 | ACTION_COPY, .targetPreferred = 0x80}), ACTION_COPY);
}

static void testMimes() {
    CHECK(listKindForMime("text/uri-list") == EListKind::URI_LIST);
    CHECK(listKindForMime("TEXT/URI-LIST") == EListKind::URI_LIST);
    CHECK(listKindForMime("text/uri-list;charset=utf-8") == EListKind::URI_LIST);
    CHECK(listKindForMime("application/x-kde4-urilist") == EListKind::URI_LIST);
    CHECK(listKindForMime("x-special/gnome-copied-files") == EListKind::GNOME_COPIED_FILES);
    CHECK(!listKindForMime("text/plain"));
    CHECK(!listKindForMime("application/vnd.portal.filetransfer"));
    CHECK(!listKindForMime(""));
}

static void testUris() {
    CHECK(fileUriToPath("file:///home/u/a%20b.txt") == std::optional<std::string>{"/home/u/a b.txt"});
    CHECK(fileUriToPath("file://localhost/tmp/x") == std::optional<std::string>{"/tmp/x"});
    CHECK(fileUriToPath("FILE:///tmp/x") == std::optional<std::string>{"/tmp/x"});
    CHECK(fileUriToPath("file:/tmp/x") == std::optional<std::string>{"/tmp/x"});
    CHECK(fileUriToPath("file:///tmp/%C3%A9t%C3%A9") == std::optional<std::string>{"/tmp/\xC3\xA9t\xC3\xA9"});
    CHECK(fileUriToPath("file:///a/%2523") == std::optional<std::string>{"/a/%23"});
    CHECK(!fileUriToPath("file://remote/tmp/x"));
    CHECK(!fileUriToPath("https://example.com/a"));
    CHECK(!fileUriToPath("file://"));
    CHECK(!fileUriToPath("file:relative"));
    CHECK(!fileUriToPath("file:///a%2"));
    CHECK(!fileUriToPath("file:///a%zz"));
    CHECK(!fileUriToPath("file:///a%00b"));
    CHECK(!fileUriToPath(std::string_view{"file:///etc/passwd\0/home/u/photo.jpg", 36}));
    CHECK(!fileUriToPath("file:///a#frag"));
    CHECK(!fileUriToPath("file:///a?q"));
    CHECK(!fileUriToPath(""));

    CHECK_EQ(pathToFileUri("/home/u/a b.txt"), std::string{"file:///home/u/a%20b.txt"});
    CHECK_EQ(pathToFileUri("/a#b?c%d"), std::string{"file:///a%23b%3Fc%25d"});
    CHECK_EQ(pathToFileUri("/\xC3\xA9"), std::string{"file:///%C3%A9"});
    CHECK_EQ(pathToFileUri("/keep-._~!$&'()*+,:=@/x"), std::string{"file:///keep-._~!$&'()*+,:=@/x"});
    CHECK_EQ(pathToFileUri("/semi;colon[1]"), std::string{"file:///semi%3Bcolon%5B1%5D"});

    // round trip over every byte a filename can hold
    std::string all = "/";
    for (int c = 1; c < 256; ++c)
        if (c != '/')
            all += static_cast<char>(c);
    CHECK(fileUriToPath(pathToFileUri(all)) == std::optional<std::string>{all});
}

static std::optional<std::string> mapFrom(const std::map<std::string, std::string>& m, const std::string& p) {
    const auto IT = m.find(p);
    return IT == m.end() ? std::nullopt : std::optional<std::string>{IT->second};
}

static void testLists() {
    // RFC 2483: CRLF, comments allowed. Links and comments must come out byte for byte.
    const std::string IN = "# dragged\r\nfile:///home/u/a%20b.txt\r\nhttps://example.com/x\r\nfile:///home/u/c\r\n";
    const auto        L  = parseList(IN, EListKind::URI_LIST);
    CHECK_EQ(L.size(), size_t{4});
    CHECK(!L[0].path);
    CHECK(L[1].path == std::optional<std::string>{"/home/u/a b.txt"});
    CHECK(!L[2].path);
    CHECK_EQ(listedPaths(L).size(), size_t{2});

    const std::map<std::string, std::string> M = {{"/home/u/a b.txt", "/run/user/1000/doc/ab12/a b.txt"}};
    const auto OUT = serializeList(L, [&](const std::string& p) { return mapFrom(M, p); });
    CHECK_EQ(OUT, std::string{"# dragged\r\nfile:///run/user/1000/doc/ab12/a%20b.txt\r\nhttps://example.com/x\r\nfile:///home/u/c\r\n"});

    // nothing mapped: identical bytes
    CHECK_EQ(serializeList(L, [](const std::string&) { return std::optional<std::string>{}; }), IN);

    // LF endings, no terminator on the last line (Thunar, Nautilus)
    const std::string LF = "file:///a\nfile:///b";
    const auto        L2 = parseList(LF, EListKind::URI_LIST);
    CHECK_EQ(L2.size(), size_t{2});
    CHECK_EQ(L2[1].eol, std::string{""});
    CHECK_EQ(serializeList(L2, [](const std::string& p) { return std::optional<std::string>{"/doc" + p}; }), std::string{"file:///doc/a\nfile:///doc/b"});

    // gnome-copied-files: the action line is not a URI; '#' is not a comment there
    const auto G = parseList("cut\nfile:///x\n", EListKind::GNOME_COPIED_FILES);
    CHECK_EQ(G.size(), size_t{2});
    CHECK(!G[0].path);
    CHECK(G[1].path == std::optional<std::string>{"/x"});
    CHECK_EQ(serializeList(G, [](const std::string&) { return std::optional<std::string>{"/d/x"}; }), std::string{"cut\nfile:///d/x\n"});

    // a file named "copy" in a uri-list is still just a line, never a header
    CHECK(!parseList("copy\n", EListKind::URI_LIST)[0].path);

    // duplicates export once
    CHECK_EQ(listedPaths(parseList("file:///a\nfile:///a\n", EListKind::URI_LIST)).size(), size_t{1});

    CHECK(parseList("", EListKind::URI_LIST).empty());
    CHECK_EQ(parseList("\n", EListKind::URI_LIST).size(), size_t{1});
}

static void testFlatpakInfo() {
    const std::string INFO = "[Application]\nname=org.telegram.desktop\nruntime=runtime/org.gnome.Platform/x86_64/51\n\n"
                             "[Instance]\ninstance-id=123\napp-path=/var/lib/flatpak/app/org.telegram.desktop\n";
    CHECK(parseFlatpakAppId(INFO) == std::optional<std::string>{"org.telegram.desktop"});
    CHECK(parseFlatpakAppId("[Instance]\nname=wrong\n[Application]\n name = org.x.Y \r\n") == std::optional<std::string>{"org.x.Y"});
    CHECK(!parseFlatpakAppId("[Instance]\nname=org.x.Y\n"));
    CHECK(!parseFlatpakAppId("[Application]\nname=\n"));
    CHECK(!parseFlatpakAppId(""));
    // a host process: no keyfile under /proc/self/root
    CHECK(!isFlatpakPid(0));
    CHECK(!isFlatpakPid(-1));
    // positive host evidence: this test shares the root it checks against
    CHECK(isHostPid(getpid()));
    CHECK(!isHostPid(0));
    CHECK(!isHostPid(-1));
    CHECK(!isHostPid(999999999));
}

static void testPasteImage() {
    using V = std::vector<std::string>;
    // a screenshot: image only -> save the png
    CHECK(pickImageMime(V{"image/png"}) == std::optional<std::string>{"image/png"});
    // a browser's "Copy image": html first, png preferred over the rest
    CHECK(pickImageMime(V{"text/html", "image/webp", "image/png"}) == std::optional<std::string>{"image/png"});
    CHECK(pickImageMime(V{"text/html", "image/jpeg"}) == std::optional<std::string>{"image/jpeg"});
    // spelled as offered: a source answers only the exact strings it offered
    CHECK(pickImageMime(V{"Image/PNG"}) == std::optional<std::string>{"Image/PNG"});
    // a file list is already pasteable: leave it alone
    CHECK(!pickImageMime(V{"image/png", "text/uri-list"}));
    CHECK(!pickImageMime(V{"x-special/gnome-copied-files", "image/png"}));
    // text only: nothing to save
    CHECK(!pickImageMime(V{"text/plain", "UTF8_STRING"}));
    CHECK(!pickImageMime(V{}));

    CHECK_EQ(imageExtension("image/png"), std::string{"png"});
    CHECK_EQ(imageExtension("image/jpeg"), std::string{"jpg"});
    CHECK_EQ(imageExtension("image/svg+xml"), std::string{"svg"});
    CHECK_EQ(imageExtension("image/x-icon"), std::string{"ico"});
    CHECK_EQ(imageExtension("image/x-portable-pixmap"), std::string{"portablepixmap"});
    CHECK_EQ(imageExtension("IMAGE/WEBP;q=1"), std::string{"webp"});
    CHECK_EQ(imageExtension("image/"), std::string{"img"});
    CHECK_EQ(imageExtension("image/../../x"), std::string{"x"}); // never a path

    CHECK(classInList("thunar", DEFAULT_PASTE_IMAGE_APPS));
    CHECK(classInList("Thunar", "thunar nemo"));
    CHECK(classInList("pcmanfm-qt", "thunar,pcmanfm-qt"));
    CHECK(!classInList("thunar-settings", "thunar"));
    CHECK(!classInList("org.telegram.desktop", DEFAULT_PASTE_IMAGE_APPS));
    CHECK(!classInList("", DEFAULT_PASTE_IMAGE_APPS));
    CHECK(!classInList("thunar", ""));

    const auto NAME = pastedImageName(0, "png");
    CHECK(NAME.starts_with("Pasted image 19"));
    CHECK(NAME.ends_with(".png"));
    CHECK(NAME.find(':') == std::string::npos);
    CHECK(NAME.find('/') == std::string::npos);

    CHECK_EQ(fileListFor(EListKind::GNOME_COPIED_FILES, "/r/Pasted image.png"), std::string{"copy\nfile:///r/Pasted%20image.png"});
    CHECK_EQ(fileListFor(EListKind::URI_LIST, "/r/a.png"), std::string{"file:///r/a.png\r\n"});
    CHECK_EQ(fileList(EListKind::GNOME_COPIED_FILES, {"/a", "/b c"}), std::string{"copy\nfile:///a\nfile:///b%20c"});
    CHECK_EQ(fileList(EListKind::URI_LIST, {"/a", "/b"}), std::string{"file:///a\r\nfile:///b\r\n"});
    CHECK_EQ(fileList(EListKind::GNOME_COPIED_FILES, {}), std::string{"copy"});
}

static void testClipboardPlan() {
    using V = std::vector<std::string>;
    // eog "Copy": an image and its name as text
    auto p = planPersist(V{"image/png", "image/jpeg", "text/plain;charset=utf-8", "UTF8_STRING", "STRING"});
    CHECK_EQ(p.size(), size_t{2});
    CHECK_EQ(p[0].mime, std::string{"text/plain;charset=utf-8"});
    CHECK_EQ(p[0].served.size(), size_t{3}); // one read answers all three text names
    CHECK_EQ(p[1].mime, std::string{"image/png"});
    CHECK(p[1].maxBytes > p[0].maxBytes);

    // a password manager's clipboard is never read, whatever else it offers
    CHECK(planPersist(V{"text/plain;charset=utf-8", "x-kde-passwordManagerHint"}).empty());
    CHECK(planPersist(V{"X-KDE-PASSWORDMANAGERHINT", "image/png"}).empty());

    // copied files: both list formats kept, text too
    p = planPersist(V{"x-special/gnome-copied-files", "text/uri-list", "UTF8_STRING"});
    CHECK_EQ(p.size(), size_t{3});

    // a browser copy: html kept, chromium internals not
    p = planPersist(V{"text/html", "chromium/x-web-custom-data", "text/plain;charset=utf-8"});
    CHECK_EQ(p.size(), size_t{2});
    for (const auto& r : p)
        CHECK(r.mime != "chromium/x-web-custom-data");

    // nothing worth keeping
    CHECK(planPersist(V{"application/x-qt-image"}).empty());
    CHECK(planPersist(V{}).empty());

    // Telegram's media drag: a portal key and no list
    CHECK(offersOnlyPortalFiles(V{"application/x-td-forward", "application/vnd.portal.filetransfer"}));
    CHECK(offersOnlyPortalFiles(V{"application/vnd.portal.filetransfer", "image/png"}));
    // Nautilus: key and list, nothing to add
    CHECK(!offersOnlyPortalFiles(V{"application/vnd.portal.filetransfer", "text/uri-list"}));
    CHECK(!offersOnlyPortalFiles(V{"text/uri-list"}));
    CHECK(!offersOnlyPortalFiles(V{}));

    // the file bridge trusts file and archive managers, never a browser
    CHECK(classInList("thunar", DEFAULT_FILE_BRIDGE_SOURCES));
    CHECK(classInList("org.gnome.Nautilus", DEFAULT_FILE_BRIDGE_SOURCES));
    CHECK(classInList("file-roller", DEFAULT_FILE_BRIDGE_SOURCES));
    for (const char* browser : {"brave-origin", "firefox", "librewolf", "chromium", "org.mozilla.firefox", "com.vscodium.codium"})
        CHECK(!classInList(browser, DEFAULT_FILE_BRIDGE_SOURCES));
}

int main() {
    testActions();
    testMimes();
    testUris();
    testLists();
    testFlatpakInfo();
    testPasteImage();
    testClipboardPlan();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
