#pragma once

#include <client/app_core.h>
#include <client/ui_manager.h>
#include <client/screen_capture.h>
#include <client/sound_player.h>
#include <client/stream_audio_capture.h>
#include <parties/types.h>
#include <parties/video_common.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

namespace parties::encdec { struct DecodedFrame; }

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

typedef struct HWND__* HWND;

namespace parties::client {

class VideoEncoder;
class VideoDecoder;
class VideoElement;
class VideoElementInstancer;
class LevelMeterElement;
class LevelMeterInstancer;

class App {
public:
    App();
    ~App();

    // renderer_id: 0=DX12, 1=DX11, 2=DX12WL, 3=Vulkan
    bool init(HWND hwnd, int renderer_id = 0);
    void shutdown();

    // Main loop tick (called each frame)
    void update();

    // Lightweight tick for global hotkeys (PTT, mute, deafen).
    // Called even when minimized.
    void poll_hotkeys();

    // Public accessor for WndProc
    UiManager* ui_manager() { return &ui_; }

private:
    // ── Shared logic (platform-independent) ──────────────────────────────
    AppCore core_;

    // ── Windows-specific platform plumbing ───────────────────────────────
    void show_share_picker();
    void start_screen_share(int target_index);
    void stop_screen_share();

    void start_decode_thread();
    void stop_decode_thread();
    void decode_loop();
    void on_video_decoded(const encdec::DecodedFrame& frame);
    void encode_loop();

    void update_voice_level();

    HWND hwnd_ = nullptr;
    SoundPlayer sound_player_;
    UiManager ui_;

    // PTT release delay
    std::chrono::steady_clock::time_point ptt_release_time_{};
    bool ptt_held_ = false;

    // Hotkey edge detection (trigger on press, not hold)
    bool mute_key_held_   = false;
    bool deafen_key_held_ = false;

    // Screen sharing state
    std::vector<CaptureTarget> capture_targets_;
    std::unique_ptr<ScreenCapture> capture_;
    std::unique_ptr<VideoEncoder> encoder_;
    std::unique_ptr<VideoDecoder> decoder_;
    std::unique_ptr<VideoElementInstancer> video_instancer_;
    std::unique_ptr<LevelMeterInstancer>   level_meter_instancer_;
    LevelMeterElement* level_meter_ = nullptr;  // owned by RmlUi document
    std::atomic<bool> capture_lost_{false};

    // Capture frame rate limiting (QPC-based)
    int64_t qpc_frequency_       = 0;
    int64_t capture_start_qpc_   = 0;
    int64_t last_capture_qpc_    = 0;
    int64_t capture_interval_qpc_ = 0;

    // Encode thread with triple-buffered staging textures
    static constexpr int ENCODE_SLOTS = 3;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> encode_textures_[ENCODE_SLOTS];
    int encode_nvenc_slots_[ENCODE_SLOTS]{-1, -1, -1};
    uint32_t encode_tex_w_ = 0, encode_tex_h_ = 0;
    bool encode_registered_ = false;

    std::thread encode_thread_;
    std::atomic<bool> encode_running_{false};
    std::mutex encode_mutex_;
    std::condition_variable encode_cv_;

    int   encode_write_slot_  = 0;
    int   encode_ready_slot_  = -1;
    int   encode_active_slot_ = -1;
    int64_t encode_ready_ts_  = 0;

    uint32_t encode_fps_ = 60;
    std::function<void(const uint8_t*, size_t, bool)> encode_on_encoded_;

    // Stream audio (capture for sharer, playback for viewer)
    std::unique_ptr<StreamAudioCapture> stream_audio_capture_;

    // Video decode thread
    struct DecodeWork {
        std::vector<uint8_t> data;
        int64_t     timestamp;
        VideoCodecId codec;
        uint16_t    width;
        uint16_t    height;
    };
    std::thread decode_thread_;
    std::atomic<bool> decode_running_{false};
    std::mutex decode_queue_mutex_;
    std::condition_variable decode_queue_cv_;
    std::queue<DecodeWork> decode_queue_;

    // Latest decoded frame — YUV/NV12 planes
    std::mutex frame_mutex_;
    std::atomic<bool> new_frame_available_{false};
    std::vector<uint8_t> shared_y_, shared_u_, shared_v_;
    std::vector<uint8_t> staging_y_, staging_u_, staging_v_;
    uint32_t shared_width_ = 0, shared_height_ = 0;
    uint32_t shared_y_stride_ = 0, shared_uv_stride_ = 0;
    bool shared_nv12_ = false;

    // FPS counters (render + stream)
    uint32_t fps_frame_count_ = 0;
    std::chrono::steady_clock::time_point fps_last_update_{std::chrono::steady_clock::now()};

    // UI document
    Rml::ElementDocument* doc_ = nullptr;
};

} // namespace parties::client
