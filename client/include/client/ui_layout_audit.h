#pragma once

#include <RmlUi/Core.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace parties::client {

// Measure rendered content, including anonymous text boxes. Checking computed
// 'align-items' alone misses later selectors that change the formatting context.
inline bool AuditUIControlLayout(Rml::ElementDocument* document, const char* scenario)
{
    const float dp = document->GetContext()->GetDensityIndependentPixelRatio();
    const auto viewport = document->GetContext()->GetDimensions();
    std::fprintf(stderr, "[UI audit] %s viewport=%.0fx%.0f dp scale=%.1f\n", scenario,
        viewport.x / dp, viewport.y / dp, dp);
    Rml::ElementList controls;
    document->QuerySelectorAll(controls,
        ".ui-button, .ui-icon-button, .ui-segment, .settings-keycap, .channel-add-btn, .sharer-tab");
    int checked = 0;
    int failures = 0;
    for (auto* control : controls) {
        if (!control->IsVisible(true)) continue;
        const auto origin = control->GetAbsoluteOffset(Rml::BoxArea::Border);
        const auto size = control->GetBox().GetSize(Rml::BoxArea::Border);
        if (size.x <= 0 || size.y <= 0) continue;
        Rml::Vector2f minimum(std::numeric_limits<float>::max());
        Rml::Vector2f maximum(std::numeric_limits<float>::lowest());
        const auto collect = [&](const auto& self, Rml::Element* element) -> void {
            if (!element->IsVisible(true)) return;
            if (auto* text = rmlui_dynamic_cast<Rml::ElementText*>(element)) {
                const auto& metrics = Rml::GetFontEngineInterface()->GetFontMetrics(text->GetFontFaceHandle());
                const float height = element->GetComputedValues().line_height().value;
                for (const auto& line : text->GetLines()) {
                    const int width = Rml::ElementUtilities::GetStringWidth(text, line.text);
                    if (width <= 0) continue;
                    auto start = element->GetAbsoluteOffset() + line.position;
                    start.y -= metrics.ascent + (height - metrics.ascent - metrics.descent) * 0.5f;
                    minimum.x = std::min(minimum.x, start.x);
                    minimum.y = std::min(minimum.y, start.y);
                    maximum.x = std::max(maximum.x, start.x + width);
                    maximum.y = std::max(maximum.y, start.y + height);
                }
                return;
            }
            if (element->GetTagName() == "svg") {
                for (int box_index = 0; box_index < element->GetNumBoxes(); ++box_index) {
                    Rml::Vector2f offset;
                    const auto extent = element->GetBox(box_index, offset).GetSize(Rml::BoxArea::Border);
                    if (extent.x <= 0 || extent.y <= 0) continue;
                    const auto start = element->GetAbsoluteOffset(Rml::BoxArea::Border) + offset;
                    minimum.x = std::min(minimum.x, start.x);
                    minimum.y = std::min(minimum.y, start.y);
                    maximum.x = std::max(maximum.x, start.x + extent.x);
                    maximum.y = std::max(maximum.y, start.y + extent.y);
                }
                return;
            }
            for (int i = 0; i < element->GetNumChildren(); ++i)
                self(self, element->GetChild(i));
        };
        collect(collect, control);
        if (maximum.x < minimum.x) {
            if (control->IsClassSet("ui-button") || control->IsClassSet("ui-segment")) {
                ++failures;
                std::fprintf(stderr, "[UI audit] FAIL %s: control has no laid-out label: %s\n",
                    scenario, control->GetClassNames().c_str());
            }
            continue; // CSS-drawn icons have no text or SVG.
        }
        ++checked;
        const auto delta = (minimum + maximum - size) * 0.5f - origin;
        const bool clipped = minimum.x < origin.x - dp || maximum.x > origin.x + size.x + dp ||
            minimum.y < origin.y - dp || maximum.y > origin.y + size.y + dp;
        if (std::abs(delta.x) > 1.5f * dp || std::abs(delta.y) > 1.5f * dp || clipped) {
            ++failures;
            std::fprintf(stderr,
                "[UI audit] FAIL %s <%s id='%s' class='%s'> size=%.1fx%.1f offset=%.1f,%.1f clipped=%d\n",
                scenario, control->GetTagName().c_str(), control->GetId().c_str(),
                control->GetClassNames().c_str(), size.x / dp, size.y / dp,
                delta.x / dp, delta.y / dp, clipped);
        }
    }
    Rml::ElementList fields;
    Rml::ElementList identities;
    document->QuerySelectorAll(identities, ".user-island-info");
    for (auto* identity : identities) {
        if (!identity->IsVisible(true)) continue;
        auto* avatar = identity->GetParentNode()->QuerySelector(".user-avatar");
        if (!avatar) continue;
        const float center = avatar->GetAbsoluteOffset(Rml::BoxArea::Border).y +
            avatar->GetBox().GetSize(Rml::BoxArea::Border).y * 0.5f;
        float top = std::numeric_limits<float>::max();
        float bottom = std::numeric_limits<float>::lowest();
        const auto collect_lines = [&](const auto& self, Rml::Element* element) -> void {
            if (!element->IsVisible(true)) return;
            if (auto* text = rmlui_dynamic_cast<Rml::ElementText*>(element)) {
                const auto& metrics = Rml::GetFontEngineInterface()->GetFontMetrics(text->GetFontFaceHandle());
                const float height = text->GetComputedValues().line_height().value;
                for (const auto& line : text->GetLines()) {
                    if (line.text.empty()) continue;
                    const float y = text->GetAbsoluteOffset().y + line.position.y -
                        metrics.ascent - (height - metrics.ascent - metrics.descent) * 0.5f;
                    top = std::min(top, y);
                    bottom = std::max(bottom, y + height);
                }
            }
            for (int i = 0; i < element->GetNumChildren(); ++i)
                self(self, element->GetChild(i));
        };
        collect_lines(collect_lines, identity);
        const float offset = (top + bottom) * 0.5f - center;
        if (bottom < top || std::abs(offset) > 1.5f * dp) {
            ++failures;
            std::fprintf(stderr, "[UI audit] FAIL %s identity text must center beside avatar: offset=%.1f dp\n",
                scenario, offset / dp);
        }
    }
    document->QuerySelectorAll(fields, "input.ui-input");
    for (auto* field : fields) {
        if (!field->IsVisible(true)) continue;
        const auto content = field->GetBox().GetSize(Rml::BoxArea::Content);
        if (content.x <= 0 || content.y <= 0) continue;
        const float line_height = field->GetComputedValues().line_height().value;
        if (line_height > content.y + dp) {
            ++failures;
            std::fprintf(stderr, "[UI audit] FAIL %s clipped input class='%s' content-height=%.1f line-height=%.1f\n",
                scenario, field->GetClassNames().c_str(), content.y / dp, line_height / dp);
        }
        if (field->IsClassSet("compose-input")) {
            auto* capsule = field->GetParentNode();
            const float center = capsule->GetAbsoluteOffset(Rml::BoxArea::Border).y +
                capsule->GetBox().GetSize(Rml::BoxArea::Border).y * 0.5f;
            // Input widgets own non-DOM text children, including the placeholder.
            for (int i = 0; i < field->GetNumChildren(true); ++i) {
                auto* text = rmlui_dynamic_cast<Rml::ElementText*>(field->GetChild(i));
                if (!text) continue;
                const auto& metrics = Rml::GetFontEngineInterface()->GetFontMetrics(text->GetFontFaceHandle());
                for (const auto& line : text->GetLines()) {
                    if (line.text.empty()) continue;
                    const float baseline = text->GetAbsoluteOffset().y + line.position.y;
                    const float offset = baseline + (metrics.descent - metrics.ascent) * 0.5f - center;
                    if (std::abs(offset) > 1.5f * dp) {
                        ++failures;
                        std::fprintf(stderr, "[UI audit] FAIL %s composer text vertical offset=%.1f dp\n",
                            scenario, offset / dp);
                    }
                }
            }
        }
    }
    Rml::ElementList targets;
    document->QuerySelectorAll(targets, "[data-event-click]");
    for (auto* target : targets) {
        if (!target->IsVisible(true)) continue;
        const auto size = target->GetBox().GetSize(Rml::BoxArea::Border);
        if (size.x <= 0 || size.y <= 0) continue;
        if (document->IsClassSet("platform-ios") && (size.x < 43.5f * dp || size.y < 43.5f * dp)) {
            ++failures;
            std::fprintf(stderr, "[UI audit] FAIL %s touch target <%s class='%s'> %.1fx%.1f action=%s\n",
                scenario, target->GetTagName().c_str(), target->GetClassNames().c_str(), size.x / dp, size.y / dp,
                target->GetAttribute<Rml::String>("data-event-click", "").c_str());
        }
        Rml::Vector2f scroll_before(0.f), scroll_after(0.f);
        for (auto* parent = target->GetParentNode(); parent; parent = parent->GetParentNode()) {
            const auto& style = parent->GetComputedValues();
            if (style.overflow_x() == Rml::Style::Overflow::Auto || style.overflow_x() == Rml::Style::Overflow::Scroll) {
                scroll_before.x += parent->GetScrollLeft();
                scroll_after.x += std::max(0.f, parent->GetScrollWidth() - parent->GetClientWidth() - parent->GetScrollLeft());
            }
            if (style.overflow_y() == Rml::Style::Overflow::Auto || style.overflow_y() == Rml::Style::Overflow::Scroll) {
                scroll_before.y += parent->GetScrollTop();
                scroll_after.y += std::max(0.f, parent->GetScrollHeight() - parent->GetClientHeight() - parent->GetScrollTop());
            }
        }
        const auto position = target->GetAbsoluteOffset(Rml::BoxArea::Border);
        if (position.x < -scroll_before.x - dp || position.x + size.x > viewport.x + scroll_after.x + dp ||
            position.y < -scroll_before.y - dp || position.y + size.y > viewport.y + scroll_after.y + dp) {
            ++failures;
            std::fprintf(stderr, "[UI audit] FAIL %s unreachable target class='%s' at %.1f,%.1f size %.1fx%.1f\n",
                scenario, target->GetClassNames().c_str(), position.x / dp, position.y / dp, size.x / dp, size.y / dp);
        }
    }
    std::fprintf(stderr, "[UI audit] %s: %d controls, %d failures\n", scenario, checked, failures);
    return failures == 0;
}

} // namespace parties::client
