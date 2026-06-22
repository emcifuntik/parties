#pragma once

#include <client/app_core.h>
#include <client/ui_manager.h>
#include <client/screen_capture.h>
#include <client/sound_player.h>
#include <client/gradient_circle_element.h>
#include <client/rml_elements.h>
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
#include <unordered_map>
#include <vector>

typedef struct HWND__* HWND;

namespace parties::client {

class VideoEncoder;
class VideoDecoder;
class VideoElement;
class LevelMeterElement;

class App {
public:
    App();
    ~App();

    // renderer_id: 0=DX12, 1=DX11, 2=DX12WL, 3=Vulkan
    bool init(HWND hwnd, int renderer_id = 0);
    void shutdown();

    // Per-iteration logic tick on the message thread (network, hotkeys,
    // fullscreen sync). Rendering runs on a dedicated render thread (render_loop)
    // so the picture keeps updating even while the OS modal move/resize loop
    // parks the message thread.
    void tick_message_thread();

    // Lightweight tick for global hotkeys (PTT, mute, deafen). Called from
    // tick_message_thread under ui_mutex_.
    void poll_hotkeys();

    // Defer a window resize / DPI change to the render thread, which owns the
    // GPU swap chain and the RmlUi context dimensions.
    void defer_resize(int width, int height);
    void defer_dpi(float scale);

    // Guards all RmlUi context + data-model access (render thread vs. the
    // message thread's input/tick). Recursive so a modal handler that re-enters
    // WndProc while already holding it (e.g. a native dialog) won't self-deadlock.
    std::recursive_mutex& ui_mutex() { return ui_mutex_; }

    // Public accessor for WndProc
    UiManager* ui_manager() { return &ui_; }

    // Chat text selection (drag-select + Ctrl+C copy). Called from WndProc with
    // ui_mutex_ held. Returns true only when the event is fully consumed (Ctrl+C).
    bool handle_chat_input(unsigned int msg, WPARAM wParam, LPARAM lParam);

private:
    // ── Shared logic (platform-independent) ──────────────────────────────
    AppCore core_;

    struct VideoStream;   // per-sharer decode pipeline (defined below)

    // ── Windows-specific platform plumbing ───────────────────────────────
    void show_share_picker();
    void start_screen_share(int target_index);
    void stop_screen_share();
    void on_video_frame_received(uint32_t sender_id, const uint8_t* data, size_t len);

    // Per-sharer video stream lifecycle (multiple streams play at once in a grid).
    void start_video_stream(UserId sharer_id);
    void stop_video_stream(UserId sharer_id);
    void stop_all_video_streams();
    void stop_stream_thread(VideoStream* s);   // signal running=false (under queue_mutex) + join
    void decode_loop(VideoStream* s);
    void on_video_decoded(VideoStream* s, const encdec::DecodedFrame& frame);
    void enqueue_decode_work(UserId sharer_id, std::vector<uint8_t>&& encoded,
                             int64_t timestamp, VideoCodecId codec,
                             uint16_t width, uint16_t height, bool is_keyframe);
    void encode_loop();

    void update_voice_level();

    // Render thread: owns the GPU swap chain + per-frame rendering and decoded-
    // video delivery, paced by vsync. Independent of the Win32 message loop.
    void render_loop();
    void render_frame();

    HWND hwnd_ = nullptr;
    SoundPlayer sound_player_;
    UiManager ui_;

    // ── Render thread + UI synchronization ───────────────────────────────
    std::recursive_mutex ui_mutex_;            // guards RmlUi context + data model

    // Chat drag-select tracking (message thread, under ui_mutex_).
    POINT chat_drag_start_{};
    bool  chat_drag_moved_ = false;
    std::thread          render_thread_;
    std::atomic<bool>    render_running_{false};
    std::atomic<bool>    resize_pending_{false};
    std::atomic<int>     pending_w_{0};
    std::atomic<int>     pending_h_{0};
    std::atomic<bool>    dpi_pending_{false};
    std::atomic<float>   pending_dpi_{1.0f};

