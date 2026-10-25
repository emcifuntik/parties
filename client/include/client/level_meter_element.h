#pragma once

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementInstancer.h>

namespace parties::client {

// Compiles VB/IB once, then updates vertex positions via Map/Unmap when level changes.
// This avoids CreateCommittedResource churn that causes high GPU Copy engine usage.
class LevelMeterElement : public Rml::Element {
public:
    explicit LevelMeterElement(const Rml::String& tag);
    ~LevelMeterElement() override;

    void SetLevel(float level);
    void SetThreshold(float thresh);
    float GetLevel() const { return level_; }
    float GetThreshold() const { return threshold_; }

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

    float level_ = 0.0f;
    float threshold_ = 0.02f;
    bool dirty_ = true;
};

}
