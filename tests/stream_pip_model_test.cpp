// Shared picture-in-picture model: StreamPipController transitions as AppCore
// drives them, the grid placeholder binding, and VideoFrameRouter's
// one-destination routing with texture retirement on the owning renderer.

#include <client/lobby_model.h>
#include <client/stream_pip.h>
#include <client/video_element.h>
#include <client/video_frame_router.h>

#include "RmlUi_RenderInterface_Extended.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/ElementInstancer.h>

#ifdef _WIN32
#include <crtdbg.h>
#endif

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

using namespace parties::client;
using parties::UserId;

// Counts video texture traffic so the test can tell which renderer allocated
// and released a stream's planes.
class CountingRenderer final : public ExtendedRenderInterface {
public:
    int nv12_generated = 0;
    int nv12_released = 0;
    int nv12_live = 0;

    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex>, Rml::Span<const int>) override { return ++next_; }
    void RenderGeometry(Rml::CompiledGeometryHandle, Rml::Vector2f, Rml::TextureHandle) override {}
    void ReleaseGeometry(Rml::CompiledGeometryHandle) override {}
    Rml::TextureHandle LoadTexture(Rml::Vector2i&, const Rml::String&) override { return {}; }
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte>, Rml::Vector2i) override { return ++next_; }
    void ReleaseTexture(Rml::TextureHandle) override {}
    void EnableScissorRegion(bool) override {}
    void SetScissorRegion(Rml::Rectanglei) override {}

    uintptr_t GenerateYUVTexture(const uint8_t*, uint32_t, const uint8_t*, const uint8_t*, uint32_t,
                                 uint32_t, uint32_t) override { return ++next_; }
    void UpdateYUVTexture(uintptr_t, const uint8_t*, uint32_t, const uint8_t*, const uint8_t*, uint32_t,
                          uint32_t, uint32_t) override {}
    void ReleaseYUVTexture(uintptr_t) override {}
    void RenderYUVGeometry(Rml::CompiledGeometryHandle, Rml::Vector2f, uintptr_t) override {}

    uintptr_t GenerateNV12Texture(const uint8_t*, uint32_t, const uint8_t*, uint32_t,
                                  uint32_t, uint32_t) override {
        ++nv12_generated;
        ++nv12_live;
        return ++next_;
    }
    void UpdateNV12Texture(uintptr_t, const uint8_t*, uint32_t, const uint8_t*, uint32_t,
                           uint32_t, uint32_t) override {}
    void ReleaseNV12Texture(uintptr_t) override {
        ++nv12_released;
        --nv12_live;
    }
    void RenderNV12Geometry(Rml::CompiledGeometryHandle, Rml::Vector2f, uintptr_t) override {}

private:
    uintptr_t next_ = 0;
};

// Print RmlUi warnings and assertions; a failed assertion fails the test
// instead of waiting on a debug-runtime dialog.
class StderrSystemInterface final : public Rml::SystemInterface {
public:
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override {
        if (type < Rml::Log::LT_WARNING)
            std::fprintf(stderr, "RmlUi: %s\n", message.c_str());
        if (type == Rml::Log::LT_ASSERT) std::_Exit(3);
        return true;
    }
};

bool passed = true;

const char* last_check = "start";

void check(bool condition, const char* message) {
    last_check = message;
    if (!condition) {
        std::fprintf(stderr, "Stream PiP: %s\n", message);
        passed = false;
    }
}

struct PresenterLog {
    std::vector<std::string> events;

    StreamPipController::Presenter presenter() {
        return {
            [this](UserId id, const std::string& title) {
                events.push_back("show " + std::to_string(id) + " " + title);
            },
            [this](UserId id, PipCloseReason reason) {
                const char* why = reason == PipCloseReason::UserClosed ? "user"
                    : reason == PipCloseReason::ReturnedToMain ? "return" : "unwatched";
                events.push_back("hide " + std::to_string(id) + " " + why);
            },
            [this](UserId id) { events.push_back("moved " + std::to_string(id)); },
        };
    }