    // PTT release delay
    std::chrono::steady_clock::time_point ptt_release_time_{};
    bool ptt_held_ = false;

    // Hotkey edge detection (trigger on press, not hold)
    bool mute_key_held_   = false;
    bool deafen_key_held_ = false;

    // Keybind capture — accumulates peak simultaneous keys, finalizes on release
    int  capture_peak_key_  = 0;
    int  capture_peak_key2_ = 0;
    int  capture_peak_mods_ = 0;
    bool capture_had_input_ = false;

    // Screen sharing state
    std::vector<CaptureTarget> capture_targets_;
    std::unique_ptr<ScreenCapture> capture_;
    std::unique_ptr<VideoEncoder> encoder_;
    std::unique_ptr<VideoDecoder> decoder_;
    parties::rml::ElementRegistry element_registry_;
    LevelMeterElement* level_meter_ = nullptr;  // owned by RmlUi document
    bool sharing_screen_ = false;
    bool stream_revealed_ = false;  // first decoded frame shown to UI
    std::atomic<bool> capture_lost_{false};

    // Capture frame rate limiting (QPC-based)
    int64_t qpc_frequency_       = 0;
    int64_t capture_start_qpc_   = 0;
    int64_t last_capture_qpc_    = 0;
    int64_t capture_interval_qpc_ = 0;

    // GPU downscale resources (bilinear blit via pixel shader)
    Microsoft::WRL::ComPtr<ID3D11PixelShader>      scale_ps_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader>     scale_vs_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState>     scale_sampler_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>        scale_src_tex_;    // full-res capture copy
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> scale_src_srv_;
    uint32_t scale_src_w_ = 0, scale_src_h_ = 0;
    bool scale_pipeline_ready_ = false;
    void init_scale_pipeline(ID3D11Device* device);

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

    // ── Video decode (multiple simultaneous sharer streams, tiled in a grid) ──
    struct DecodeWork {
        std::vector<uint8_t> data;
        int64_t      timestamp;
        VideoCodecId codec;
        uint16_t     width;
        uint16_t     height;
    };

    // One watched sharer's decode pipeline: its own hardware decoder + thread +
    // jitter queue + latest-decoded-frame buffers. Created when a stream is added
    // to the grid, destroyed when removed. The QUIC receive thread feeds `queue`;
    // the decode thread produces into the frame buffers; render_frame drains them
    // onto this stream's <video_frame> grid cell (element_id).
    struct VideoStream {
        UserId       sharer_id = 0;
        std::string  element_id;          // "screen-share-<id>"
        std::unique_ptr<VideoDecoder> decoder;
        std::thread  thread;
        std::atomic<bool> running{false};
        std::atomic<bool> awaiting_keyframe{true};

        std::mutex queue_mutex;
        std::condition_variable queue_cv;
        std::queue<DecodeWork> queue;

        std::mutex frame_mutex;
        std::atomic<bool> new_frame{false};
        std::vector<uint8_t> y, u, v;                          // latest decoded planes
        std::vector<uint8_t> staging_y, staging_u, staging_v;  // decode-thread scratch
        uint32_t width = 0, height = 0, y_stride = 0, uv_stride = 0;
        bool nv12 = false;
    };

    // Guards the map structure AND each per-stream queue push, so a stream can't
    // be destroyed out from under the QUIC receive thread mid-enqueue.
    std::mutex streams_mutex_;
    std::unordered_map<UserId, std::unique_ptr<VideoStream>> video_streams_;

    // FPS counters (render + stream)
    uint32_t fps_frame_count_ = 0;
    std::chrono::steady_clock::time_point fps_last_update_{std::chrono::steady_clock::now()};
    // Last titlebar strings — SetInnerRML rebuilds the text element (and its
    // render geometry) even for identical content, so skip unchanged updates.
    Rml::String titlebar_fps_last_;
    Rml::String titlebar_ping_last_;

    // UI document
    Rml::ElementDocument* doc_ = nullptr;
};

} // namespace parties::client
