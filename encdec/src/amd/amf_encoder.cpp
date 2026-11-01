#include "amf_encoder.h"
#include "amf_loader.h"
#include <encdec/rate_control.h>

#include <AMF/components/VideoEncoderVCE.h>
#include <AMF/components/VideoEncoderHEVC.h>
#include <AMF/components/VideoEncoderAV1.h>

#include <parties/profiler.h>
#include <parties/log.h>

namespace parties::encdec::amd {

namespace {

// QueryOutput wait budget: one frame interval rounded up, never below 8 ms
// so that very high frame rates still give the hardware a chance to finish.
amf_int64 query_timeout_ms_for_fps(uint32_t fps) {
    const uint32_t safe_fps = fps == 0 ? 30u : fps;
    const uint32_t interval_ms = (1000u + safe_fps - 1u) / safe_fps;
    return static_cast<amf_int64>(interval_ms < 8u ? 8u : interval_ms);
}

// The HEVC component applies VBV_BUFFER_SIZE only at Init. Once the bitrate
// has moved this far from the rate the VBV was sized for, the init-time VBV no
// longer matches the two-frame policy and the encoder should be recreated.
constexpr uint64_t HEVC_REINIT_RATIO_NUM = 3;
constexpr uint64_t HEVC_REINIT_RATIO_DEN = 2;

bool outside_reinit_ratio(uint32_t bitrate, uint32_t sized_for) {
    const uint64_t a = bitrate;
    const uint64_t b = sized_for;
    return a * HEVC_REINIT_RATIO_DEN > b * HEVC_REINIT_RATIO_NUM ||
           b * HEVC_REINIT_RATIO_DEN > a * HEVC_REINIT_RATIO_NUM;
}

// Upper bound on encoded frames AMF can hold between SubmitInput and
// QueryOutput. Only a sanity clamp for the outstanding-output counter.
constexpr int MAX_OUTSTANDING_OUTPUTS = 16;

} // namespace

AmfEncoder::AmfEncoder() = default;

AmfEncoder::~AmfEncoder() {
    if (!initialized_) return;

    if (encoder_) {
        // Drain is an end-of-stream operation and can block in a driver while
        // the application is tearing down its capture device. There is no
        // consumer for delayed output at this point, so terminate directly.
        encoder_->Terminate();
        encoder_->Release();
        encoder_ = nullptr;
    }

    staging_texture_.Reset();
    d3d_context_.Reset();
    device_.Reset();
    num_external_inputs_ = 0;

    if (context_) {
        context_->Terminate();
        context_->Release();
        context_ = nullptr;
    }

    initialized_ = false;
}

bool AmfEncoder::init(ID3D11Device* device, uint32_t width, uint32_t height,
                       uint32_t fps, uint32_t bitrate, VideoCodecId preferred_codec) {
    ZoneScopedN("AmfEncoder::init");
    if (initialized_) return false;

    if (!load_amf(factory_)) return false;

    AMF_RESULT res = factory_->CreateContext(&context_);
    if (res != AMF_OK || !context_) {
        LOG_ERROR("CreateContext failed: {}", (int)res);
        return false;
    }

    res = context_->InitDX11(device);
    if (res != AMF_OK) {
        LOG_ERROR("InitDX11 failed: {}", (int)res);
        context_->Release();
        context_ = nullptr;
        return false;
    }

    device_ = device;
    device_->GetImmediateContext(&d3d_context_);
    width_ = width;
    height_ = height;
    fps_ = fps;

    struct CodecEntry { const wchar_t* id; VideoCodecId codec; };
    CodecEntry codecs[] = {
        {AMFVideoEncoder_AV1,    VideoCodecId::AV1},
        {AMFVideoEncoder_HEVC,   VideoCodecId::H265},
        {AMFVideoEncoderVCE_AVC, VideoCodecId::H264},
    };

    bool found = false;
    for (auto& c : codecs) {
        if (c.codec == preferred_codec && try_codec(c.id, c.codec)) { found = true; break; }
    }
    if (!found) {
        for (auto& c : codecs) {
            if (c.codec != preferred_codec && try_codec(c.id, c.codec)) { found = true; break; }
        }
    }

    if (!found) {
        LOG_ERROR("No supported encoder codec found");
        context_->Release();
        context_ = nullptr;
        return false;
    }

    const auto rate_control = make_stream_cbr_rate_control(bitrate, fps);
    LOG_INFO("Selected encoder codec: {} ({}x{} @ {} fps), CBR: {} bps, VBV: {} bits, max frame: {} bits",
             codec_name(codec_), width, height, fps,
             rate_control.average_bitrate, rate_control.vbv_buffer_size,
             rate_control.max_frame_bits);
    vbv_sized_for_bitrate_ = bitrate;

    const amf_int64 average_bitrate =
        static_cast<amf_int64>(rate_control.average_bitrate);
    const amf_int64 peak_bitrate =
        static_cast<amf_int64>(rate_control.peak_bitrate);
    const amf_int64 vbv_buffer_size =
        static_cast<amf_int64>(rate_control.vbv_buffer_size);
    const amf_int64 max_frame_bits =
        static_cast<amf_int64>(rate_control.max_frame_bits);
    amf_int64 keyframe_period = static_cast<amf_int64>(fps * (VIDEO_KEYFRAME_INTERVAL_MS / 1000));

    // QueryOutput blocks inside AMF for up to one frame interval (floor 8 ms)
    // so do_encode can collect the frame it just submitted without polling.
    const amf_int64 query_timeout_ms = query_timeout_ms_for_fps(fps);

    // Rate control for all codecs: CBR with the two-frame VBV from
    // rate_control.h, HRD enforced so the VBV is a hard bound, filler data off
    // (padding up to the target rate would only add link load), and every AU
    // capped at the VBV size. MAX_AU_SIZE (H.264/HEVC) and
    // MAX_COMPRESSED_FRAME_SIZE (AV1) are both in bits per the AMF headers.
    // No B-frames anywhere: they add reordering delay.

    if (codec_ == VideoCodecId::AV1) {
        // Static properties (must be set before Init).
        encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_USAGE,
            static_cast<amf_int64>(AMF_VIDEO_ENCODER_AV1_USAGE_LOW_LATENCY));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET,
            static_cast<amf_int64>(AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_SPEED));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_ENCODING_LATENCY_MODE,
            static_cast<amf_int64>(AMF_VIDEO_ENCODER_AV1_ENCODING_LATENCY_MODE_REAL_TIME));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_QUERY_TIMEOUT, query_timeout_ms);
        // Dynamic properties (initial values; set_bitrate updates the rate ones).
        encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD,
            static_cast<amf_int64>(AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_CBR));
        // B-frames are a VCN5 feature; older runtimes reject the properties,
        // which is fine because they cannot produce B-frames either.
        encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_MAX_CONSECUTIVE_BPICTURES, static_cast<amf_int64>(0));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_B_PIC_PATTERN, static_cast<amf_int64>(0));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_TARGET_BITRATE, average_bitrate);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_PEAK_BITRATE, peak_bitrate);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_VBV_BUFFER_SIZE, vbv_buffer_size);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_MAX_COMPRESSED_FRAME_SIZE, max_frame_bits);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_INITIAL_VBV_BUFFER_FULLNESS, static_cast<amf_int64>(64));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_ENFORCE_HRD, true);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_FILLER_DATA, false);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_FRAMERATE, AMFConstructRate(fps, 1));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_GOP_SIZE, keyframe_period);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_HEADER_INSERTION_MODE,
            static_cast<amf_int64>(AMF_VIDEO_ENCODER_AV1_HEADER_INSERTION_MODE_KEY_FRAME_ALIGNED));
    } else if (codec_ == VideoCodecId::H265) {
        // Static properties (must be set before Init). Note that the HEVC
        // header lists VBV_BUFFER_SIZE among the static properties, so the
        // value chosen here is the one that is guaranteed to take effect.
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_USAGE,
            static_cast<amf_int64>(AMF_VIDEO_ENCODER_HEVC_USAGE_LOW_LATENCY));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET,
            static_cast<amf_int64>(AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_SPEED));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_LOWLATENCY_MODE, true);
        // The HEVC component exposes no B-frame properties (it never emits them).
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD,
            static_cast<amf_int64>(AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_VBV_BUFFER_SIZE, vbv_buffer_size);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_INITIAL_VBV_BUFFER_FULLNESS, static_cast<amf_int64>(64));
        // Dynamic properties (initial values; set_bitrate updates the rate ones).
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_QUERY_TIMEOUT, query_timeout_ms);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE, average_bitrate);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE, peak_bitrate);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_MAX_AU_SIZE, max_frame_bits);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_ENFORCE_HRD, true);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_FILLER_DATA_ENABLE, false);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_FRAMERATE, AMFConstructRate(fps, 1));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_GOP_SIZE, keyframe_period);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_HEADER_INSERTION_MODE,
            static_cast<amf_int64>(AMF_VIDEO_ENCODER_HEVC_HEADER_INSERTION_MODE_IDR_ALIGNED));
    } else {
        // Static properties (must be set before Init).
        encoder_->SetProperty(AMF_VIDEO_ENCODER_USAGE,
            static_cast<amf_int64>(AMF_VIDEO_ENCODER_USAGE_LOW_LATENCY));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_QUALITY_PRESET,
            static_cast<amf_int64>(AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_LOWLATENCY_MODE, true);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_MAX_CONSECUTIVE_BPICTURES, static_cast<amf_int64>(0));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD,
            static_cast<amf_int64>(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR));
        // Dynamic properties (initial values; set_bitrate updates the rate ones).
        encoder_->SetProperty(AMF_VIDEO_ENCODER_QUERY_TIMEOUT, query_timeout_ms);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE, average_bitrate);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_PEAK_BITRATE, peak_bitrate);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE, vbv_buffer_size);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_MAX_AU_SIZE, max_frame_bits);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_INITIAL_VBV_BUFFER_FULLNESS, static_cast<amf_int64>(64));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_ENFORCE_HRD, true);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_FILLER_DATA_ENABLE, false);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_FRAMERATE, AMFConstructRate(fps, 1));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_IDR_PERIOD, keyframe_period);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_B_PIC_PATTERN, static_cast<amf_int64>(0));
        // Documented range 0-1000. Forced keyframes insert SPS/PPS themselves
        // (encode()), so spacing them out further than that costs nothing.
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEADER_INSERTION_SPACING,
                              (std::min)(keyframe_period, static_cast<amf_int64>(1000)));
    }

    res = encoder_->Init(amf::AMF_SURFACE_BGRA, width, height);
    if (res != AMF_OK) {
        LOG_ERROR("Encoder Init(BGRA) failed: {}", (int)res);
        encoder_->Release();
        encoder_ = nullptr;
        context_->Release();
        context_ = nullptr;
        return false;
    }

    initialized_ = true;
    return true;
}

