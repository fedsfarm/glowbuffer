# GlowBuffer
[![license](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://github.com/fedsfarm/glowbuffer/blob/main/LICENSE) [![Matrix](https://img.shields.io/badge/Matrix-Join%20chat-green?logo=matrix&logoColor=white)](https://escape.feds.farm/#main:feds.farm) [![Donate](https://img.shields.io/badge/Donate-XMR%20%C2%B7%20BTC%20%C2%B7%20ETH-orange?logo=monero&logoColor=white&labelColor=555)](#donate)

- Dropping files from Thunar into File Roller and other GTK4 apps works.
- Holding Shift or Ctrl while dragging moves or copies.
- Dropping files from Thunar into Telegram, Signal and other Flatpak apps works.
- Pasting files copied in Thunar into Telegram, Signal and other Flatpak apps works.
- Dragging videos and files (not just pictures) out of Telegram and other Flatpak apps into
  Thunar works.
- Copying in eog (or any app) and then closing it no longer empties the clipboard.
  Password managers' clipboards are still dropped with the app.
- Pasting a screenshot or a copied image into Thunar, Nemo, Caja or PCManFM saves it as a
  file.

## Install

Via hyprpm:

```sh
hyprpm add https://github.com/fedsfarm/glowbuffer
hyprpm enable glowbuffer
```

## Manual build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++
cmake --build build -j
```

Produces `build/glowbuffer.so`. The ABI must match the running Hyprland exactly —
build against the same headers, or a version skew gives a `.so` that refuses to load.
Needs `libsystemd` (sd-bus). The plugin stays mapped after `hyprctl plugin unload`
(clipboard objects keep calling into it), so a new build loads from a new path or
after a Hyprland restart. Unloading clears a clipboard kept after its app closed.

## Usage

Nothing to bind. `hyprctl glowbuffer` (or `hyprctl -j glowbuffer`) shows whether it is
active and what it has done.

## Config

All keys live under `plugin:glowbuffer:*`.

| Option | Type | Default | Description |
|---|---|---|---|
| `negotiate_actions` | bool (0/1) | `1` | Negotiate the drag action with the drop target instead of forcing move |
| `modifiers` | bool (0/1) | `1` | Shift forces move, Control forces copy while dragging |
| `file_bridge` | bool (0/1) | `1` | Share dropped and pasted files with Flatpak apps through the document portal |
| `file_bridge_sources` | string | file and archive managers | Window classes whose dragged or copied files are shared with Flatpak apps |
| `paste_images` | bool (0/1) | `1` | Paste a clipboard image into a file manager as a new file |
| `paste_image_apps` | string | `thunar nemo caja pcmanfm pcmanfm-qt` | Window classes that get clipboard images as files |
| `persist_clipboard` | bool (0/1) | `1` | Keep the clipboard after the app that copied closes |

### Lua

```lua
    hl.config({
        plugin = {
            glowbuffer = {
                negotiate_actions = 1,
                modifiers         = 1,
                file_bridge       = 1,
                paste_images      = 1,
                paste_image_apps  = "thunar nemo caja pcmanfm pcmanfm-qt",
                persist_clipboard = 1,
            },
        },
    })
```

### hyprland.conf

```ini
plugin {
    glowbuffer {
        negotiate_actions = 1
        modifiers         = 1
        file_bridge       = 1
        paste_images      = 1
        paste_image_apps  = thunar nemo caja pcmanfm pcmanfm-qt
        persist_clipboard = 1
    }
}
```

## Contribute

If you want to contribute, you can check out the feature requests by users in the Discussions tab, or implement your own feature

It will be merged if it builds and fits. If you aren't sure, you can contact me (see below)

AI code is allowed if it's submitted and tested by a human

## Donate

#### XMR:
`42uxSBp4aMyTAsPCMGEwHvJyGpemr1c7kdjtFsD5tnEsU7XsnYMjseyXBzLWHkruSWFGbQWagsh31bBRdU7vDNUBAzm1Mo4`  

#### BTC:
`bc1p2xkwf9elq8wgajtq2cc6zthuh4k998tgnk6365cnjqgal7mpd09q4jtfq8`

#### ETH (ERC-20):
`0xBD636eBD3a6b9F046930101657459E90DA370e81`  

---

Email [root@feds.farm](mailto:root@feds.farm) or DM [@root:feds.farm](https://escape.feds.farm/#@root:feds.farm) on Matrix if you want your donation to be visible
