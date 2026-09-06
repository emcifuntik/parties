#pragma once

#include <miniaudio.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace parties::client {

inline ma_context_config MakeAudioContextConfig()
{
    auto config = ma_context_config_init();
#if defined(__APPLE__) && TARGET_OS_IPHONE
    // UIKit owns the shared session. Notification playback and voice must not
    // reset Bluetooth routing or deactivate each other's audio session.
    config.coreaudio.sessionCategory = ma_ios_session_category_none;
    config.coreaudio.noAudioSessionActivate = MA_TRUE;
    config.coreaudio.noAudioSessionDeactivate = MA_TRUE;
#endif
    return config;
}

} // namespace parties::client