    bool take(std::vector<std::string> expected) {
        const bool same = events == expected;
        if (!same) {
            std::fprintf(stderr, "  expected:");
            for (const auto& e : expected) std::fprintf(stderr, " [%s]", e.c_str());
            std::fprintf(stderr, "\n  actual:  ");
            for (const auto& e : events) std::fprintf(stderr, " [%s]", e.c_str());
            std::fprintf(stderr, "\n");
        }
        events.clear();
        return same;
    }
};

void populate_channel(LobbyModel& model) {
    ChannelUser bob; bob.id = 22; bob.name = "Bob";
    ChannelUser carol; carol.id = 33; carol.name = "Carol";
    ChannelInfo channel; channel.id = 1; channel.users = {bob, carol};
    model.channels = Rml::Vector<ChannelInfo>{channel};
    model.current_channel = 1;
    model.add_channel_sharer(22);
    model.add_channel_sharer(33);
}

// Mirrors AppCore::rebuild_watched_model for the grid binding.
void publish_watched(LobbyModel& model, const std::unordered_set<UserId>& watched) {
    auto& cells = model.watched.silent();
    cells.clear();
    for (const auto& sharer : model.sharers.get()) {
        if (watched.count(static_cast<UserId>(sharer.id)) == 0) continue;
        cells.push_back({sharer.id, sharer.name, "screen-share-" + std::to_string(sharer.id)});
    }
    model.watched.notify();
    model.watching_count = static_cast<int>(cells.size());
}

