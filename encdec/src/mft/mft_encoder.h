#pragma once

#include <encdec/encoder.h>

#include <d3d11.h>
#include <mfapi.h>
#include <mftransform.h>
#include <mfidl.h>
#include <wrl/client.h>

#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <vector>

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
    // Calls ProcessOutput up to max_outputs times (0 = until it has nothing
    // more). Async MFTs allow one call per METransformHaveOutput event.
    bool collect_output(int max_outputs = 0);
    void encoder_loop();
    // Async mode: feeds queued samples while the MFT has outstanding
    // METransformNeedInput requests. Called with queue_mutex_ held through
    // `lock`; releases it around each ProcessInput.
    void feed_pending_input(std::unique_lock<std::mutex>& lock);
    void submit_input(IMFSample* sample);

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
    // queue_mutex_ guards input_queue_, pending_input_requests_, feeding_
    // and loop_exited_.
    std::mutex queue_mutex_;
    std::queue<Microsoft::WRL::ComPtr<IMFSample>> input_queue_;
    int pending_input_requests_ = 0;   // NeedInput events not yet answered
    bool feeding_ = false;             // a thread is inside feed_pending_input
    bool loop_exited_ = false;
    std::condition_variable loop_exit_cv_;
    bool rate_update_warned_ = false;  // set_bitrate failure logged once
    // Output access unit with filler NAL units removed (collect_output only).
    std::vector<uint8_t> filtered_output_;

    VideoCodecId codec_ = VideoCodecId::H264;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t fps_ = 30;
    bool initialized_ = false;
    bool mf_started_ = false;  // MFStartup succeeded — MFShutdown owed even if init() failed later
    // Set by force_keyframe() from any thread (PLI handlers), consumed by the
    // thread that submits frames.
    std::atomic<bool> force_keyframe_{false};
};

} // namespace parties::encdec::mft
