#pragma once

// Drag-and-drop action negotiation, as the Wayland protocol describes it and as wlroots and
// Mutter implement it. Pure logic: no Hyprland, no wayland headers, so tests/unit.cpp can
// exercise every rule without a compositor.
//
// What Hyprland 0.56 does instead (src/protocols/core/DataDevice.cpp,
// CWLDataOfferResource::sendData): it sends wl_data_offer.action(MOVE) the moment an offer is
// created, before wl_data_device.enter, whenever the source allows MOVE, and it never handles
// wl_data_offer.set_actions. GTK4 reads that early MOVE as the only action on the table, so a
// drop target that accepts copy alone (File Roller, most GNOME apps, portal dialogs) refuses
// every drag out of a file manager. The source is never told the negotiated action either, so
// a file manager drag into a removable drive "moves" whatever the target asked for.
// Upstream: hyprwm/Hyprland#13578, closed by a bot, never fixed.

#include <bit>
#include <cstdint>

namespace Glowbuffer {

    // wl_data_device_manager.dnd_action, spelled out so this header needs no wayland headers.
    inline constexpr uint32_t ACTION_NONE = 0;
    inline constexpr uint32_t ACTION_COPY = 1;
    inline constexpr uint32_t ACTION_MOVE = 2;
    inline constexpr uint32_t ACTION_ASK  = 4;
    inline constexpr uint32_t ACTION_ALL  = ACTION_COPY | ACTION_MOVE | ACTION_ASK;

    // Hyprland's eKeyboardModifiers bits (devices/IKeyboard.hpp), which getModsFromAllKBs()
    // returns. Restated for the same reason as above; main.cpp static_asserts they still match.
    inline constexpr uint32_t MOD_SHIFT = 1u << 0;
    inline constexpr uint32_t MOD_CTRL  = 1u << 2;

    // wl_data_offer.action exists from version 3; older targets cannot negotiate and the
    // protocol treats them as copy-only.
    inline constexpr uint32_t OFFER_ACTIONS_SINCE_VERSION = 3;

    // The action the user forces with a held modifier, per the wl_data_device_manager
    // documentation: Shift selects move, Control selects copy. Both, or neither, force nothing.
    constexpr uint32_t compositorAction(uint32_t mods) {
        const bool SHIFT = mods & MOD_SHIFT;
        const bool CTRL  = mods & MOD_CTRL;
        if (SHIFT && !CTRL)
            return ACTION_MOVE;
        if (CTRL && !SHIFT)
            return ACTION_COPY;
        return ACTION_NONE;
    }

    constexpr uint32_t lowestAction(uint32_t actions) {
        return actions ? (1u << std::countr_zero(actions)) : ACTION_NONE;
    }

    struct SNegotiationInput {
        // wl_data_source.set_actions. 0 means the source never declared any, which wlroots
        // treats as copy-only rather than as "nothing allowed" (and so do we).
        uint32_t sourceActions = 0;
        // wl_data_offer.set_actions, as the target last sent them (0/0 until it does).
        uint32_t targetActions   = 0;
        uint32_t targetPreferred = 0;
        uint32_t targetVersion   = OFFER_ACTIONS_SINCE_VERSION;
        // compositorAction() of the modifiers held right now
        uint32_t forced = ACTION_NONE;
    };

    // wlroots' data_offer_choose_action(), rule for rule:
    //   1. what both sides allow; nothing in common is ACTION_NONE (the drop will be refused)
    //   2. a modifier-forced action, when both sides allow it
    //   3. the target's preferred action, when the source allows it
    //   4. otherwise the first allowed action in bit order: copy, then move, then ask
    // A target that has not called set_actions yet allows nothing, so before it speaks the
    // negotiated action is NONE. That is the point: no action is announced before enter.
    constexpr uint32_t chooseAction(const SNegotiationInput& in) {
        const bool     V3        = in.targetVersion >= OFFER_ACTIONS_SINCE_VERSION;
        const uint32_t TARGET    = V3 ? (in.targetActions & ACTION_ALL) : ACTION_COPY;
        const uint32_t PREFERRED = V3 ? (in.targetPreferred & ACTION_ALL) : ACTION_NONE;
        const uint32_t SOURCE    = in.sourceActions ? (in.sourceActions & ACTION_ALL) : ACTION_COPY;
        const uint32_t AVAILABLE = TARGET & SOURCE;

        if (!AVAILABLE)
            return ACTION_NONE;
        if (in.forced & AVAILABLE)
            return lowestAction(in.forced & AVAILABLE);
        if (PREFERRED & AVAILABLE)
            return lowestAction(PREFERRED & AVAILABLE);
        return lowestAction(AVAILABLE);
    }

} // namespace Glowbuffer