void check_transitions(Rml::Context* context) {
    LobbyModel model;
    if (!model.init(context)) {
        check(false, "lobby model did not initialise");
        return;
    }
    populate_channel(model);

    StreamPipController pip;
    PresenterLog log;
    pip.attach(&model, log.presenter());
    std::unordered_set<UserId> watched{22, 33};
    publish_watched(model, watched);
    // AppCore wires the RML intent exactly like this (AppCore::toggle_pip).
    model.on_toggle_stream_pip = [&](int id) { pip.toggle(static_cast<UserId>(id), watched); };

    auto* document = context->LoadDocumentFromMemory(R"RML(
<rml><body data-model="lobby">
<div id="stream-grid">
    <div class="stream-cell" data-for="w : watched">
        <video_frame data-if="w.id != pip_stream_id" data-attr-id="'video-' + w.id"
                     data-attr-streamid="w.id" style="display: block; width: 160px; height: 90px;" />
        <div data-if="w.id == pip_stream_id" data-attr-id="'placeholder-' + w.id">
            <span class="ui-body">Playing in picture-in-picture</span>
        </div>
        <button data-attr-id="'pip-' + w.id" data-event-click="toggle_stream_pip(w.id)">PiP</button>
    </div>
</div>
</body></rml>)RML");
    if (!document) {
        check(false, "placeholder document did not load");
        return;
    }
    document->Show();
    context->Update();
    auto visible = [&](const char* id) {
        Rml::Element* element = document->GetElementById(id);
        return element && element->IsVisible(true);
    };
    auto click = [&](const char* id) {
        if (Rml::Element* element = document->GetElementById(id)) element->DispatchEvent("click", {});
        context->Update();
    };

    check(!pip.is_open() && model.pip_stream_id.get() == 0, "PiP did not start closed");
    check(visible("video-22") && visible("video-33") && !visible("placeholder-22"),
        "grid did not show both videos without PiP");

    // Open from the stream controls.
    click("pip-22");
    check(pip.stream() == 22 && model.pip_stream_id.get() == 22, "open did not select the stream");
    check(log.take({"show 22 Bob", "moved 22"}), "open presented the wrong transition");
    check(!visible("video-22") && visible("placeholder-22") && visible("video-33"),
        "PiP stream's cell did not switch to the placeholder");

    // Picking PiP on another stream switches it.
    click("pip-33");
    check(pip.stream() == 33 && model.pip_stream_id.get() == 33, "switch did not move PiP");
    check(log.take({"show 33 Carol", "moved 22", "moved 33"}), "switch presented the wrong transition");
    check(visible("video-22") && !visible("placeholder-22") && visible("placeholder-33"),
        "switch did not return the previous stream to its cell");

    // Only watched streams can be shown.
    check(!pip.open(44, watched) && pip.stream() == 33 && log.take({}), "an unwatched stream opened PiP");

    // The PiP button on the PiP stream closes it; the stream stays in the grid.
    click("pip-33");
    check(!pip.is_open() && model.pip_stream_id.get() == 0, "close left PiP open");
    check(log.take({"hide 33 user", "moved 33"}), "close presented the wrong transition");
    check(visible("video-33") && !visible("placeholder-33"), "close did not restore the cell");
    pip.close(PipCloseReason::UserClosed);
    check(log.take({}), "closing a closed PiP notified the platform");

    // Return action: closes PiP with its own reason.
    pip.open(22, watched);
    log.take({"show 22 Bob", "moved 22"});
    pip.close(PipCloseReason::ReturnedToMain);
    check(!pip.is_open() && log.take({"hide 22 return", "moved 22"}), "return did not close PiP");

    // The stream ends: AppCore::remove_watch -> rebuild_watched_model.
    pip.open(22, watched);
    log.take({"show 22 Bob", "moved 22"});
    watched.erase(22);
    model.remove_channel_sharer(22);
    publish_watched(model, watched);
    pip.reconcile(watched);
    context->Update();
    check(!pip.is_open() && model.pip_stream_id.get() == 0, "stream end left PiP open");
    check(log.take({"hide 22 unwatched"}), "stream end presented the wrong transition");
    check(document->GetElementById("placeholder-22") == nullptr || !visible("placeholder-22"),
        "ended stream still shows a placeholder");

    // Another stream ending leaves PiP alone.
    model.add_channel_sharer(22);
    watched.insert(22);
    publish_watched(model, watched);
    pip.open(33, watched);
    log.take({"show 33 Carol", "moved 33"});
    watched.erase(22);
    pip.reconcile(watched);
    check(pip.stream() == 33 && log.take({}), "an unrelated stream end closed PiP");

    // Stop watching: AppCore::stop_watching empties the set.
    watched.clear();
    publish_watched(model, watched);
    pip.reconcile(watched);
    check(!pip.is_open() && log.take({"hide 33 unwatched"}), "stop watching left PiP open");

    // Leave channel: AppCore::leave_channel -> stop_watching + clear_all_sharers,
    // which reconciles against an empty set even if nothing was rebuilt.
    watched = {22, 33};
    publish_watched(model, watched);
    pip.open(22, watched);
    log.take({"show 22 Bob", "moved 22"});
    model.clear_channel_sharers();
    pip.reconcile({});
    check(!pip.is_open() && model.pip_stream_id.get() == 0 && log.take({"hide 22 unwatched"}),
        "leaving the channel left PiP open");

    // The volume handler must apply the slider's new value. RmlUi runs the
    // change handler before data-value commits it, so reading the model there
    // would apply (and persist) the previous value; unmuting relies on this.
    float applied_volume = -1.0f;
    model.stream_volume = 1.0f;
    model.on_stream_volume_changed = [&](float volume) { applied_volume = volume; };
    auto* controls = context->LoadDocumentFromMemory(R"RML(
<rml><body data-model="lobby">
<input type="range" id="volume" min="0" max="2" step="0.01" data-value="stream_volume"
       data-event-change="stream_volume_changed" style="display: block; width: 100px; height: 16px;" />
</body></rml>)RML");
    if (controls) {
        controls->Show();
        context->Update();
        controls->GetElementById("volume")->SetAttribute("value", 1.5f);
        context->Update();
        check(applied_volume > 1.49f && applied_volume < 1.51f && model.stream_volume.get() > 1.49f,
            "stream volume change applied the previous value");
        controls->Close();
    } else {
        check(false, "volume document did not load");
    }

    document->Close();
    model.on_toggle_stream_pip = {};
    model.on_stream_volume_changed = {};
    // Release the closed documents before the model's data bindings go away.
    context->Update();
}

void feed_frame(VideoElement* element) {
    static std::vector<uint8_t> y(16 * 16, 128);
    static std::vector<uint8_t> uv(16 * 8, 128);
    if (element) element->UpdateNV12Frame(y.data(), 16, uv.data(), 16, 16, 16);
}

