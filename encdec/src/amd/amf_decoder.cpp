#include "amf_decoder.h"
#include "amf_loader.h"

#include <AMF/components/VideoDecoderUVD.h>
#include <AMF/core/Surface.h>
#include <AMF/core/Plane.h>

#include <parties/profiler.h>
#include <parties/log.h>
#include <cstring>
#include <dxgi.h>

#ifdef _WIN32
#include <windows.h>
// Wrap AMF calls in SEH to catch driver access violations gracefully.
// Must be in a plain-C function (no C++ destructors in scope).
static AMF_RESULT safe_submit_input(amf::AMFComponent* decoder, amf::AMFData* data) {
    __try {
        return decoder->SubmitInput(data);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                    ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return AMF_FAIL;
    }
}

static AMF_RESULT safe_query_output(amf::AMFComponent* decoder, amf::AMFData** data) {
    __try {
        return decoder->QueryOutput(data);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                    ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        *data = nullptr;
        return AMF_FAIL;
    }
}

static AMF_RESULT safe_drain(amf::AMFComponent* decoder) {
    __try {
        return decoder->Drain();
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                    ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return AMF_FAIL;
    }
}

static AMF_RESULT safe_reinit(amf::AMFComponent* decoder, uint32_t width, uint32_t height) {
    __try {
        return decoder->ReInit(width, height);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                    ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return AMF_FAIL;
    }
}
#else
static AMF_RESULT safe_submit_input(amf::AMFComponent* decoder, amf::AMFData* data) {
    return decoder->SubmitInput(data);
}
static AMF_RESULT safe_query_output(amf::AMFComponent* decoder, amf::AMFData** data) {
    return decoder->QueryOutput(data);
}
static AMF_RESULT safe_drain(amf::AMFComponent* decoder) {
    return decoder->Drain();
}
static AMF_RESULT safe_reinit(amf::AMFComponent* decoder, uint32_t width, uint32_t height) {
    return decoder->ReInit(width, height);
}
#endif

namespace parties::encdec::amd {

static const wchar_t* decoder_component_id(VideoCodecId codec) {
    switch (codec) {
    case VideoCodecId::AV1:  return AMFVideoDecoderHW_AV1;
    case VideoCodecId::H265: return AMFVideoDecoderHW_H265_HEVC;
    case VideoCodecId::H264: return AMFVideoDecoderUVD_H264_AVC;
    default:                 return AMFVideoDecoderHW_AV1;
    }
}

AmfDecoder::AmfDecoder() = default;

AmfDecoder::~AmfDecoder() {
    if (!initialized_ && !context_lost_) return;

    if (decoder_) {
        if (!context_lost_) {
            decoder_->Drain();
            decoder_->Terminate();
        }
        decoder_->Release();
        decoder_ = nullptr;
    }

    if (context_) {
        if (!context_lost_)
            context_->Terminate();
        context_->Release();
        context_ = nullptr;
    }

    device_.Reset();
    initialized_ = false;
    context_lost_ = false;
}

bool AmfDecoder::check_device_health() {
    if (!device_) return false;
    HRESULT reason = device_->GetDeviceRemovedReason();
    if (reason != S_OK) {
        LOG_ERROR("D3D11 device removed (reason=0x{:08x}) — AMF context lost", (unsigned)reason);
        context_lost_ = true;
        return false;
    }
    return true;
}

bool AmfDecoder::init(VideoCodecId codec, uint32_t width, uint32_t height) {
    ZoneScopedN("AmfDecoder::init");
    if (initialized_) return false;

    if (!load_amf(factory_)) return false;

    // Create a dedicated D3D11 device for AMF decoding so we can monitor its health.
    // Enumerate DXGI adapters to find the AMD GPU.
    Microsoft::WRL::ComPtr<IDXGIFactory1> dxgi_factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&dxgi_factory)))) {
        LOG_ERROR("CreateDXGIFactory1 failed");
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> amd_adapter;
    for (UINT i = 0; dxgi_factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.VendorId == 0x1002) { // AMD
            amd_adapter = adapter;
            break;
        }
        adapter.Reset();
    }

    D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_1;
    HRESULT hr = D3D11CreateDevice(
        amd_adapter.Get(),  // nullptr = default adapter, or specific AMD adapter
        amd_adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
        nullptr, 0,
        &feature_level, 1,
        D3D11_SDK_VERSION,
        &device_, nullptr, nullptr);
    if (FAILED(hr)) {
        LOG_ERROR("D3D11CreateDevice for AMF decoder failed: 0x{:08x}", (unsigned)hr);
        return false;
    }

    AMF_RESULT res = factory_->CreateContext(&context_);
    if (res != AMF_OK || !context_) {
        LOG_ERROR("Decoder CreateContext failed: {}", (int)res);
        return false;
    }

    res = context_->InitDX11(device_.Get());
    if (res != AMF_OK) {
        LOG_ERROR("Decoder InitDX11 failed: {}", (int)res);
        context_->Release();
        context_ = nullptr;
        return false;
    }

    const wchar_t* comp_id = decoder_component_id(codec);
    res = factory_->CreateComponent(context_, comp_id, &decoder_);
    if (res != AMF_OK || !decoder_) {
        LOG_ERROR("Decoder CreateComponent({}) failed: {}",
                  codec_name(codec), (int)res);
        context_->Release();
        context_ = nullptr;
        return false;
    }

    decoder_->SetProperty(AMF_VIDEO_DECODER_REORDER_MODE,
        static_cast<amf_int64>(AMF_VIDEO_DECODER_MODE_LOW_LATENCY));

    uint32_t w = width > 0 ? width : 1920;
    uint32_t h = height > 0 ? height : 1080;

    res = decoder_->Init(amf::AMF_SURFACE_NV12, w, h);
    if (res != AMF_OK) {
        LOG_ERROR("Decoder Init({}, {}x{}) failed: {}",
                  codec_name(codec), w, h, (int)res);
        decoder_->Release();
        decoder_ = nullptr;
        context_->Release();
        context_ = nullptr;
        return false;
    }

    codec_ = codec;
    width_ = w;
    height_ = h;

    LOG_INFO("Initialized {} decoder ({}x{})",
             codec_name(codec), w, h);
    initialized_ = true;
    return true;
}

