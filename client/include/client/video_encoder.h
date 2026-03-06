#pragma once

#include <parties/video_common.h>

#include <cstdint>
#include <functional>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>

#include <d3d11.h>
#include <wrl/client.h>

struct IMFTransform;
struct IMFDXGIDeviceManager;
struct IMFMediaEventGenerator;
struct IMFSample;

namespace parties::client {

class VideoEncoder {
public:
    VideoEncoder();
    ~VideoEncoder();

    // Initialize with a D3D11 device (shared with screen capture)
    // Probes for hardware encoders: AV1 > H.265 > H.264 (or preferred codec first)
    // input_width/height = capture texture size, width/height = encode output size
    bool init(ID3D11Device* device, uint32_t width, uint32_t height,
              uint32_t input_width = 0, uint32_t input_height = 0,
              uint32_t fps = 30, uint32_t bitrate = VIDEO_DEFAULT_BITRATE,
              VideoCodecId preferred_codec = VideoCodecId::AV1);
    void shutdown();

    // Encode a BGRA texture. Calls on_encoded when output is ready.
    bool encode_frame(ID3D11Texture2D* bgra_texture, int64_t timestamp_100ns);

    // Force next frame to be a keyframe
    void force_keyframe();

    // Dynamically change bitrate
    void set_bitrate(uint32_t bitrate);

    // Which codec was selected?
    VideoCodecId codec() const { return codec_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

    // Callback with encoded bitstream data
    std::function<void(const uint8_t* data, size_t len, bool keyframe)> on_encoded;

private:
    bool try_create_encoder(const GUID& codec_subtype, VideoCodecId id);
    bool configure_encoder(uint32_t width, uint32_t height,
                           uint32_t fps, uint32_t bitrate);
    bool create_color_converter(uint32_t in_w, uint32_t in_h,
                                uint32_t out_w, uint32_t out_h);
    bool collect_output();
    void encoder_loop();

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> dxgi_manager_;
    UINT dxgi_reset_token_ = 0;

    // Color converter: BGRA → NV12 (GPU)
    Microsoft::WRL::ComPtr<IMFTransform> converter_;

    // Hardware encoder
    Microsoft::WRL::ComPtr<IMFTransform> encoder_;
    Microsoft::WRL::ComPtr<IMFMediaEventGenerator> encoder_events_;
    bool async_mode_ = false;

    // Encoder thread (for async MFTs)
    std::thread encoder_thread_;
    std::atomic<bool> encoder_running_{false};
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::queue<Microsoft::WRL::ComPtr<IMFSample>> input_queue_;

    VideoCodecId codec_ = VideoCodecId::H264;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t fps_ = 30;
    bool initialized_ = false;
    bool force_keyframe_ = false;
};

} // namespace parties::client