bool AmfEncoder::try_codec(const wchar_t* component_id, VideoCodecId id) {
    amf::AMFComponent* enc = nullptr;
    AMF_RESULT res = factory_->CreateComponent(context_, component_id, &enc);
    if (res != AMF_OK || !enc) return false;

    encoder_ = enc;
    codec_ = id;
    return true;
}

bool AmfEncoder::encode(ID3D11Texture2D* bgra_texture, int64_t timestamp_100ns) {
    ZoneScopedN("AmfEncoder::encode");
    if (!initialized_ || !bgra_texture || !ensure_staging_texture()) return false;

    d3d_context_->CopyResource(staging_texture_.Get(), bgra_texture);
    d3d_context_->Flush();

    return do_encode(staging_texture_.Get(), timestamp_100ns);
}

bool AmfEncoder::ensure_staging_texture() {
    if (staging_texture_) return true;

    // Registered capture textures are the normal path and are wrapped by AMF
    // directly. Allocate a copy target only for callers using encode().
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width_;
    desc.Height = height_;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc = {1, 0};
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    const HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &staging_texture_);
    if (FAILED(hr)) {
        LOG_ERROR("CreateTexture2D AMF fallback staging failed: {:#010x}",
                  static_cast<unsigned>(hr));
        return false;
    }
    return true;
}

