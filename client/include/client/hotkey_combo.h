#pragma once

// Matching of the global hotkeys (push-to-talk, mute, deafen) against the
// keyboard state. Pure functions so the rules can be tested without Win32.
//
// A combo is a regular key, an optional second regular key and a modifier
// bitmask (1 = Ctrl, 2 = Shift, 4 = Alt). Callers pass key2_down = true for a
// combo without a second key.

namespace parties::client {

constexpr int HOTKEY_MOD_CTRL  = 1;
constexpr int HOTKEY_MOD_SHIFT = 2;
constexpr int HOTKEY_MOD_ALT   = 4;

// Push-to-talk is held while the player keeps playing, and games bind Shift,
// Ctrl and Alt to sprint, crouch and walk. So the combo engages when its keys
// are down together with at least its modifiers (extra ones are fine), and once
// engaged it stays engaged until one of its regular keys is released: pressing
// or releasing a modifier mid-sentence never cuts the transmission.
constexpr bool hold_combo_active(bool was_active, bool key_down, bool key2_down,
                                 int mods_down, int required_mods)
{
    if (!key_down || !key2_down) return false;
    if (was_active) return true;
    return (mods_down & required_mods) == required_mods;
}

// Toggles (mute, deafen) fire on the press of the exact combo: M must not
// toggle while typing a capital M, and Ctrl+M must not fire a plain-M binding.
constexpr bool toggle_combo_down(bool key_down, bool key2_down, int mods_down, int required_mods)
{
    return key_down && key2_down && mods_down == required_mods;
}

} // namespace parties::client
