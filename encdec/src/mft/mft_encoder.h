#pragma once

#include <encdec/encoder.h>

#include <d3d11.h>
#include <mfapi.h>
#include <mftransform.h>
#include <mfidl.h>
#include <wrl/client.h>

#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>

struct IMFMediaEventGenerator;

namespace parties::encdec::mft {

class MftEncoder final : public Encoder {
public:
    ~MftEncoder() override;

    bool init(ID3D11Device* device, uint32_t width, uint32_t height,
              uint32_t input_width, uint32_t input_height,
              uint32_t fps, uint32_t bitrate, VideoCodecId preferred_codec);

    bool encode(ID3D11Texture2D* bgra_texture, int64_t timestamp_100ns) override;
    void force_keyframe() override;
    void set_bitrate(uint32_t bitrate) override;
    EncoderInfo info() const override;

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

    Microsoft::WRL::ComPtr<IMFTransform> converter_;
    Microsoft::WRL::ComPtr<IMFTransform> encoder_;
    Microsoft::WRL::ComPtr<IMFMediaEventGenerator> encoder_events_;
    bool async_mode_ = false;

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
    bool mf_started_ = false;  // MFStartup succeeded — MFShutdown owed even if init() failed later
    bool force_keyframe_ = false;
};

} // namespace parties::encdec::mft