int AmfEncoder::register_input(ID3D11Texture2D* texture) {
    if (num_external_inputs_ >= MAX_EXTERNAL_INPUTS) return -1;
    int slot = num_external_inputs_++;
    external_inputs_[slot] = texture;
    return slot;
}

void AmfEncoder::unregister_inputs() {
    for (int i = 0; i < num_external_inputs_; i++)
        external_inputs_[i] = nullptr;
    num_external_inputs_ = 0;
}

bool AmfEncoder::encode_registered(int slot, int64_t timestamp_100ns) {
    ZoneScopedN("AmfEncoder::encode_registered");
    if (!initialized_ || slot < 0 || slot >= num_external_inputs_) return false;
    return do_encode(external_inputs_[slot], timestamp_100ns);
}

bool AmfEncoder::do_encode(ID3D11Texture2D* texture, int64_t timestamp_100ns) {
    amf::AMFSurface* surface = nullptr;
    AMF_RESULT res = context_->CreateSurfaceFromDX11Native(texture, &surface, nullptr);
    if (res != AMF_OK || !surface) {
        LOG_ERROR("CreateSurfaceFromDX11Native failed: {}", (int)res);
        return false;
    }

    surface->SetPts(timestamp_100ns);

    // A forced keyframe also carries the stream headers explicitly, so a
    // viewer that joins on it (or recovers through it) can always start
    // decoding, whatever the header insertion mode does for forced frames.
    const bool requested_keyframe = force_keyframe_.load();
    if (requested_keyframe) {
        if (codec_ == VideoCodecId::AV1) {
            surface->SetProperty(AMF_VIDEO_ENCODER_AV1_FORCE_FRAME_TYPE,
                static_cast<amf_int64>(AMF_VIDEO_ENCODER_AV1_FORCE_FRAME_TYPE_KEY));
            surface->SetProperty(AMF_VIDEO_ENCODER_AV1_FORCE_INSERT_SEQUENCE_HEADER, true);
        } else if (codec_ == VideoCodecId::H265) {
            surface->SetProperty(AMF_VIDEO_ENCODER_HEVC_FORCE_PICTURE_TYPE,
                static_cast<amf_int64>(AMF_VIDEO_ENCODER_HEVC_PICTURE_TYPE_IDR));
            surface->SetProperty(AMF_VIDEO_ENCODER_HEVC_INSERT_HEADER, true);
        } else {
            surface->SetProperty(AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE,
                static_cast<amf_int64>(AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR));
            surface->SetProperty(AMF_VIDEO_ENCODER_INSERT_SPS, true);
            surface->SetProperty(AMF_VIDEO_ENCODER_INSERT_PPS, true);
        }
    }

    // AMF_INPUT_FULL means ownership has not transferred. Retain and retry the
    // exact same surface after consuming output; releasing it here used to drop
    // frames and could also lose a requested IDR.
    static constexpr int max_submit_retries = 8;
    for (int retry = 0; retry < max_submit_retries; ++retry) {
        res = encoder_->SubmitInput(surface);
        if (res == AMF_OK) break;
        if (res != AMF_INPUT_FULL) {
            LOG_ERROR("SubmitInput failed: {}", (int)res);
            surface->Release();
            return false;
        }

        amf::AMFData* pending = nullptr;
        const AMF_RESULT output_result = encoder_->QueryOutput(&pending);
        if (pending) {
            if (outstanding_outputs_ > 0) --outstanding_outputs_;
            if (!deliver_output(pending)) {
                surface->Release();
                return false;
            }
        }
        if (output_result != AMF_OK && output_result != AMF_REPEAT) {
            LOG_ERROR("QueryOutput while encoder input was full failed: {}",
                      static_cast<int>(output_result));
            surface->Release();
            return false;
        }
        Sleep(0);
    }
    surface->Release();

    if (res != AMF_OK) {
        LOG_WARN("AMF encoder remained full after {} retries; dropping newest frame",
                 max_submit_retries);
        return true;
    }
    if (requested_keyframe) force_keyframe_.store(false);
    if (outstanding_outputs_ < MAX_OUTSTANDING_OUTPUTS) ++outstanding_outputs_;

    return drain_output();
}

