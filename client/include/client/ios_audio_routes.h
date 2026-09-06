#pragma once

#include <string>
#include <vector>

namespace parties::client {

struct IOSAudioPort {
    std::string uid;
    std::string name;
    bool paired_output = false;
    bool speaker = false;
};

enum class IOSAudioRouteKind { System, Speaker, Input, CurrentOutput };

struct IOSAudioRouteChoice {
    std::string name;
    std::string uid;
    IOSAudioRouteKind kind;
};

struct IOSAudioRoutes {
    std::vector<IOSAudioRouteChoice> inputs;
    std::vector<IOSAudioRouteChoice> outputs;
    int selected_input = 0;
    int selected_output = 0;
};

// iOS exposes available inputs, but only the currently routed outputs.
// Selecting a headset/HFP input also selects its paired output.
inline IOSAudioRoutes BuildIOSAudioRoutes(const std::vector<IOSAudioPort>& available_inputs,
    const IOSAudioPort& current_input, const IOSAudioPort& current_output)
{
    IOSAudioRoutes result;
    result.inputs.push_back({"System default", "", IOSAudioRouteKind::System});
    result.outputs.push_back({"System default", "", IOSAudioRouteKind::System});
    result.outputs.push_back({"iPhone Speaker", "", IOSAudioRouteKind::Speaker});
    if (current_output.speaker) result.selected_output = 1;
    for (const auto& port : available_inputs) {
        if (port.uid.empty()) continue;
        bool duplicate = false;
        for (const auto& choice : result.inputs) duplicate |= choice.uid == port.uid;
        if (duplicate) continue;
        result.inputs.push_back({port.name, port.uid, IOSAudioRouteKind::Input});
        if (port.uid == current_input.uid) result.selected_input = static_cast<int>(result.inputs.size()) - 1;
        if (port.paired_output) {
            result.outputs.push_back({port.name, port.uid, IOSAudioRouteKind::Input});
            if (port.uid == current_input.uid && current_output.paired_output)
                result.selected_output = static_cast<int>(result.outputs.size()) - 1;
        }
    }
    if (!current_output.uid.empty() && result.selected_output == 0) {
        result.outputs.push_back({current_output.name, current_output.uid, IOSAudioRouteKind::CurrentOutput});
        result.selected_output = static_cast<int>(result.outputs.size()) - 1;
    }
    return result;
}

} // namespace parties::client
