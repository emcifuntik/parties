#include <client/ios_audio_routes.h>
#include <cstdio>

using namespace parties::client;

int main()
{
    int failures = 0;
    const auto check = [&](bool valid, const char* message) {
        if (!valid) { std::fprintf(stderr, "iOS audio routes: %s\n", message); ++failures; }
    };
    const IOSAudioPort mic{"builtin-mic", "iPhone Microphone"};
    const IOSAudioPort speaker{"builtin-speaker", "Speaker", false, true};
    const IOSAudioPort airpods{"airpods-hfp", "AirPods Pro", true};
    auto routes = BuildIOSAudioRoutes({mic}, mic, speaker);
    check(routes.inputs.size() == 2 && routes.outputs.size() == 2, "built-in routes and system defaults");
    check(routes.selected_input == 1 && routes.selected_output == 1, "actual built-in selection");

    routes = BuildIOSAudioRoutes({mic, airpods}, mic, speaker);
    check(routes.inputs.size() == 3 && routes.outputs.size() == 3, "inactive AirPods visible for both directions");
    check(routes.outputs[2].uid == airpods.uid && routes.outputs[2].kind == IOSAudioRouteKind::Input,
        "headset selection uses the current available input UID");

    routes = BuildIOSAudioRoutes({mic, airpods}, airpods, {"airpods-output", "AirPods Pro", true});
    check(routes.selected_input == 2 && routes.selected_output == 2, "HFP input and output selected together");
    check(routes.outputs.size() == 3, "paired output is not duplicated");

    routes = BuildIOSAudioRoutes({mic}, mic, {"airpods-a2dp", "AirPods Pro"});
    check(routes.outputs[routes.selected_output].name == "AirPods Pro", "active stereo route is named");
    check(routes.outputs[routes.selected_output].kind == IOSAudioRouteKind::CurrentOutput,
        "output-only routes are not mistaken for selectable input ports");

    routes = BuildIOSAudioRoutes({mic}, mic, speaker);
    check(routes.inputs.size() == 2 && routes.outputs.size() == 2 && routes.selected_output == 1,
        "disconnect removes stale headset choices and restores actual selection");
    routes = BuildIOSAudioRoutes({mic, airpods, airpods, {"usb-mic", "USB microphone"}}, mic, speaker);
    check(routes.inputs.size() == 4 && routes.outputs.size() == 3,
        "duplicate UIDs are removed and an input-only USB mic is not advertised as an output");
    routes = BuildIOSAudioRoutes({}, {}, {});
    check(routes.selected_input == 0 && routes.selected_output == 0, "empty session has valid defaults");

    routes = BuildIOSAudioRoutes({mic, airpods}, mic, speaker);
    check(ios_route_already_active(routes, routes.selected_input, IOSAudioRouteDirection::Input),
        "republished input selection is an echo, not a route change");
    check(ios_route_already_active(routes, routes.selected_output, IOSAudioRouteDirection::Output),
        "republished output selection is an echo, not a route change");
    check(!ios_route_already_active(routes, 2, IOSAudioRouteDirection::Input),
        "picking the inactive AirPods input changes the route");
    check(!ios_route_already_active(routes, 0, IOSAudioRouteDirection::Output),
        "picking the system default output changes the route");
    check(!ios_route_already_active(routes, 3, IOSAudioRouteDirection::Output)
            && !ios_route_already_active(routes, -1, IOSAudioRouteDirection::Input),
        "out-of-range picks are not treated as active");

    routes = BuildIOSAudioRoutes({mic}, mic, {"airpods-a2dp", "AirPods Pro"});
    const int current_output = routes.selected_output;
    check(ios_route_already_active(routes, current_output, IOSAudioRouteDirection::Output),
        "the routed output-only device is already active");
    routes.selected_output = 0;
    check(ios_route_already_active(routes, current_output, IOSAudioRouteDirection::Output),
        "an output-only route cannot be selected, only observed");
    return failures ? 1 : 0;
}
