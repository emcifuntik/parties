#pragma once

// Data model of the on-screen diagnostics overlay (data-model="diag" in
// lobby.rml). AppCore fills it from DiagnosticsRecorder a few times a second
// while the overlay is visible; the graphs read the recorder directly
// (DiagnosticsGraphElement), re-drawn when `revision` changes.

#include <client/rml_binding.h>

#include <RmlUi/Core/Types.h>

#include <functional>

namespace parties::client {

namespace rml = parties::rml;

struct DiagStreamRow {
    Rml::String name;
    Rml::String rates;      // "58 fps shown · 60 delivered · 9.8 Mbps"
    Rml::String counters;   // "lost 12 · keyframes 30 · requests 2 · playout 120 ms"
    Rml::String freezes;    // "3 freezes in the last minute, longest 840 ms"
    bool stalled = false;   // nothing shown for 200 ms or more right now
};

struct DiagFreezeRow {
    Rml::String text;       // "17:20:31  Alice  840 ms  network"
    Rml::String cause;      // CSS class suffix: sender / network / lost / decode
};

struct DiagLogRow {
    Rml::String text;
    Rml::String level;      // CSS class suffix: info / warn / error
};

class DiagnosticsModel : public rml::Model {
public:
    rml::Prop<bool>        visible{false};
    rml::Prop<int>         revision{0};
    rml::Prop<int>         primary_stream{0};   // stream drawn in the video graph (0: none)
    rml::Prop<Rml::String> primary_stream_name;
    rml::Prop<Rml::String> link_rtt;
    rml::Prop<Rml::String> link_rates;
    rml::Prop<Rml::String> link_loss;
    rml::Prop<Rml::String> link_server;
    rml::Prop<Rml::String> sender;              // empty while not sharing
    rml::Prop<Rml::String> net_scale;
    rml::Prop<Rml::String> rtt_scale;
    rml::Prop<Rml::String> fps_scale;
    rml::Prop<float>       net_scale_value{100.0f};   // full height of each chart (kbps, ms, fps)
    rml::Prop<float>       rtt_scale_value{50.0f};
    rml::Prop<float>       fps_scale_value{60.0f};
    rml::Prop<Rml::Vector<DiagStreamRow>> streams;
    rml::Prop<Rml::Vector<DiagFreezeRow>> freezes;
    rml::Prop<Rml::Vector<DiagLogRow>>    logs;

    std::function<void()> on_close;

    const char* model_name() const override { return "diag"; }
    void build(rml::Builder& b) override;
};

} // namespace parties::client
