#include <client/lobby_model.h>
#include <RmlUi/Core.h>

#include <cstdio>

namespace {
class NullRenderer final : public Rml::RenderInterface {
public:
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex>, Rml::Span<const int>) override { return 1; }
    void RenderGeometry(Rml::CompiledGeometryHandle, Rml::Vector2f, Rml::TextureHandle) override {}
    void ReleaseGeometry(Rml::CompiledGeometryHandle) override {}
    Rml::TextureHandle LoadTexture(Rml::Vector2i&, const Rml::String&) override { return {}; }
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte>, Rml::Vector2i) override { return {}; }
    void ReleaseTexture(Rml::TextureHandle) override {}
    void EnableScissorRegion(bool) override {}
    void SetScissorRegion(Rml::Rectanglei) override {}
};

bool check(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "Channel streams: %s\n", message);
    return condition;
}
} // namespace

int main() {
    using namespace parties::client;
    NullRenderer renderer;
    Rml::SetRenderInterface(&renderer);
    if (!Rml::Initialise()) return 1;
    auto* context = Rml::CreateContext("channel-streams", {640, 480});
    if (!context) return 2;
    bool passed = true;
    {
        LobbyModel model;
        if (!model.init(context)) return 3;
        ChannelUser alice; alice.id = 11; alice.name = "Alice";
        ChannelUser bob; bob.id = 22; bob.name = "Bob";
        ChannelInfo first; first.id = 1; first.users = {alice};
        ChannelInfo second; second.id = 2; second.users = {bob};
        model.channels = Rml::Vector<ChannelInfo>{first, second};
        model.current_channel = 1;

        passed &= check(model.add_channel_sharer(11), "current-channel share was rejected");
        passed &= check(!model.add_channel_sharer(22), "foreign-channel share was accepted");
        passed &= check(!model.add_channel_sharer(99), "unknown sharer was accepted");
        passed &= check(model.sharers.get().size() == 1 && model.someone_sharing.get(), "wrong sharer list");
        model.sharers.silent().front().watching = true;
        passed &= check(model.add_channel_sharer(11) && model.sharers.get().size() == 1 &&
            model.sharers.get().front().watching, "replay duplicated or reset a watched share");

        model.clear_channel_sharers();
        model.current_channel = 2;
        passed &= check(model.sharers.get().empty() && !model.someone_sharing.get(), "old channel shares survived transition");
        passed &= check(!model.channels.get()[0].users[0].streaming, "old streaming badge survived transition");
        passed &= check(!model.add_channel_sharer(11), "delayed old-channel start was accepted");
        passed &= check(model.add_channel_sharer(22) && model.sharers.get().front().name == "Bob", "new-channel replay failed");

        // A stale sidebar badge must not make another channel's stream clickable.
        model.channels.silent()[0].users[0].streaming = true;
        int watched_id = 0;
        model.on_watch_sharer = [&](int id) { watched_id = id; };
        auto* document = context->LoadDocumentFromMemory(R"RML(
<rml><body data-model="lobby">
<button id="foreign" data-event-click="watch_user_stream(11)">Foreign stream</button>
<button id="current" data-event-click="watch_user_stream(22)">Current stream</button>
</body></rml>)RML");
        if (!document) return 4;
        document->Show();
        context->Update();
        document->GetElementById("foreign")->DispatchEvent("click", {});
        passed &= check(watched_id == 0, "foreign-channel watch action escaped filtering");
        document->GetElementById("current")->DispatchEvent("click", {});
        passed &= check(watched_id == 22, "current-channel watch action was blocked");
        document->Close();

        model.remove_channel_sharer(22);
        passed &= check(model.sharers.get().empty() && !model.someone_sharing.get() &&
            !model.channels.get()[1].users[0].streaming, "stopped stream remained visible");
        model.clear_channel_sharers();
        model.current_channel = 0;
        passed &= check(!model.add_channel_sharer(11) && !model.add_channel_sharer(22), "share was accepted outside a channel");
    }
    Rml::RemoveContext("channel-streams");
    Rml::Shutdown();
    return passed ? 0 : 1;
}
