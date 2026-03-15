#pragma once

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementInstancer.h>

namespace parties::client {

// Custom RmlUi element that renders a circle filled with a seed-based gradient.
// Usage in RML: <gradient_circle seed="somehashstring" class="my-circle" />
// The gradient colors are deterministically generated from the seed string.
class GradientCircleElement : public Rml::Element {
public:
    explicit GradientCircleElement(const Rml::String& tag);
    ~GradientCircleElement() override;

    void SetSeed(const Rml::String& seed);

protected:
    void OnRender() override;
    void OnResize() override;
    void OnAttributeChange(const Rml::ElementAttributes& changed_attributes) override;

private:
    void ReleaseResources();
    void RebuildGeometry();

    // Generate RGBA color from HSV (h: 0-360, s/v: 0-1)
    static Rml::ColourbPremultiplied HsvToRgb(float h, float s, float v, uint8_t a = 255);
    // Deterministic hash
    static uint32_t HashString(const Rml::String& s);

    Rml::CompiledGeometryHandle geom_ = 0;
    float cached_w_ = 0;
    float cached_h_ = 0;
    Rml::String seed_;
    bool dirty_ = true;
};

class GradientCircleInstancer : public Rml::ElementInstancer {
public:
    Rml::ElementPtr InstanceElement(Rml::Element* parent, const Rml::String& tag,
                                     const Rml::XMLAttributes& attributes) override;
    void ReleaseElement(Rml::Element* element) override;
};

} // namespace parties::client
