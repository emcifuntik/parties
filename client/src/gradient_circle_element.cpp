#include <client/gradient_circle_element.h>

#include "RmlUi_RenderInterface_Extended.h"

#include <RmlUi/Core/RenderInterface.h>

#include <cmath>
#include <cstring>

namespace parties::client {

// Circle approximation: 32-segment fan + center vertex = 33 vertices, 96 indices
static constexpr int SEGMENTS = 32;
static constexpr int NUM_VERTS = SEGMENTS + 1;   // center + rim
static constexpr int NUM_INDICES = SEGMENTS * 3;  // triangle fan

GradientCircleElement::GradientCircleElement(const Rml::String& tag)
    : Rml::Element(tag) {}

GradientCircleElement::~GradientCircleElement() {
    ReleaseResources();
}

uint32_t GradientCircleElement::HashString(const Rml::String& s) {
    uint32_t h = 2166136261u;
    for (char c : s) {
        h ^= static_cast<uint8_t>(c);
        h *= 16777619u;
    }
    return h;
}

Rml::ColourbPremultiplied GradientCircleElement::HsvToRgb(float h, float s, float v, uint8_t a) {
    float c = v * s;
    float x = c * (1.0f - std::fabs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
    float m = v - c;
    float r1, g1, b1;
    if (h < 60)       { r1 = c; g1 = x; b1 = 0; }
    else if (h < 120) { r1 = x; g1 = c; b1 = 0; }
    else if (h < 180) { r1 = 0; g1 = c; b1 = x; }
    else if (h < 240) { r1 = 0; g1 = x; b1 = c; }
    else if (h < 300) { r1 = x; g1 = 0; b1 = c; }
    else               { r1 = c; g1 = 0; b1 = x; }
    auto to_byte = [](float f) -> uint8_t { return static_cast<uint8_t>(f * 255.0f + 0.5f); };
    uint8_t r = to_byte(r1 + m);
    uint8_t g = to_byte(g1 + m);
    uint8_t b = to_byte(b1 + m);
    // Premultiply
    return Rml::ColourbPremultiplied(
        static_cast<uint8_t>(r * a / 255),
        static_cast<uint8_t>(g * a / 255),
        static_cast<uint8_t>(b * a / 255),
        a);
}

void GradientCircleElement::SetSeed(const Rml::String& seed) {
    if (seed_ != seed) {
        seed_ = seed;
        dirty_ = true;
    }
}

void GradientCircleElement::OnAttributeChange(const Rml::ElementAttributes& changed) {
    Rml::Element::OnAttributeChange(changed);
    auto it = changed.find("seed");
    if (it != changed.end()) {
        SetSeed(it->second.Get<Rml::String>());
    }
}

void GradientCircleElement::ReleaseResources() {
    if (geom_) {
        if (auto* ri = Rml::GetRenderInterface())
            ri->ReleaseGeometry(geom_);
        geom_ = 0;
    }
}

void GradientCircleElement::RebuildGeometry() {
    ReleaseResources();

    auto* ri = Rml::GetRenderInterface();
    if (!ri) return;

    Rml::Vector2f size = GetBox().GetSize(Rml::BoxArea::Content);
    float w = size.x;
    float h = size.y;
    if (w <= 0 || h <= 0) return;

    cached_w_ = w;
    cached_h_ = h;

    // Generate two gradient colors from seed
    uint32_t hash = HashString(seed_);
    float hue1 = static_cast<float>(hash % 360);
    float hue2 = static_cast<float>((hash / 360) % 360);
    // Ensure hues are at least 40 degrees apart for visual contrast
    if (std::fabs(hue2 - hue1) < 40.0f)
        hue2 = std::fmod(hue1 + 90.0f + static_cast<float>((hash >> 16) % 180), 360.0f);

    auto color1 = HsvToRgb(hue1, 0.65f, 0.90f);
    auto color2 = HsvToRgb(hue2, 0.70f, 0.85f);

    float cx = w * 0.5f;
    float cy = h * 0.5f;
    float rx = cx;
    float ry = cy;

    // Build triangle fan: vertex 0 = center, vertices 1..SEGMENTS = rim
    Rml::Vertex vertices[NUM_VERTS] = {};
    int indices[NUM_INDICES] = {};

    // Center vertex: blend of both colors
    Rml::ColourbPremultiplied center_color(
        static_cast<uint8_t>((color1.red + color2.red) / 2),
        static_cast<uint8_t>((color1.green + color2.green) / 2),
        static_cast<uint8_t>((color1.blue + color2.blue) / 2),
        255);
    vertices[0] = {{cx, cy}, center_color, {0.5f, 0.5f}};

    // Gradient direction: bottom to top (dy = -1)
    float grad_dx = 0.0f;
    float grad_dy = -1.0f;

    for (int i = 0; i < SEGMENTS; ++i) {
        float angle = static_cast<float>(i) / static_cast<float>(SEGMENTS) * 2.0f * 3.14159265f;
        float px = cx + rx * std::cos(angle);
        float py = cy + ry * std::sin(angle);

        // Gradient blend: project rim point onto gradient direction (-1 to 1) → 0 to 1
        float nx = (px - cx) / rx;  // normalized -1..1
        float ny = (py - cy) / ry;
        float t = (nx * grad_dx + ny * grad_dy) * 0.5f + 0.5f;

        Rml::ColourbPremultiplied c(
            static_cast<uint8_t>(color1.red + t * (color2.red - color1.red)),
            static_cast<uint8_t>(color1.green + t * (color2.green - color1.green)),
            static_cast<uint8_t>(color1.blue + t * (color2.blue - color1.blue)),
            255);
        vertices[i + 1] = {{px, py}, c, {0, 0}};
    }

    // Triangle fan indices
    for (int i = 0; i < SEGMENTS; ++i) {
        indices[i * 3 + 0] = 0;
        indices[i * 3 + 1] = i + 1;
        indices[i * 3 + 2] = (i + 1 < SEGMENTS) ? i + 2 : 1;
    }

    geom_ = ri->CompileGeometry({vertices, NUM_VERTS}, {indices, NUM_INDICES});
    dirty_ = false;
}

void GradientCircleElement::OnResize() {
    Rml::Element::OnResize();
    Rml::Vector2f size = GetBox().GetSize(Rml::BoxArea::Content);
    if (size.x != cached_w_ || size.y != cached_h_)
        dirty_ = true;
}

void GradientCircleElement::OnRender() {
    auto* ri = Rml::GetRenderInterface();
    if (!ri) return;

    Rml::Vector2f size = GetBox().GetSize(Rml::BoxArea::Content);
    if (size.x <= 0 || size.y <= 0) return;

    if (!geom_ || dirty_ || size.x != cached_w_ || size.y != cached_h_)
        RebuildGeometry();
    if (!geom_) return;

    Rml::Vector2f offset = GetAbsoluteOffset(Rml::BoxArea::Content);
    ri->RenderGeometry(geom_, offset, 0);
}

// ── Instancer ──────────────────────────────────────────────────────

Rml::ElementPtr GradientCircleInstancer::InstanceElement(
    Rml::Element* /*parent*/, const Rml::String& tag,
    const Rml::XMLAttributes& /*attributes*/) {
    return Rml::ElementPtr(new GradientCircleElement(tag));
}

void GradientCircleInstancer::ReleaseElement(Rml::Element* element) {
    delete element;
}

} // namespace parties::client
