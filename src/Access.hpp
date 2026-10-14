#pragma once

// Reaching into Hyprland objects whose wrappers are private. Defined once, in main.cpp, where
// the explicit-instantiation access trick lives (an explicit instantiation may appear only
// once in a program).

struct wl_client;
class IDataSource;

namespace Glowbuffer {
    // The client that owns a wl_data_source, or nullptr for any other kind of source
    // (Xwayland, data-control, glowbuffer's own kept clipboard).
    wl_client* sourceClient(IDataSource* source);
}
