#include <client/diagnostics_graph_element.h>
#include <client/diagnostics.h>

#include "RmlUi_RenderInterface_Extended.h"

#include <RmlUi/Core/RenderInterface.h>
#include <parties/profiler.h>

#include <algorithm>
#include <vector>

namespace parties::client {

namespace {

using History = DiagnosticsRecorder::History;
using Series = DiagnosticsRecorder::Series;

constexpr size_t LAYERS = 2;
constexpr size_t QUADS = LAYERS * DiagnosticsRecorder::BUCKETS;

const DiagnosticsRecorder* g_source = nullptr;

Rml::ColourbPremultiplied colour(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return Rml::ColourbPremultiplied(static_cast<uint8_t>(r * a / 255), static_cast<uint8_t>(g * a / 255),
                                     static_cast<uint8_t>(b * a / 255), a);
}

} // namespace

void DiagnosticsGraphElement::set_source(const DiagnosticsRecorder* recorder) {
    g_source = recorder;
}

DiagnosticsGraphElement::DiagnosticsGraphElement(const Rml::String& tag)
    : Rml::Element(tag) {}

DiagnosticsGraphElement::~DiagnosticsGraphElement() {
    ReleaseResources();
}

void DiagnosticsGraphElement::OnAttributeChange(const Rml::ElementAttributes& changed_attributes) {
    Rml::Element::OnAttributeChange(changed_attributes);
    dirty_ = true;
}

void DiagnosticsGraphElement::ReleaseResources() {
    if (geom_) {
        if (auto* ri = Rml::GetRenderInterface())
            ri->ReleaseGeometry(geom_);
        geom_ = 0;
    }
}

void DiagnosticsGraphElement::RebuildGeometry() {
    ReleaseResources();
    auto* ri = Rml::GetRenderInterface();
    if (!ri) return;
    std::vector<Rml::Vertex> vertices(QUADS * 4);
    std::vector<int> indices(QUADS * 6);
    for (size_t q = 0; q < QUADS; ++q) {
        const int v = static_cast<int>(q * 4);
        int* i = &indices[q * 6];
        i[0] = v; i[1] = v + 1; i[2] = v + 2;
        i[3] = v; i[4] = v + 2; i[5] = v + 3;
    }
    geom_ = ri->CompileGeometry({vertices.data(), vertices.size()}, {indices.data(), indices.size()});
    const Rml::Vector2f size = GetBox().GetSize(Rml::BoxArea::Content);
    cached_w_ = size.x;
    cached_h_ = size.y;
    dirty_ = true;
}

void DiagnosticsGraphElement::UpdateVertices() {
    if (!geom_) return;
    auto* ext_ri = static_cast<ExtendedRenderInterface*>(Rml::GetRenderInterface());
    if (!ext_ri) return;

    const Rml::String kind = GetAttribute<Rml::String>("kind", "net");
    const float scale = (std::max)(GetAttribute<float>("scale", 1.0f), 1e-3f);
    History base{}, over{};
    float over_scale = scale;
    bool over_narrow = true;
    Rml::ColourbPremultiplied base_colour, over_colour;
    if (g_source) {
        if (kind == "rtt") {
            base = g_source->history(Series::RttMs);
            const History up = g_source->history(Series::UpLossPct);
            const History down = g_source->history(Series::DownLossPct);
            for (size_t i = 0; i < over.size(); ++i) over[i] = (std::max)(up[i], down[i]);
            over_scale = LOSS_SCALE_PCT;
            base_colour = colour(96, 165, 250, 220);
            over_colour = colour(248, 113, 113, 255);
        } else if (kind == "video") {
            const uint32_t stream = static_cast<uint32_t>(GetAttribute<int>("stream", 0));
            base = g_source->stream_fps(stream);
            over = g_source->stream_freezes(stream);
            over_scale = 1.0f;
            over_narrow = false;
            base_colour = colour(52, 211, 153, 220);
            over_colour = colour(248, 113, 113, 110);
        } else {
            base = g_source->history(Series::DownKbps);
            over = g_source->history(Series::UpKbps);
            base_colour = colour(94, 234, 212, 210);
            over_colour = colour(245, 158, 11, 255);
        }
    }

    const float w = cached_w_, h = cached_h_;
    const float step = w / static_cast<float>(DiagnosticsRecorder::BUCKETS);
    std::vector<Rml::Vertex> vertices(QUADS * 4);
    auto quad = [&](size_t q, float x0, float x1, float value, const Rml::ColourbPremultiplied& c) {
        const float top = h - std::clamp(value, 0.0f, 1.0f) * h;
        Rml::Vertex* v = &vertices[q * 4];
        v[0] = {{x0, top}, c, {0, 0}};
        v[1] = {{x1, top}, c, {0, 0}};
        v[2] = {{x1, h}, c, {0, 0}};
        v[3] = {{x0, h}, c, {0, 0}};
    };
    for (size_t i = 0; i < DiagnosticsRecorder::BUCKETS; ++i) {
        const float x0 = static_cast<float>(i) * step;
        const float x1 = x0 + (std::max)(step - 0.5f, 0.5f);
        quad(i, x0, x1, base[i] / scale, base_colour);
        const float inset = over_narrow ? step * 0.3f : 0.0f;
        quad(DiagnosticsRecorder::BUCKETS + i, x0 + inset, x1 - inset, over[i] / over_scale, over_colour);
    }
    ext_ri->UpdateGeometryVertices(geom_, {vertices.data(), vertices.size()});
    dirty_ = false;
}

void DiagnosticsGraphElement::OnResize() {
    Rml::Element::OnResize();
    dirty_ = true;
}

void DiagnosticsGraphElement::OnRender() {
    ZoneScopedN("DiagnosticsGraph::OnRender");
    auto* ri = Rml::GetRenderInterface();
    if (!ri) return;
    const Rml::Vector2f size = GetBox().GetSize(Rml::BoxArea::Content);
    if (size.x <= 0 || size.y <= 0) return;
    if (!geom_ || size.x != cached_w_ || size.y != cached_h_)
        RebuildGeometry();
    if (!geom_) return;
    if (dirty_) UpdateVertices();
    ri->RenderGeometry(geom_, GetAbsoluteOffset(Rml::BoxArea::Content), 0);
}

} // namespace parties::client
