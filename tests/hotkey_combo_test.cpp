// Offline tests for the global hotkey matching rules in client/hotkey_combo.h.

#include <client/hotkey_combo.h>

#include <iostream>

using namespace parties::client;

namespace {

bool fail(const char* msg) {
    std::cerr << msg << "\n";
    return false;
}

// Push-to-talk on V with no modifiers, the most common binding.
bool test_ptt_survives_game_modifiers() {
    bool active = hold_combo_active(false, true, true, 0, 0);
    if (!active) return fail("ptt: V engages");
    // Sprint (Shift), crouch (Ctrl), walk (Alt) while talking.
    for (int mods : {HOTKEY_MOD_SHIFT, HOTKEY_MOD_CTRL, HOTKEY_MOD_ALT,
                     HOTKEY_MOD_SHIFT | HOTKEY_MOD_CTRL, 0}) {
        active = hold_combo_active(active, true, true, mods, 0);
        if (!active) return fail("ptt: a modifier press cut the transmission");
    }
    if (hold_combo_active(active, false, true, HOTKEY_MOD_SHIFT, 0))
        return fail("ptt: releasing V must end it");
    return true;
}

// Already sprinting when starting to talk.
bool test_ptt_engages_with_extra_modifiers() {
    if (!hold_combo_active(false, true, true, HOTKEY_MOD_SHIFT, 0))
        return fail("ptt: V must engage while Shift is held");
    return true;
}

// Push-to-talk bound to Ctrl+B.
bool test_ptt_with_required_modifier() {
    if (hold_combo_active(false, true, true, 0, HOTKEY_MOD_CTRL))
        return fail("ptt: Ctrl+B must not engage on B alone");
    bool active = hold_combo_active(false, true, true, HOTKEY_MOD_CTRL | HOTKEY_MOD_SHIFT, HOTKEY_MOD_CTRL);
    if (!active) return fail("ptt: Ctrl+B engages with Shift held too");
    active = hold_combo_active(active, true, true, 0, HOTKEY_MOD_CTRL);
    if (!active) return fail("ptt: letting go of Ctrl while holding B keeps talking");
    return true;
}

// Two-key combo: either regular key ends it.
bool test_ptt_second_key() {
    bool active = hold_combo_active(false, true, true, 0, 0);
    if (hold_combo_active(active, true, false, 0, 0))
        return fail("ptt: releasing the second key must end it");
    if (hold_combo_active(false, true, false, 0, 0))
        return fail("ptt: one of two keys must not engage");
    return true;
}

bool test_toggles_stay_exact() {
    if (!toggle_combo_down(true, true, 0, 0)) return fail("toggle: M fires");
    if (toggle_combo_down(true, true, HOTKEY_MOD_SHIFT, 0))
        return fail("toggle: Shift+M must not fire a plain-M binding");
    if (toggle_combo_down(true, true, 0, HOTKEY_MOD_CTRL))
        return fail("toggle: M must not fire a Ctrl+M binding");
    if (!toggle_combo_down(true, true, HOTKEY_MOD_CTRL, HOTKEY_MOD_CTRL))
        return fail("toggle: Ctrl+M fires");
    return true;
}

} // namespace

int main() {
    bool ok = true;
    ok &= test_ptt_survives_game_modifiers();
    ok &= test_ptt_engages_with_extra_modifiers();
    ok &= test_ptt_with_required_modifier();
    ok &= test_ptt_second_key();
    ok &= test_toggles_stay_exact();
    std::cout << (ok ? "hotkey combo tests passed\n" : "hotkey combo tests FAILED\n");
    return ok ? 0 : 1;
}