bool AmfEncoder::drain_output() {
    // AMF produces one output buffer per submitted frame (frame output mode,
    // no B-frames). Drain every output that is owed, not just one: if a slow
    // encode ever left a frame behind, collecting a single output per submit
    // kept the stream permanently one frame late (and a forced keyframe on a
    // static screen never left the encoder).
    //
    // The QUERY_TIMEOUT property makes QueryOutput block inside AMF for up to
    // one frame interval while the newest frame is still encoding, so a call
    // normally returns as soon as it is ready. Only a bounded number of empty
    // (AMF_REPEAT) results is tolerated per submit; an output that is still
    // not ready then is collected by the next call, which keeps the encode
    // thread from stalling on a slow frame.
    static constexpr int max_empty_queries = 3;
    int empty_queries = 0;
    while (outstanding_outputs_ > 0) {
        amf::AMFData* data = nullptr;
        const AMF_RESULT res = encoder_->QueryOutput(&data);
        if (data) {
            --outstanding_outputs_;
            if (!deliver_output(data)) return false;
            continue;
        }
        if (res != AMF_REPEAT && res != AMF_OK) {
            LOG_WARN("QueryOutput failed: {}", static_cast<int>(res));
            // The encoder will not return the outputs it owed; do not wait for
            // them on every later frame.
            outstanding_outputs_ = 0;
            break;
        }
        if (++empty_queries >= max_empty_queries) break;
        // Drivers without query-timeout support return immediately; give the
        // hardware a moment before the (bounded) retry.
        Sleep(1);
    }
    return true;
}

