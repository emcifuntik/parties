#include <RmlUi/Core.h>

#include <cstdio>
#include <unordered_set>

namespace {
class Diagnostics final : public Rml::SystemInterface {
public:
    bool failed = false;
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override {
        if (type == Rml::Log::LT_ERROR || type == Rml::Log::LT_ASSERT) {
            failed = true;
            std::fprintf(stderr, "RmlUi: %s\n", message.c_str());
        }
        return true;
    }
};

class Renderer final : public Rml::RenderInterface {
public:
    int draws = 0;
    bool failed = false;
    std::unordered_set<Rml::CompiledGeometryHandle> geometries;
    std::unordered_set<Rml::TextureHandle> textures;

    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex>, Rml::Span<const int>) override {
        auto handle = reinterpret_cast<Rml::CompiledGeometryHandle>(new int);
        geometries.insert(handle);
        return handle;
    }
    void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f, Rml::TextureHandle texture) override {
        if (!geometries.count(geometry) || (texture && !textures.count(texture))) failed = true;
        if (texture) ++draws;
    }
    void ReleaseGeometry(Rml::CompiledGeometryHandle handle) override {
        if (geometries.erase(handle)) delete reinterpret_cast<int*>(handle);
        else failed = true;
    }
    Rml::TextureHandle LoadTexture(Rml::Vector2i&, const Rml::String&) override { return {}; }
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte>, Rml::Vector2i) override {
        auto handle = reinterpret_cast<Rml::TextureHandle>(new int);
        textures.insert(handle);
        return handle;
    }
    void ReleaseTexture(Rml::TextureHandle handle) override {
        if (textures.erase(handle)) delete reinterpret_cast<int*>(handle);
        else failed = true;
    }
    void EnableScissorRegion(bool) override {}
    void SetScissorRegion(Rml::Rectanglei) override {}
};

Rml::ElementDocument* LoadIcon(Rml::Context* context, const char* colour) {
    const Rml::String markup = Rml::String("<rml><head></head><body><svg width=\"20\" height=\"20\" style=\"image-color: ")
        + colour + ";\" src=\"" PARTIES_UI_SOURCE_DIR "/icon-undeafen.svg\" /></body></rml>";
    auto* document = context->LoadDocumentFromMemory(markup);
    if (document) document->Show();
    return document;
}
} // namespace

int main() {
    Diagnostics diagnostics;
    Renderer main_renderer, pip_renderer;
    Rml::SetSystemInterface(&diagnostics);
    Rml::SetRenderInterface(&main_renderer);
    if (!Rml::Initialise()) return 1;
    auto* main_context = Rml::CreateContext("main", {200, 100}, &main_renderer);
    if (!main_context || !LoadIcon(main_context, "#ffffff")) return 1;
    main_context->Update();
    main_context->Render();

    // Equal colours exercise cached geometry; different colours exercise the
    // texture shared by separate geometry entries. Reopening also checks cleanup.
    bool passed = main_renderer.draws > 0;
    for (const char* colour : {"#ffffff", "#dce1ea", "#ffffff", "#dce1ea"}) {
        auto* pip_context = Rml::CreateContext("pip", {200, 100}, &pip_renderer);
        if (!pip_context || !LoadIcon(pip_context, colour)) return 1;
        const int main_draws = main_renderer.draws;
        const int pip_draws = pip_renderer.draws;
        pip_context->Update();
        pip_context->Render();
        passed &= pip_renderer.draws > pip_draws && main_renderer.draws == main_draws;
        Rml::RemoveContext("pip");
        main_context->Update();
        main_context->Render();
    }
    Rml::RemoveContext("main");
    Rml::Shutdown();
    passed &= !diagnostics.failed && !main_renderer.failed && !pip_renderer.failed;
    passed &= main_renderer.geometries.empty() && main_renderer.textures.empty();
    passed &= pip_renderer.geometries.empty() && pip_renderer.textures.empty();
    if (!passed) std::fprintf(stderr, "SVG resources crossed render managers or were not released\n");
    return passed ? 0 : 1;
}
