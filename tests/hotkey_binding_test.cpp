#include <client/hotkey_binding.h>

#include <cstdio>
#include <map>
#include <optional>
#include <string>

namespace {

using namespace parties::client;

struct FakePrefs {
    std::map<std::string, std::string> values;

    std::optional<std::string> get_pref(const std::string& key) {
        const auto it = values.find(key);
        if (it == values.end()) return std::nullopt;
        return it->second;
    }
    bool set_pref(const std::string& key, const std::string& value) {
        values[key] = value;
        return true;
    }
};

bool check(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "Hotkey binding: %s\n", message);
    return condition;
}

std::string key_name(uint16_t key) {
    return std::string(1, static_cast<char>(key));
}

}

int main() {
    bool passed = true;

    const auto ctrl_shift = HotkeyModifiers{}.with(HotkeyModifier::Ctrl).with(HotkeyModifier::Shift);
    passed &= check(ctrl_shift.persisted_bits() == 3, "Ctrl+Shift must persist as bits 1|2");
    passed &= check(HotkeyModifiers::from_persisted_bits(4).has(HotkeyModifier::Alt) &&
                    !HotkeyModifiers::from_persisted_bits(4).has(HotkeyModifier::Ctrl), "Alt bit 4");
    passed &= check(HotkeyModifiers::from_persisted_bits(8 | 1) == HotkeyModifiers{}.with(HotkeyModifier::Ctrl),
                    "unknown modifier bits were kept");
    passed &= check(HotkeyModifiers{} != ctrl_shift, "modifier matching is not exact");

    const auto ab = hotkey_from_prefs("65", "66", "3");
    passed &= check(ab && ab->primary_key == 65 && ab->second_key == 66 && ab->modifiers == ctrl_shift,
                    "two-key binding did not parse");
    const auto single = hotkey_from_prefs("32", "0", "0");
    passed &= check(single && single->primary_key == 32 && !single->second_key && single->modifiers == HotkeyModifiers{},
                    "key2 0 must mean no second key");
    passed &= check(!hotkey_from_prefs("0", "0", "0"), "key 0 must mean unset");
    passed &= check(!hotkey_from_prefs("", "", ""), "empty key must mean unset");
    passed &= check(!hotkey_from_prefs("F1", "0", "0"), "non-numeric key was accepted");
    passed &= check(!hotkey_from_prefs("65", "x", "0"), "non-numeric second key was accepted");

    passed &= check(hotkey_label(*ab, key_name) == "Ctrl+Shift+B+A", "label order changed");
    passed &= check(hotkey_label(*single, key_name) == " ", "single key label changed");

    FakePrefs prefs;
    passed &= check(!load_hotkey(prefs, HotkeyAction::ToggleMute), "missing prefs produced a binding");
    prefs.values["audio.mute_key"] = "77";
    const auto mute = load_hotkey(prefs, HotkeyAction::ToggleMute);
    passed &= check(mute && mute->primary_key == 77 && !mute->second_key && mute->modifiers == HotkeyModifiers{},
                    "binding without key2 and mods prefs did not load");

    save_hotkey(prefs, HotkeyAction::PushToTalk, *ab);
    passed &= check(prefs.values["audio.ptt_key"] == "65" && prefs.values["audio.ptt_key2"] == "66" &&
                    prefs.values["audio.ptt_mods"] == "3", "push to talk persisted format changed");
    save_hotkey(prefs, HotkeyAction::ToggleDeafen, *single);
    passed &= check(prefs.values["audio.deafen_key"] == "32" && prefs.values["audio.deafen_key2"] == "0" &&
                    prefs.values["audio.deafen_mods"] == "0", "deafen persisted format changed");
    passed &= check(load_hotkey(prefs, HotkeyAction::PushToTalk) == ab, "push to talk did not round-trip");
    passed &= check(load_hotkey(prefs, HotkeyAction::ToggleDeafen) == single, "deafen did not round-trip");

    return passed ? 0 : 1;
}