bool AmfEncoder::deliver_output(amf::AMFData* data) {
    if (!data) return true;

    amf::AMFBuffer* buffer = nullptr;
    data->QueryInterface(amf::AMFBuffer::IID(), reinterpret_cast<void**>(&buffer));
    if (!buffer) {
        data->Release();
        return false;
    }

    // Report only real random-access points as keyframes: viewers join and
    // recover on them. An AV1 INTRA_ONLY frame (or a non-IDR H.264/HEVC I
    // frame) does not reset the reference state, so later frames may still
    // reference pictures a joining decoder never saw.
    bool keyframe = false;
    amf_int64 frame_type = 0;
    if (codec_ == VideoCodecId::AV1) {
        if (data->GetProperty(AMF_VIDEO_ENCODER_AV1_OUTPUT_FRAME_TYPE, &frame_type) == AMF_OK)
            keyframe = (frame_type == AMF_VIDEO_ENCODER_AV1_OUTPUT_FRAME_TYPE_KEY);
    } else if (codec_ == VideoCodecId::H265) {
        if (data->GetProperty(AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE, &frame_type) == AMF_OK)
            keyframe = (frame_type == AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE_IDR);
    } else {
        if (data->GetProperty(AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE, &frame_type) == AMF_OK)
            keyframe = (frame_type == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_IDR);
    }

    if (on_encoded) {
        on_encoded(static_cast<const uint8_t*>(buffer->GetNative()),
                   buffer->GetSize(), keyframe);
    }

    buffer->Release();
    data->Release();
    return true;
}

void AmfEncoder::force_keyframe() {
    force_keyframe_ = true;
}

void AmfEncoder::set_bitrate(uint32_t bitrate) {
    if (!initialized_ || !encoder_) return;

    const auto rate_control = make_stream_cbr_rate_control(bitrate, fps_);
    const amf_int64 average_bitrate =
        static_cast<amf_int64>(rate_control.average_bitrate);
    const amf_int64 peak_bitrate =
        static_cast<amf_int64>(rate_control.peak_bitrate);
    const amf_int64 vbv_buffer_size =
        static_cast<amf_int64>(rate_control.vbv_buffer_size);
    const amf_int64 max_frame_bits =
        static_cast<amf_int64>(rate_control.max_frame_bits);
    if (codec_ == VideoCodecId::AV1) {
        encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_TARGET_BITRATE, average_bitrate);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_PEAK_BITRATE, peak_bitrate);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_VBV_BUFFER_SIZE, vbv_buffer_size);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_MAX_COMPRESSED_FRAME_SIZE, max_frame_bits);
    } else if (codec_ == VideoCodecId::H265) {
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE, average_bitrate);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE, peak_bitrate);
        // The HEVC header classifies VBV_BUFFER_SIZE as static: the runtime
        // keeps the init-time value. Set it anyway for runtimes that accept
        // it, and ask the owner to recreate the encoder once the bitrate has
        // drifted so far that the init-time VBV no longer fits the policy
        // (too small starves keyframes, too large brings the bursts back).
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_VBV_BUFFER_SIZE, vbv_buffer_size);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_MAX_AU_SIZE, max_frame_bits);
        if (!needs_reinit_.load(std::memory_order_relaxed) &&
            outside_reinit_ratio(bitrate, vbv_sized_for_bitrate_)) {
            LOG_INFO("AMF HEVC bitrate {} bps is beyond 1.5x of the {} bps its VBV was sized for; "
                     "encoder needs to be recreated",
                     bitrate, vbv_sized_for_bitrate_);
            needs_reinit_.store(true, std::memory_order_relaxed);
        }
    } else {
        encoder_->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE, average_bitrate);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_PEAK_BITRATE, peak_bitrate);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE, vbv_buffer_size);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_MAX_AU_SIZE, max_frame_bits);
    }
    LOG_DEBUG("AMF rate control updated: CBR {} bps, VBV {} bits, max frame {} bits",
              rate_control.average_bitrate, rate_control.vbv_buffer_size,
              rate_control.max_frame_bits);
}

EncoderInfo AmfEncoder::info() const {
    return {Backend::AMF, codec_, width_, height_};
}

} // namespace parties::encdec::amd
