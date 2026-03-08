#include <client/level_meter_element.h>

#include "dx12/RmlUi_Renderer_DX12.h"

#include <RmlUi/Core/RenderInterface.h>
#include <parties/profiler.h>

namespace parties::client {

// ── LevelMeterElement ──────────────────────────────────────────────

LevelMeterElement::LevelMeterElement(const Rml::String& tag)
    : Rml::Element(tag) {}

LevelMeterElement::~LevelMeterElement() {
    ReleaseResources();
}

void LevelMeterElement::SetLevel(float level) {
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    if (level_ != level) {
        level_ = level;
        dirty_ = true;
    }
}

void LevelMeterElement::SetThreshold(float thresh) {
    if (thresh < 0.0f) thresh = 0.0f;
    if (thresh > 1.0f) thresh = 1.0f;
    if (threshold_ != thresh) {
        threshold_ = thresh;
        dirty_ = true;
    }
}

void LevelMeterElement::ReleaseResources() {
    if (geom_) {
        if (auto* ri = Rml::GetRenderInterface())
            ri->ReleaseGeometry(geom_);
        geom_ = 0;
    }
}

void LevelMeterElement::RebuildGeometry() {
    ReleaseResources();

    // 8 vertices (2 quads), 12 indices (4 triangles)
    // Quad 0: fill bar (vertices 0-3, green)
    // Quad 1: threshold marker (vertices 4-7, red)
    Rml::Vertex vertices[8] = {};
    int indices[12] = {
        0, 1, 2, 0, 2, 3,  // fill bar
        4, 5, 6, 4, 6, 7,  // threshold marker
    };

    // Initialize colors
    Rml::ColourbPremultiplied green(61, 214, 140, 255);  // #3dd68c
    Rml::ColourbPremultiplied red(232, 100, 90, 255);    // #e8645a
    for (int i = 0; i < 4; ++i) vertices[i].colour = green;
    for (int i = 4; i < 8; ++i) vertices[i].colour = red;

    // Positions will be set by UpdateVertices after compile
    auto* ri = Rml::GetRenderInterface();
    if (!ri) return;
    geom_ = ri->CompileGeometry({vertices, 8}, {indices, 12});

    Rml::Vector2f size = GetBox().GetSize(Rml::BoxArea::Content);
    cached_w_ = size.x;
    cached_h_ = size.y;
    dirty_ = true;  // force vertex update
}

void LevelMeterElement::UpdateVertices() {
    if (!geom_) return;
    auto* ri = Rml::GetRenderInterface();
    if (!ri) return;
    auto* dx12_ri = static_cast<RenderInterface_DX12*>(ri);

    float w = cached_w_;
    float h = cached_h_;

    // Fill bar: 0 to level_ * width
    float fill_w = level_ * w;

    // Threshold marker: 2dp wide line at threshold_ * width
    float marker_x = threshold_ * w;
    float marker_w = 2.0f;  // ~2dp

    Rml::Vertex vertices[8] = {};
    Rml::ColourbPremultiplied green(61, 214, 140, 255);
    Rml::ColourbPremultiplied red(232, 100, 90, 255);

    // Fill bar quad
    vertices[0] = {{0, 0}, green, {0, 0}};
    vertices[1] = {{fill_w, 0}, green, {0, 0}};
    vertices[2] = {{fill_w, h}, green, {0, 0}};
    vertices[3] = {{0, h}, green, {0, 0}};

    // Threshold marker quad
    vertices[4] = {{marker_x, 0}, red, {0, 0}};
    vertices[5] = {{marker_x + marker_w, 0}, red, {0, 0}};
    vertices[6] = {{marker_x + marker_w, h}, red, {0, 0}};
    vertices[7] = {{marker_x, h}, red, {0, 0}};

    dx12_ri->UpdateGeometryVertices(geom_, {vertices, 8});
    dirty_ = false;
}

void LevelMeterElement::OnResize() {
    Rml::Element::OnResize();
    Rml::Vector2f size = GetBox().GetSize(Rml::BoxArea::Content);
    if (size.x != cached_w_ || size.y != cached_h_) {
        // Size changed — need new geometry (VB might be too small if we ever add more verts)
        // But since vertex count is fixed, just update positions
        cached_w_ = size.x;
        cached_h_ = size.y;
        dirty_ = true;
    }
}

void LevelMeterElement::OnRender() {
    ZoneScopedN("LevelMeter::OnRender");
    auto* ri = Rml::GetRenderInterface();
    if (!ri) return;

    Rml::Vector2f size = GetBox().GetSize(Rml::BoxArea::Content);
    if (size.x <= 0 || size.y <= 0) return;

    if (!geom_ || size.x != cached_w_ || size.y != cached_h_)
        RebuildGeometry();
    if (!geom_) return;

    if (dirty_)
        UpdateVertices();

    Rml::Vector2f offset = GetAbsoluteOffset(Rml::BoxArea::Content);
    ri->RenderGeometry(geom_, offset, 0);
}

// ── Instancer ──────────────────────────────────────────────────────

Rml::ElementPtr LevelMeterInstancer::InstanceElement(
    Rml::Element* /*parent*/, const Rml::String& tag,
    const Rml::XMLAttributes& /*attributes*/) {
    return Rml::ElementPtr(new LevelMeterElement(tag));
}

void LevelMeterInstancer::ReleaseElement(Rml::Element* element) {
    delete element;
}

} // namespace parties::client
