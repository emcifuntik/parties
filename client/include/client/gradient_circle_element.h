#pragma once

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementInstancer.h>

namespace parties::client {

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

    static Rml::ColourbPremultiplied HsvToRgb(float h, float s, float v, uint8_t a = 255);
    static uint32_t HashString(const Rml::String& s);

    Rml::CompiledGeometryHandle geom_ = 0;
    float cached_w_ = 0;
    float cached_h_ = 0;
    Rml::String seed_;
    bool dirty_ = true;
};

}
