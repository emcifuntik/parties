#pragma once

#include <RmlUi/Core/Element.h>


namespace parties::client {

class DiagnosticsRecorder;

// <diag_graph kind="net|rtt|video" scale="..." stream="..." revision="..."/>
//
// A bar chart of the last minute of DiagnosticsRecorder history, two layers:
//   net    download kbps (wide bars), upload kbps (narrow) against `scale` kbps
//   rtt    RTT ms against `scale` ms, packet loss (the worse direction) against 20 %
//   video  presented fps of `stream` against `scale`, buckets with a freeze in red
// Bind `revision` to the model's revision so the chart redraws when a bucket
// closes. The vertex count is fixed: geometry is compiled once per size and
// then updated in place (no GPU resource churn), like LevelMeterElement.
class DiagnosticsGraphElement : public Rml::Element {
public:
    explicit DiagnosticsGraphElement(const Rml::String& tag);
    ~DiagnosticsGraphElement() override;

    // Main thread. The recorder every graph reads; null detaches them.
    static void set_source(const DiagnosticsRecorder* recorder);

    static constexpr float LOSS_SCALE_PCT = 20.0f;

protected:
    void OnAttributeChange(const Rml::ElementAttributes& changed_attributes) override;
    void OnRender() override;
    void OnResize() override;

private:
    void ReleaseResources();
    void RebuildGeometry();
    void UpdateVertices();

    Rml::CompiledGeometryHandle geom_ = 0;
    float cached_w_ = 0;
    float cached_h_ = 0;
    bool dirty_ = true;
};

} // namespace parties::client