bool AmfDecoder::decode(const uint8_t* data, size_t len, int64_t timestamp) {
    ZoneScopedN("AmfDecoder::decode");
    if (!initialized_ || context_lost_) return false;
    if (!data || len == 0) return false;
    if (!decoder_ || !context_ || !check_device_health())
        return false;

    amf::AMFBuffer* buffer = nullptr;
    AMF_RESULT res = context_->AllocBuffer(amf::AMF_MEMORY_HOST, len, &buffer);
    if (res != AMF_OK || !buffer) {
        LOG_ERROR("AllocBuffer failed: {}", (int)res);
        if (res == AMF_FAIL || res == AMF_NOT_INITIALIZED)
            context_lost_ = true;
        return false;
    }

    void* native = buffer->GetNative();
    if (!native) {
        LOG_ERROR("AMFBuffer::GetNative() returned null");
        buffer->Release();
        return false;
    }

    std::memcpy(native, data, len);
    buffer->SetPts(timestamp);

    res = safe_submit_input(decoder_, buffer);
    buffer->Release();

    if (res != AMF_OK && res != AMF_DECODER_NO_FREE_SURFACES) {
        LOG_ERROR("Decoder SubmitInput failed: {} (len={})", (int)res, len);
        if (res == AMF_FAIL || res == AMF_NOT_INITIALIZED) {
            LOG_ERROR("AMF context lost — will fall back to software decoder");
            context_lost_ = true;
        }
        return false;
    }

    while (true) {
        amf::AMFData* out_data = nullptr;
        res = safe_query_output(decoder_, &out_data);
        if (res == AMF_REPEAT || !out_data) break;
        if (res != AMF_OK) break;

        amf::AMFSurface* surface = nullptr;
        out_data->QueryInterface(amf::AMFSurface::IID(), reinterpret_cast<void**>(&surface));
        if (!surface) {
            out_data->Release();
            continue;
        }

        res = surface->Convert(amf::AMF_MEMORY_HOST);
        if (res != AMF_OK) {
            surface->Release();
            out_data->Release();
            continue;
        }

        amf::AMFPlane* y_plane = surface->GetPlane(amf::AMF_PLANE_Y);
        amf::AMFPlane* uv_plane = surface->GetPlane(amf::AMF_PLANE_UV);

        if (y_plane && uv_plane && on_decoded) {
            DecodedFrame frame{};
            frame.y_plane = static_cast<const uint8_t*>(y_plane->GetNative());
            frame.u_plane = static_cast<const uint8_t*>(uv_plane->GetNative());
            frame.v_plane = nullptr;
            frame.y_stride = static_cast<uint32_t>(y_plane->GetHPitch());
            frame.uv_stride = static_cast<uint32_t>(uv_plane->GetHPitch());
            frame.width = static_cast<uint32_t>(y_plane->GetWidth());
            frame.height = static_cast<uint32_t>(y_plane->GetHeight());
            frame.timestamp = out_data->GetPts();
            frame.nv12 = true;

            on_decoded(frame);
        }

        surface->Release();
        out_data->Release();
    }

    return true;
}

void AmfDecoder::flush() {
    if (!initialized_ || !decoder_ || context_lost_) return;
    if (!check_device_health()) return;

    AMF_RESULT res = safe_drain(decoder_);
    if (res != AMF_OK && res != AMF_EOF) {
        LOG_ERROR("AMF Drain failed: {} - marking context lost", (int)res);
        context_lost_ = true;
        return;
    }
    while (true) {
        amf::AMFData* data = nullptr;
        res = safe_query_output(decoder_, &data);
        if (res != AMF_OK || !data) break;
        data->Release();
    }
    res = safe_reinit(decoder_, width_, height_);
    if (res != AMF_OK) {
        LOG_ERROR("AMF ReInit failed: {} - marking context lost", (int)res);
        context_lost_ = true;
    }
}

DecoderInfo AmfDecoder::info() const {
    return {Backend::AMF, codec_};
}

} // namespace parties::encdec::amd
