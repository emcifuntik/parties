#pragma once

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementInstancer.h>

namespace parties::client {

// Custom RmlUi element that renders a voice level meter without geometry recompilation.
// Compiles VB/IB once, then updates vertex positions via Map/Unmap when level changes.
// This avoids CreateCommittedResource churn that causes high GPU Copy engine usage.
class LevelMeterElement : public Rml::Element {
public:
    explicit LevelMeterElement(const Rml::String& tag);
    ~LevelMeterElement() override;

    void SetLevel(float level);       // 0.0 - 1.0
    void SetThreshold(float thresh);  // 0.0 - 1.0

protected:
    void OnRender() override;
    void OnResize() override;

private:
    void ReleaseResources();
    void RebuildGeometry();
    void UpdateVertices();

    Rml::CompiledGeometryHandle geom_ = 0;
    float cached_w_ = 0;
    float cached_h_ = 0;

    float level_ = 0.0f;
    float threshold_ = 0.02f;
    bool dirty_ = true;
};

class LevelMeterInstancer : public Rml::ElementInstancer {
public:
    Rml::ElementPtr InstanceElement(Rml::Element* parent, const Rml::String& tag,
                                     const Rml::XMLAttributes& attributes) override;
    void ReleaseElement(Rml::Element* element) override;
};

} // namespace parties::client
