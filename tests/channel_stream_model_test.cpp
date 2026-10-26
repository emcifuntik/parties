#include <client/lobby_model.h>
#include <RmlUi/Core.h>

#include <cstdio>
#include <optional>
#include <string>

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

bool check_voice_navigation(Rml::Context* context, parties::client::LobbyModel& model) {
    using namespace parties::client;
    ChannelUser carol; carol.id = 33; carol.name = "Carol";
    model.channels.silent()[1].users.push_back(carol);
    model.add_channel_sharer(carol.id);
    model.on_join_channel = [&](int id) { model.show_voice_channel(id); };

    auto* document = context->LoadDocumentFromMemory(R"RML(
<rml><body class="ui-body" data-model="lobby">
<button class="ui-control" id="voice" data-event-mousedown="channel_mousedown(2, 'Voice')">Voice</button>
<div id="room" data-if="route == 'room' && current_channel > 0 && watching_count == 0">
    <button class="ui-control" id="watch" data-event-click="watch_user_stream(22)">Watch Bob</button>
</div>
<div id="viewer" data-if="route == 'streams' && watching_count > 0">
    <div id="sharer-list">
        <button class="ui-control" data-for="sharer : sharers" data-attr-id="'sharer-' + sharer.id"
                data-event-click="toggle_watch(sharer.id)">{{ sharer.name }}</button>
    </div>
</div>
<div id="chat" data-if="route == 'chat'">Chat</div>
</body></rml>)RML");
    if (!check(document != nullptr, "voice navigation document did not load")) return false;
    document->Show();
    bool passed = true;
    for (int count : {0, 1, 2}) {
        model.watched = Rml::Vector<WatchedStream>{};
        for (auto& sharer : model.sharers.silent()) sharer.watching = false;
        for (int i = 0; i < count; ++i) {
            auto& sharer = model.sharers.silent()[i];
            sharer.watching = true;
            model.watched.silent().push_back({sharer.id, sharer.name, "stream"});
        }
        model.sharers.notify();
        model.watched.notify();
        model.watching_count = count;
        model.router.go(DocumentRoute::Chat);
        context->Update();
        passed &= check(document->GetElementById("chat")->IsVisible(), "chat did not open");

        document->GetElementById("voice")->DispatchEvent("mousedown", {});
        context->Update();
        passed &= check(!document->GetElementById("chat")->IsVisible(), "voice navigation left chat visible");
        passed &= check(document->GetElementById("room")->IsVisible() == (count == 0),
            "return from chat did not restore room visibility");
        passed &= check(document->GetElementById("viewer")->IsVisible() == (count > 0),
            "return from chat hid the subscribed streams");
        passed &= check(model.current_channel.get() == 2 && model.watching_count.get() == count &&
            model.watched.get().size() == static_cast<size_t>(count), "navigation changed voice or watch state");
        passed &= check(model.sharers.get().size() == 2 && model.channels.get()[1].users[0].streaming,
            "return from chat lost sharers or streaming badges");

        int watched_id = 0;
        model.on_watch_sharer = [&](int id) { watched_id = id; };
        model.on_toggle_watch = [&](int id) { watched_id = id; };
        auto* watch = document->GetElementById(count == 0 ? "watch" : "sharer-33");
        passed &= check(watch && watch->IsVisible(true), "stream entry is not visible after returning from chat");
        if (watch) watch->DispatchEvent("click", {});
        passed &= check(watched_id == (count == 0 ? 22 : 33), "watch action was lost after returning from chat");
        model.on_watch_sharer = {};
        model.on_toggle_watch = {};
    }

    model.router.go(DocumentRoute::Chat);
    model.show_voice_channel(1);
    passed &= check(model.router.is(DocumentRoute::Room), "another channel reopened the old viewer");

    model.router.go(DocumentRoute::Chat);
    model.watched = Rml::Vector<WatchedStream>{};
    model.watching_count = 0;
    model.router.leave_streams();
    passed &= check(model.router.is(DocumentRoute::Chat), "stream teardown dismissed chat");
    document->GetElementById("voice")->DispatchEvent("mousedown", {});
    context->Update();
    passed &= check(document->GetElementById("room")->IsVisible(), "finished streams left an empty viewer");

    document->Close();
    model.on_join_channel = {};
    model.remove_channel_sharer(carol.id);
    return passed;
}
bool check_role_bindings(Rml::Context* context, parties::client::LobbyModel& model) {
    using namespace parties::client;
    ChannelUser admin; admin.id = 44; admin.name = "Ada"; admin.role = parties::Role::Admin;
    ChannelUser member; member.id = 55; member.name = "Max"; member.role = parties::Role::User;
    ChannelInfo room; room.id = 9; room.users = {admin, member};
    model.channels = Rml::Vector<ChannelInfo>{room};

    auto* document = context->LoadDocumentFromMemory(R"RML(
<rml><body data-model="lobby">
<div data-for="channel : channels">
    <div data-for="user : channel.users" data-attr-id="'user-' + user.id"
         data-event-mousedown="user_mousedown(user.id, user.name, user.role)">
        <span class="ui-micro" data-attr-id="'admin-' + user.id" data-if="user.role == 1">ADMIN</span>
    </div>
</div>
<button class="ui-control" id="make-mod" data-event-click="set_user_role(2)">Make moderator</button>
<button class="ui-control" id="make-bogus" data-event-click="set_user_role(9)">Bogus</button>
</body></rml>)RML");
    if (!check(document != nullptr, "role document did not load")) return false;
    document->Show();
    context->Update();
    bool passed = true;
    passed &= check(document->GetElementById("admin-44") && document->GetElementById("admin-44")->IsVisible(),
        "admin badge hidden for an admin");
    passed &= check(document->GetElementById("admin-55") && !document->GetElementById("admin-55")->IsVisible(),
        "admin badge shown for a member");

    int menu_user = 0;
    parties::Role menu_role = parties::Role::Owner;
    model.on_show_user_menu = [&](int id, std::string, parties::Role role) { menu_user = id; menu_role = role; };
    Rml::Dictionary right_click;
    right_click["button"] = 1;
    if (auto* row = document->GetElementById("user-44")) row->DispatchEvent("mousedown", right_click);
    passed &= check(menu_user == 44 && menu_role == parties::Role::Admin, "user menu did not receive the admin role");

    int role_target = 0;
    std::optional<parties::Role> requested;
    model.menu_user_id = 55;
    model.on_set_user_role = [&](int id, parties::Role role) { role_target = id; requested = role; };
    document->GetElementById("make-bogus")->DispatchEvent("click", {});
    passed &= check(!requested, "out-of-table role index reached the role callback");
    document->GetElementById("make-mod")->DispatchEvent("click", {});
    passed &= check(role_target == 55 && requested == parties::Role::Moderator, "role index 2 did not request moderator");

    document->Close();
    model.on_show_user_menu = {};
    model.on_set_user_role = {};
    model.channels = Rml::Vector<ChannelInfo>{};
    return passed;
}
}

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

        passed &= check_voice_navigation(context, model);

        model.remove_channel_sharer(22);
        passed &= check(model.sharers.get().empty() && !model.someone_sharing.get() &&
            !model.channels.get()[1].users[0].streaming, "stopped stream remained visible");
        model.clear_channel_sharers();
        model.current_channel = 0;
        passed &= check(!model.add_channel_sharer(11) && !model.add_channel_sharer(22), "share was accepted outside a channel");

        passed &= check_role_bindings(context, model);
    }
    Rml::RemoveContext("channel-streams");
    Rml::Shutdown();
    return passed ? 0 : 1;
}
