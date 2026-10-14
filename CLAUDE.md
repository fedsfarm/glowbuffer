# CLAUDE.md

glowbuffer is a Hyprland 0.56 plugin: drag-and-drop action negotiation (Hyprland forces
MOVE and ignores `wl_data_offer.set_actions`) plus a file bridge that exports dragged and
pasted files to Flatpak apps through the document portal. README.md is brief on purpose
(the fedsfarm plugin style, like gloview); the details live here.

## The file bridge, in rules

A list (`text/uri-list`, `application/x-kde4-urilist`, `x-special/gnome-copied-files`)
is rewritten only when all of these hold, else forwarded byte for byte:

- the reader is a Flatpak (`/proc/<pid>/root/.flatpak-info`, app id read on a worker, and
  the grant made only if the reader's pidfd from `SO_PEERPIDFD` still names a live process);
- the source is a host app PROVEN to be one (`isHostPid`: its root is the compositor's), not
  the reader's own connection, and a file or archive manager by window class
  (`file_bridge_sources`). Never a sandboxed source: its paths opened from here through
  `/proc/<pid>/root` let its `..` and absolute symlinks reach any host file (that was a
  sandbox escape). Never a browser: web content can put any `file://` in a drag. Never a
  kept clipboard, Xwayland or data-control source: no live process to check;
- the file is one the app cannot already read (`as-needed-by-app`).

Grants are `read` only, for the portal's lifetime.

## Drag action after the drop

A pointer button release (also touch up, tablet tip up) ends the drag for glowbuffer too:
the offer is marked dropped, modifiers stop renegotiating, and the target's `set_actions`
is honoured only to resolve `ask`. Without that, Shift pressed while the target reads a
dropped list turned a copy into a move after the fact.

## Limits that keep a malicious client from hurting the compositor

Every worker is reachable by any client asking for data, so `Bridge.cpp` caps them: 16
light (lists, portal keys, writing kept clipboards), 2 heavy (reading clipboards to keep,
saving pasted images); past the cap the reader gets an empty transfer. Images are capped at
64 MiB, kept text/HTML/lists at 4 MiB each. The paste-image directory is opened with
`O_NOFOLLOW`, must be the user's and mode 0700, is written with `openat(O_EXCL|O_NOFOLLOW)`
and pruned with `unlinkat` relative to that fd, and holds at most 256 MiB (oldest first).
Kept clipboard bytes are zeroed when freed (best effort). Nothing blocks the compositor
thread except a few `stat`/`access` calls through procfs. Every worker body is wrapped in
try/catch (an exception escaping a thread is `std::terminate`: the compositor).
`AddFull` goes in batches of 200 fds (`SCM_MAX_FD` is 253). A list slower than 2 s or over
4 MiB is streamed through unchanged, never cut. `.flatpak-info` is opened
`O_NONBLOCK|O_NOFOLLOW` and must be a regular file (a FIFO would hang a worker). A pasted
image is saved once per clipboard and reused on later reads.

## Unload

PLUGIN_EXIT disarms the handlers, removes both hooks and clears a kept clipboard if it is the
current one: the workers that would answer its pastes are stopping, and the hook that would
free it on the next copy is gone.

## Paste image as file

Only for windows whose class is in `paste_image_apps`, and only when the clipboard holds an
image and no file list: their clipboard offer gains `x-special/gnome-copied-files` and
`text/uri-list`; reading either saves the image to `$XDG_RUNTIME_DIR/glowbuffer/Pasted
image <date> <time>.<ext>` (pruned after an hour) and returns that file. Every other app
sees the clipboard exactly as offered: a chat app offered a file list would attach a file
instead of pasting the picture. Nautilus and Dolphin paste images themselves; keep them out
of the list.

## Files from sandboxed apps to host apps

Telegram (Qt in Flatpak) offers files only as `application/vnd.portal.filetransfer`, plus
image bytes for pictures, so only pictures ever landed in Thunar. For a reader proven to be
a host app (`isHostPid`) of an offer with the key and no file list, glowbuffer adds `text/uri-list` (and
`x-special/gnome-copied-files` on the clipboard) and answers it from
`FileTransfer.RetrieveFiles`, which gives a host caller the real paths. On drags the accept
handler is replaced too, so the source hears the format it has.

## Clipboard kept after its app closes

Second hook: `CSeatManager::setCurrentSelection`. On a new app clipboard (a real
`wl_data_source`; not data-control, not Xwayland) glowbuffer reads plain text, HTML, file
lists and one image (ClipboardPlan.cpp, capped) and subscribes to the source's destruction
before SeatManager does. Its listener runs first and marks the source dying; when
SeatManager's clear follows, the hook sets a `CKeptSource` instead. A clear while the source
is alive (an app's "clear clipboard") stays a clear. Anything offering
`x-kde-passwordManagerHint` is never read. Kept bytes live in compositor memory until the
next copy or clear. Other secrets (a password copied from a terminal) are kept like any
clipboard manager would keep them; `persist_clipboard = 0` turns this off. The hook holds the current clipboard alive across Hyprland's call:
Hyprland calls `cancelled()` through a weak pointer, and the kept source exists only in
glowbuffer.

Not covered: Xwayland<->Wayland drags (Hyprland's X11 bridge), primary selection, Snaps.

## Commands

```sh
tests/run.sh                    # unit tests, plain g++, seconds
scripts/build-remote.sh         # build build/glowbuffer.so on the job runner (VPS_JOB=path/to/vps-job)
tests/itest/itest.sh            # end to end in a headless nested Hyprland; needs build/glowbuffer.so
GLOWBUFFER_ITEST_STOP=boot|control|config tests/itest/itest.sh   # stop early, for debugging the harness
GLOWBUFFER_ITEST_WLDEBUG=1 ...  # protocol traces in tests/itest/out/{hyprland.stdout,weston.log,wlproxy.log}
```

Do not compile the plugin on the workstation; it goes through `scripts/build-remote.sh`.
Never `hyprctl plugin load` it into the live session from an agent: a plugin crash takes
the user's desktop down. The integration test is the place to load it.

## Layout

- `src/main.cpp` — the hooks (`CWLDataOfferResource::sendData`,
  `CSeatManager::setCurrentSelection`), the replacement `set_actions`, `accept` and
  `receive` handlers, config, `hyprctl glowbuffer`.
- `src/Persist.*` — the kept clipboard. `src/ClipboardPlan.*` — what is read or added, pure.
- `src/Actions.hpp` — wlroots' action choice, pure. `src/UriList.*`, `src/Sandbox.*` —
  pure parsing. `src/PasteImage.*` — image paste rules, pure. `src/Bridge.*` — worker threads:
  sd-bus `Documents.AddFull`, saving pasted images.
- `tests/itest/` — `itest.sh` harness, `client.c` test client, `wlproxy.py` relay that
  lets the nested Hyprland start on weston (version bumps, fake seat, heartbeat; its
  docstring says why each is safe).

## Constraints that are easy to break

- Private members are reached with the explicit-instantiation trick (`robbed()` in
  main.cpp), never `#define private public`.
- The plugin links with `-z nodelete`: handlers installed on live offers outlive
  PLUGIN_EXIT. PLUGIN_EXIT disarms (`g_armed`), it does not free `g`. A reload runs in the
  same image with the old statics (`Bridge::start()` resets the stop flag).
- No D-Bus and no logging on the compositor thread's hot path; the worker never logs (the
  Hyprland logger is not known to be thread-safe) and never touches Hyprland state.
- Every bridge failure forwards the source's bytes unchanged.
- itest isolation: `env -i`, private `XDG_RUNTIME_DIR`, `dbus-run-session`,
  `HYPRLAND_NO_SD_VARS=1`, `LIBSEAT_BACKEND=seatd` (otherwise the nested Hyprland asks
  logind to take control of the real session before falling back to Wayland). Keep all of
  them.
