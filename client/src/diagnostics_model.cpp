#include <client/diagnostics_model.h>

namespace parties::client {

void DiagnosticsModel::build(rml::Builder& b) {
    b.register_struct<DiagStreamRow>([](auto& s) {
        s.member("name",     &DiagStreamRow::name)
         .member("rates",    &DiagStreamRow::rates)
         .member("counters", &DiagStreamRow::counters)
         .member("freezes",  &DiagStreamRow::freezes)
         .member("stalled",  &DiagStreamRow::stalled);
    });
    b.register_array<Rml::Vector<DiagStreamRow>>();
    b.register_struct<DiagFreezeRow>([](auto& s) {
        s.member("text",  &DiagFreezeRow::text)
         .member("cause", &DiagFreezeRow::cause);
    });
    b.register_array<Rml::Vector<DiagFreezeRow>>();
    b.register_struct<DiagLogRow>([](auto& s) {
        s.member("text",  &DiagLogRow::text)
         .member("level", &DiagLogRow::level);
    });
    b.register_array<Rml::Vector<DiagLogRow>>();

    b.bind("visible",             visible)
     .bind("revision",            revision)
     .bind("primary_stream",      primary_stream)
     .bind("primary_stream_name", primary_stream_name)
     .bind("link_rtt",            link_rtt)
     .bind("link_rates",          link_rates)
     .bind("link_loss",           link_loss)
     .bind("link_server",         link_server)
     .bind("sender",              sender)
     .bind("net_scale",           net_scale)
     .bind("rtt_scale",           rtt_scale)
     .bind("fps_scale",           fps_scale)
     .bind("net_scale_value",     net_scale_value)
     .bind("rtt_scale_value",     rtt_scale_value)
     .bind("fps_scale_value",     fps_scale_value)
     .bind("streams",             streams)
     .bind("freezes",             freezes)
     .bind("logs",                logs);

    b.on("close_diagnostics", [this] {
        if (on_close) on_close();
    });
}

} // namespace parties::client
