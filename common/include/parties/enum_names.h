#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

namespace parties {

template <class E>
struct EnumName {
    E                value;
    std::string_view name;
};

template <class E, std::size_t N>
struct EnumNames {
    std::array<EnumName<E>, N> entries;

    constexpr std::string_view name(E v) const {
        for (const auto& e : entries)
            if (e.value == v) return e.name;
        return {};
    }

    constexpr std::optional<E> parse(std::string_view s) const {
        for (const auto& e : entries)
            if (e.name == s) return e.value;
        return std::nullopt;
    }

    constexpr E from_persisted_int(int raw, E fallback) const {
        for (const auto& e : entries)
            if (static_cast<int>(e.value) == raw) return e.value;
        return fallback;
    }
};

template <class E>
inline constexpr auto enum_names_v = nullptr;

}
