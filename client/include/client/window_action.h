#pragma once

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Property.h>
#include <RmlUi/Core/PropertyDefinition.h>
#include <RmlUi/Core/StyleSheetSpecification.h>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace parties::client {

enum class WindowAction : int { None, Caption, Close, Minimize, Maximize };

inline constexpr std::string_view kWindowActionProperty = "window-action";

inline constexpr std::array<std::string_view, 5> kWindowActionKeywords{
    "none", "caption", "close", "minimize", "maximize"};
static_assert(kWindowActionKeywords.size() == static_cast<std::size_t>(WindowAction::Maximize) + 1);

inline void register_window_action_property() {
    std::string keywords;
    for (const auto keyword : kWindowActionKeywords) {
        if (!keywords.empty()) keywords += ", ";
        keywords += keyword;
    }
    Rml::StyleSheetSpecification::RegisterProperty(
        Rml::String(kWindowActionProperty), Rml::String(kWindowActionKeywords[0]), true)
        .AddParser("keyword", keywords);
}

inline WindowAction window_action_of(Rml::Element& element) {
    const Rml::Property* property = element.GetProperty(Rml::String(kWindowActionProperty));
    if (!property || property->unit != Rml::Unit::KEYWORD) return WindowAction::None;
    const int index = property->value.Get<int>();
    if (index < 0 || index >= static_cast<int>(kWindowActionKeywords.size())) return WindowAction::None;
    return static_cast<WindowAction>(index);
}

}
