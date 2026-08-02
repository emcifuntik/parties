#include <RmlUi/Core.h>

#include <cstdio>

namespace {

class TestRenderInterface final : public Rml::RenderInterface {
public:
    Rml::CompiledGeometryHandle CompileGeometry(
        Rml::Span<const Rml::Vertex>, Rml::Span<const int>) override { return {}; }
    void RenderGeometry(Rml::CompiledGeometryHandle, Rml::Vector2f, Rml::TextureHandle) override {}
    void ReleaseGeometry(Rml::CompiledGeometryHandle) override {}
    Rml::TextureHandle LoadTexture(Rml::Vector2i&, const Rml::String&) override { return {}; }
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte>, Rml::Vector2i) override { return {}; }
    void ReleaseTexture(Rml::TextureHandle) override {}
    void EnableScissorRegion(bool) override {}
    void SetScissorRegion(Rml::Rectanglei) override {}
};

} // namespace

int main() {
    TestRenderInterface renderer;
    Rml::SetRenderInterface(&renderer);
    if (!Rml::Initialise()) {
        std::fprintf(stderr, "failed to initialize RmlUi\n");
        return 1;
    }

    Rml::CallbackTextureSource shared_font_texture(
        [](const Rml::CallbackTextureInterface&) { return false; });
    Rml::RenderManager* persistent_manager = nullptr;

    for (int iteration = 0; iteration < 32; ++iteration) {
        const Rml::String name = "popup-lifecycle-" + std::to_string(iteration);
        Rml::Context* context = Rml::CreateContext(name, {420, 430}, &renderer);
        if (!context) {
            std::fprintf(stderr, "failed to create popup context %d\n", iteration);
            return 1;
        }

        Rml::RenderManager* current_manager = &context->GetRenderManager();
        if (!persistent_manager)
            persistent_manager = current_manager;
        else if (current_manager != persistent_manager) {
            std::fprintf(stderr, "renderer did not reuse its RenderManager\n");
            return 1;
        }

        // Mirrors an application-owned callback texture: it intentionally survives individual
        // popup contexts and is released only during global font-engine shutdown.
        (void)shared_font_texture.GetTexture(*current_manager);
        Rml::RemoveContext(name);
    }

    shared_font_texture = Rml::CallbackTextureSource{};
    Rml::Shutdown();
    return 0;
}