// RmlUi keeps a render manager per render interface until Rml::Shutdown, so
// both renderers outlive the library, as the platform PiP hosts must.
void check_routing(Rml::Context* main_context, CountingRenderer& main_renderer,
                   CountingRenderer& pip_renderer) {
    Rml::Context* pip_context = Rml::CreateContext("stream-pip-surface", {320, 180}, &pip_renderer);
    if (!pip_context) {
        check(false, "PiP context was not created");
        return;
    }
    VideoElement::RegisterContextRenderInterface(pip_context, &pip_renderer);

    auto* grid_document = main_context->LoadDocumentFromMemory(R"RML(
<rml><body>
<div id="stream-grid">
    <video_frame streamid="22" style="display: block; width: 160px; height: 90px;" />
    <video_frame streamid="33" style="display: block; width: 160px; height: 90px;" />
</div>
</body></rml>)RML");
    auto* pip_document = pip_context->LoadDocumentFromMemory(R"RML(
<rml><body><video_frame id="pip-video" style="display: block; width: 320px; height: 180px;" /></body></rml>)RML");
    if (!grid_document || !pip_document) {
        check(false, "routing documents did not load");
        return;
    }
    grid_document->Show();
    pip_document->Show();

    VideoFrameRouter router;
    VideoFrameRouter::Surfaces surfaces;
    surfaces.grid = grid_document->GetElementById("stream-grid");
    surfaces.pip = rmlui_dynamic_cast<VideoElement*>(pip_document->GetElementById("pip-video"));
    VideoElement* cell22 = find_stream_grid_video(surfaces.grid, 22);
    VideoElement* cell33 = find_stream_grid_video(surfaces.grid, 33);
    check(cell22 && cell33 && cell22 != cell33 && surfaces.pip, "grid lookup failed");
    check(find_stream_grid_video(surfaces.grid, 44) == nullptr, "grid lookup found an unwatched stream");

    auto render = [&] {
        main_context->Update();
        main_context->Render();
        pip_context->Update();
        pip_context->Render();
    };

    // No PiP: every stream renders in its own cell on the main renderer.
    check(!router.sync(0, surfaces).grid, "sync without a change cleared a surface");
    check(router.target(22, surfaces) == cell22 && router.target(33, surfaces) == cell33,
        "grid streams were not routed to their cells");
    feed_frame(router.target(22, surfaces));
    feed_frame(router.target(33, surfaces));
    render();
    check(main_renderer.nv12_live == 2 && pip_renderer.nv12_generated == 0,
        "grid frames were not uploaded by the main renderer");

    // Stream 22 moves to PiP: its cell releases on the main renderer, and its
    // frames go only to the PiP surface, uploaded by the PiP renderer.
    const auto moved_in = router.sync(22, surfaces);
    check(moved_in.grid && moved_in.pip, "moving to PiP did not clear both surfaces");
    check(main_renderer.nv12_live == 1 && cell22->frame_width() == 0,
        "the cell's texture was not retired on the main renderer");
    check(router.target(22, surfaces) == surfaces.pip && router.target(33, surfaces) == cell33,
        "PiP stream was not routed to exactly one surface");
    feed_frame(router.target(22, surfaces));
    feed_frame(router.target(33, surfaces));
    render();
    check(pip_renderer.nv12_live == 1 && main_renderer.nv12_live == 1 && cell22->frame_width() == 0,
        "a PiP frame reached the grid or the wrong renderer");

    // Switch to 33: the PiP surface drops 22's frame on its own renderer.
    router.sync(33, surfaces);
    check(pip_renderer.nv12_live == 0 && main_renderer.nv12_live == 0,
        "switching did not retire both streams' previous surfaces");
    check(router.target(22, surfaces) == cell22 && router.target(33, surfaces) == surfaces.pip,
        "switch did not swap destinations");

    // Close: the PiP surface releases, the stream renders in its cell again.
    feed_frame(router.target(33, surfaces));
    render();
    router.sync(0, surfaces);
    check(pip_renderer.nv12_live == 0 && pip_renderer.nv12_released == pip_renderer.nv12_generated,
        "closing PiP leaked a texture on the PiP renderer");
    check(router.target(33, surfaces) == cell33, "close did not return the stream to the grid");

    grid_document->Close();
    pip_document->Close();
    main_context->Update();
    pip_context->Update();
    Rml::RemoveContext("stream-pip-surface");
    VideoElement::UnregisterContextRenderInterface(pip_context);
}

