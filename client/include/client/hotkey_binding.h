#pragma once

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace parties::client {

enum class HotkeyModifier : uint8_t { Ctrl = 1, Shift = 2, Alt = 4 };

class HotkeyModifiers {
public:
    constexpr HotkeyModifiers() = default;

    static constexpr HotkeyModifiers from_persisted_bits(int bits) {
        return HotkeyModifiers(static_cast<uint8_t>(bits & kKnownBits));
    }

    constexpr int persisted_bits() const { return bits_; }
    constexpr bool has(HotkeyModifier modifier) const {
        return (bits_ & static_cast<uint8_t>(modifier)) != 0;
    }
    constexpr HotkeyModifiers with(HotkeyModifier modifier) const {
        return HotkeyModifiers(static_cast<uint8_t>(bits_ | static_cast<uint8_t>(modifier)));
    }

    friend constexpr bool operator==(HotkeyModifiers, HotkeyModifiers) = default;

private:
    static constexpr uint8_t kKnownBits = 1 | 2 | 4;
    explicit constexpr HotkeyModifiers(uint8_t bits) : bits_(bits) {}
    uint8_t bits_ = 0;
};

struct HotkeyBinding {
    uint16_t                primary_key = 0;
    std::optional<uint16_t> second_key;
    HotkeyModifiers         modifiers;

    friend constexpr bool operator==(const HotkeyBinding&, const HotkeyBinding&) = default;
};

constexpr std::optional<HotkeyBinding> hotkey_from_keys(int primary_key, int second_key,
                                                        HotkeyModifiers modifiers) {
    constexpr int kMaxKey = 0xFFFF;
    if (primary_key <= 0 || primary_key > kMaxKey || second_key < 0 || second_key > kMaxKey)
        return std::nullopt;
    HotkeyBinding binding;
    binding.primary_key = static_cast<uint16_t>(primary_key);
    if (second_key != 0) binding.second_key = static_cast<uint16_t>(second_key);
    binding.modifiers = modifiers;
    return binding;
}

enum class HotkeyAction : uint8_t { PushToTalk, ToggleMute, ToggleDeafen };

inline constexpr std::array<HotkeyAction, 3> kHotkeyActions{
    HotkeyAction::PushToTalk, HotkeyAction::ToggleMute, HotkeyAction::ToggleDeafen};

constexpr std::size_t hotkey_index(HotkeyAction action) {
    return static_cast<std::size_t>(action);
}

constexpr std::string_view hotkey_pref_prefix(HotkeyAction action) {
    switch (action) {
        case HotkeyAction::PushToTalk:   return "audio.ptt";
        case HotkeyAction::ToggleMute:   return "audio.mute";
        case HotkeyAction::ToggleDeafen: return "audio.deafen";
    }
    return {};
}

struct HotkeyPrefKeys {
    std::string key;
    std::string second_key;
    std::string modifiers;
};

inline HotkeyPrefKeys hotkey_pref_keys(HotkeyAction action) {
    const std::string prefix(hotkey_pref_prefix(action));
    return {prefix + "_key", prefix + "_key2", prefix + "_mods"};
}

namespace detail {

inline std::optional<int> parse_hotkey_int(std::string_view text) {
    int value = 0;
    const char* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value);
    if (ec != std::errc{} || ptr != end) return std::nullopt;
    return value;
}

}

inline std::optional<HotkeyBinding> hotkey_from_prefs(std::string_view key, std::string_view second_key,
                                                      std::string_view modifiers) {
    const auto primary = detail::parse_hotkey_int(key);
    const auto second = second_key.empty() ? std::optional<int>(0) : detail::parse_hotkey_int(second_key);
    const auto bits = modifiers.empty() ? std::optional<int>(0) : detail::parse_hotkey_int(modifiers);
    if (!primary || !second || !bits) return std::nullopt;
    return hotkey_from_keys(*primary, *second, HotkeyModifiers::from_persisted_bits(*bits));
}

template <class Store>
std::optional<HotkeyBinding> load_hotkey(Store& store, HotkeyAction action) {
    const HotkeyPrefKeys keys = hotkey_pref_keys(action);
    const auto key = store.get_pref(keys.key);
    if (!key) return std::nullopt;
    const auto second_key = store.get_pref(keys.second_key);
    const auto modifiers = store.get_pref(keys.modifiers);
    return hotkey_from_prefs(*key, second_key.value_or(""), modifiers.value_or(""));
}

template <class Store>
void save_hotkey(Store& store, HotkeyAction action, const HotkeyBinding& binding) {
    const HotkeyPrefKeys keys = hotkey_pref_keys(action);
    store.set_pref(keys.key, std::to_string(binding.primary_key));
    store.set_pref(keys.second_key, std::to_string(binding.second_key.value_or(0)));
    store.set_pref(keys.modifiers, std::to_string(binding.modifiers.persisted_bits()));
}

template <class KeyName>
std::string hotkey_label(const HotkeyBinding& binding, KeyName&& key_name) {
    std::string label;
    if (binding.modifiers.has(HotkeyModifier::Ctrl)) label += "Ctrl+";
    if (binding.modifiers.has(HotkeyModifier::Shift)) label += "Shift+";
    if (binding.modifiers.has(HotkeyModifier::Alt)) label += "Alt+";
    if (binding.second_key) {
        label += key_name(*binding.second_key);
        label += "+";
    }
    label += key_name(binding.primary_key);
    return label;
}

}