// data-for reuses cells by index: when the watched list shifts, the hidden
// <video_frame> of the PiP stream can become an element that still holds
// another stream's texture. The router must clear it while PiP stays open.
void check_cell_reuse(CountingRenderer& main_renderer) {
    Rml::Context* context = Rml::CreateContext("stream-pip-reuse", {640, 480});
    if (!context) {
        check(false, "reuse context was not created");
        return;
    }
    {
        LobbyModel model;
        if (!model.init(context)) {
            check(false, "reuse model did not initialise");
            return;
        }
        populate_channel(model);
        std::unordered_set<UserId> watched{22, 33};
        publish_watched(model, watched);
        auto* document = context->LoadDocumentFromMemory(R"RML(
<rml><body data-model="lobby">
<div id="stream-grid">
    <div data-for="w : watched">
        <video_frame data-if="w.id != pip_stream_id" data-attr-streamid="w.id"
                     style="display: block; width: 160px; height: 90px;" />
    </div>
</div>
</body></rml>)RML");
        if (!document) {
            check(false, "reuse document did not load");
            return;
        }
        document->Show();
        context->Update();

        VideoFrameRouter router;
        VideoFrameRouter::Surfaces surfaces;
        surfaces.grid = document->GetElementById("stream-grid");
        const int live_before = main_renderer.nv12_live;
        feed_frame(router.target(22, surfaces));
        feed_frame(router.target(33, surfaces));
        context->Update();
        context->Render();
        check(main_renderer.nv12_live == live_before + 2, "both grid cells did not upload a frame");

        // 33 enters PiP: its cell is cleared and hidden.
        model.pip_stream_id = 33;
        router.sync(33, surfaces);
        context->Update();
        check(main_renderer.nv12_live == live_before + 1, "the PiP stream's cell kept its texture");

        // 22 stops sharing. The list shifts and the first cell, which still
        // holds 22's texture, is rebound to the hidden PiP stream 33.
        watched.erase(22);
        model.remove_channel_sharer(22);
        publish_watched(model, watched);
        context->Update();
        router.sync(33, surfaces);
        VideoElement* cell33 = find_stream_grid_video(surfaces.grid, 33);
        check(main_renderer.nv12_live == live_before && cell33 && cell33->frame_width() == 0,
            "a reused grid cell kept another stream's frame while hidden for PiP");

        document->Close();
        context->Update();
    }
    Rml::RemoveContext("stream-pip-reuse");
}

} // namespace

int main() {
#ifdef _WIN32
    // Debug-runtime reports go to stderr so CTest fails instead of waiting.
    for (int report : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
        _CrtSetReportMode(report, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(report, _CRTDBG_FILE_STDERR);
    }
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    std::signal(SIGABRT, [](int) {
        std::fprintf(stderr, "Stream PiP: aborted after check \"%s\"\n", last_check);
        std::_Exit(3);
    });
    StderrSystemInterface system;
    Rml::SetSystemInterface(&system);
    CountingRenderer main_renderer;
    CountingRenderer pip_renderer;
    Rml::SetRenderInterface(&main_renderer);
    if (!Rml::Initialise()) return 1;
    Rml::ElementInstancerGeneric<VideoElement> video_instancer;
    Rml::Factory::RegisterElementInstancer("video_frame", &video_instancer);

    auto* context = Rml::CreateContext("stream-pip", {640, 480});
    if (!context) return 2;
    check_transitions(context);
    check_routing(context, main_renderer, pip_renderer);
    check_cell_reuse(main_renderer);
    Rml::RemoveContext("stream-pip");
    Rml::Shutdown();
    return passed ? 0 : 1;
}
