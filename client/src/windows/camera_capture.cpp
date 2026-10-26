#include <client/camera_capture.h>

#include <parties/log.h>
#include <parties/profiler.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <tuple>

using Microsoft::WRL::ComPtr;

namespace parties::client {

namespace {

std::string utf8(const wchar_t* text)
{
    if (!text || !*text) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), len, nullptr, nullptr);
    return out;
}

std::string describe(const char* what, HRESULT hr)
{
    if (hr == E_ACCESSDENIED)
        return std::string(what) + ": access denied (check the Windows camera privacy settings)";
    char code[16];
    std::snprintf(code, sizeof(code), "%#010lx", static_cast<unsigned long>(hr));
    return std::string(what) + " (" + code + ")";
}

// COM for MF on threads we do not own (the UI thread may already be an STA:
// MF works there too, so a changed mode is not an error).
struct ComScope {
    bool owned = false;
    ComScope() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        owned = SUCCEEDED(hr);
    }
    ~ComScope() { if (owned) CoUninitialize(); }
};

struct MfScope {
    bool started = false;
    MfScope() { started = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)); }
    ~MfScope() { if (started) MFShutdown(); }
};

// Device-native format chosen for the requested box and frame rate.
struct NativeFormat {
    ComPtr<IMFMediaType> type;
    uint32_t width = 0, height = 0;
    uint32_t rate_num = 0, rate_den = 1;
};

double rate_of(const NativeFormat& f)
{
    return f.rate_den ? static_cast<double>(f.rate_num) / f.rate_den : 0.0;
}

// Lower is better. Uncompressed formats avoid a decode; MJPEG is what most
// webcams offer at 720p30+ over USB 2; anything else (H.264 cameras, RGB24)
// still works through the reader's converters but is the last resort.
int subtype_rank(const GUID& subtype)
{
    if (subtype == MFVideoFormat_NV12)  return 0;
    if (subtype == MFVideoFormat_YUY2)  return 1;
    if (subtype == MFVideoFormat_MJPG)  return 2;
    if (subtype == MFVideoFormat_H264 || subtype == MFVideoFormat_HEVC) return 4;
    return 3;
}

// Picks the device format for a capture box of max_w x max_h at `fps`:
// first one that reaches the frame rate, then one that covers the box (we
// only ever scale down), then the box's shape (a 4:3 mode is not picked over
// a 16:9 one for a 16:9 box), then the smallest covering size or else the
// largest below it, then the frame rate closest to the request (a 60 fps
// mode costs twice the conversions and exposes half as long), then the
// cheapest pixel format.
bool choose_native_format(IMFMediaTypeHandler* handler, uint32_t max_w, uint32_t max_h,
                          uint32_t fps, NativeFormat& out)
{
    const double box_aspect = static_cast<double>(max_w) / max_h;
    DWORD count = 0;
    if (FAILED(handler->GetMediaTypeCount(&count))) return false;

    bool have = false;
    NativeFormat best;
    GUID best_subtype{};
    for (DWORD i = 0; i < count; ++i) {
        ComPtr<IMFMediaType> type;
        if (FAILED(handler->GetMediaTypeByIndex(i, &type))) continue;
        GUID subtype{};
        if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype))) continue;
        NativeFormat f;
        f.type = type;
        if (FAILED(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &f.width, &f.height)) ||
            f.width == 0 || f.height == 0)
            continue;
        if (FAILED(MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &f.rate_num, &f.rate_den)) ||
            f.rate_den == 0 || f.rate_num == 0)
            continue;

        if (!have) { best = f; best_subtype = subtype; have = true; continue; }

        const auto key = [&](const NativeFormat& c, const GUID& st) {
            const bool fast   = rate_of(c) + 0.5 >= fps;
            const bool covers = c.width >= max_w || c.height >= max_h;
            const bool shaped = std::fabs(std::log(static_cast<double>(c.width) / c.height / box_aspect)) < 0.05;
            // Covering: smaller area is better. Not covering: larger is better.
            const int64_t area = static_cast<int64_t>(c.width) * c.height;
            const double rate_error = std::fabs(rate_of(c) - fps);
            return std::make_tuple(fast ? 0 : 1, covers ? 0 : 1, shaped ? 0 : 1,
                                   covers ? area : -area, std::lround(rate_error),
                                   subtype_rank(st), rate_error);
        };
        if (key(f, subtype) < key(best, best_subtype)) { best = f; best_subtype = subtype; }
    }
    if (!have) return false;
    out = best;
    return true;
}

// Fits w x h into the box preserving aspect, never upscaling; even sizes.
void fit_box(uint32_t w, uint32_t h, uint32_t max_w, uint32_t max_h, uint32_t& out_w, uint32_t& out_h)
{
    const double scale = (std::min)({1.0, static_cast<double>(max_w) / w, static_cast<double>(max_h) / h});
    out_w = (std::max)(2u, static_cast<uint32_t>(std::lround(w * scale)) & ~1u);
    out_h = (std::max)(2u, static_cast<uint32_t>(std::lround(h * scale)) & ~1u);
}

} // namespace

// ── Reader callback ──────────────────────────────────────────────────────────
// A separate, properly reference-counted COM object: MF may release its last
// reference after the reader is gone, so it must not live inside Impl. Impl
// detaches it before tearing down; late callbacks then do nothing.

class CameraReaderCallback final : public IMFSourceReaderCallback {
public:
    struct Sink {
        virtual void on_read_sample(HRESULT status, DWORD flags, LONGLONG timestamp, IMFSample* sample) = 0;
        virtual void on_flush() = 0;
    protected:
        ~Sink() = default;
    };

    explicit CameraReaderCallback(Sink* sink) : sink_(sink) {}

    void detach()
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        sink_ = nullptr;
    }

    STDMETHODIMP QueryInterface(REFIID iid, void** out) override
    {
        if (!out) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFSourceReaderCallback)) {
            *out = static_cast<IMFSourceReaderCallback*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override
    {
        const ULONG left = --refs_;
        if (left == 0) delete this;
        return left;
    }

    STDMETHODIMP OnReadSample(HRESULT status, DWORD /*stream*/, DWORD flags, LONGLONG timestamp,
                              IMFSample* sample) override
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (sink_) sink_->on_read_sample(status, flags, timestamp, sample);
        return S_OK;
    }
    STDMETHODIMP OnFlush(DWORD /*stream*/) override
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (sink_) sink_->on_flush();
        return S_OK;
    }
    STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }

private:
    ~CameraReaderCallback() = default;
    std::atomic<ULONG> refs_{1};
    // Serializes callbacks against detach(). Recursive: the sink issues the
    // next ReadSample from inside OnReadSample.
    std::recursive_mutex mutex_;
    Sink* sink_ = nullptr;
};

// ── Capture ─────────────────────────────────────────────────────────────────

struct CameraCapture::Impl final : CameraReaderCallback::Sink {
    MfScope mf;

    ComPtr<ID3D11Device>        device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IMFDXGIDeviceManager> manager;
    UINT reset_token = 0;
    ComPtr<IMFMediaSource>  source;
    ComPtr<IMFSourceReader> reader;
    CameraReaderCallback*   callback = nullptr;   // one reference held here
    bool     gpu = false;
    uint32_t width = 0, height = 0, fps = 0;      // as opened (accessors, logging)

    // Geometry of the reader's output type. Changes only with
    // MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED and travels with each sample.
    struct FrameFormat {
        uint32_t width = 0, height = 0;
        LONG     stride = 0;   // default stride: negative = bottom-up rows
    };
    FrameFormat format;   // guarded by mutex once running

    FrameFn on_frame;
    ErrorFn on_error;

    // Mailbox between the reader callback (MF work queue) and the worker.
    std::mutex mutex;
    std::condition_variable cv;
    ComPtr<IMFSample> pending;
    int64_t pending_ts = 0;
    FrameFormat pending_format;
    bool running = false;
    bool flushed = false;
    std::string error;   // first failure, reported once by the worker
    std::thread worker;

    // Worker-thread only.
    ComPtr<ID3D11Texture2D> frame_tex;
    uint32_t frame_w = 0, frame_h = 0;
    std::vector<uint8_t> upload;

    ~Impl() { teardown(); }

    bool create_device()
    {
        // The adapter that drives the displays: the one the screen capture and
        // the hardware encoders prefer too.
        ComPtr<IDXGIFactory1> factory;
        ComPtr<IDXGIAdapter1> chosen;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            ComPtr<IDXGIAdapter1> adapter;
            for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
                ComPtr<IDXGIOutput> output;
                if (adapter->EnumOutputs(0, &output) != DXGI_ERROR_NOT_FOUND) {
                    chosen = adapter;
                    break;
                }
                adapter.Reset();
            }
        }
        const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        HRESULT hr = D3D11CreateDevice(chosen.Get(),
                                       chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                                       nullptr, flags, nullptr, 0, D3D11_SDK_VERSION,
                                       &device, nullptr, &context);
        if (FAILED(hr)) {
            hr = D3D11CreateDevice(chosen.Get(),
                                   chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                                   nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                   D3D11_SDK_VERSION, &device, nullptr, &context);
        }
        if (FAILED(hr)) return false;
        // MF's video processor and the encoder share this device's context.
        ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(device.As(&mt))) mt->SetMultithreadProtected(TRUE);
        return true;
    }

    bool set_output_type(const GUID& subtype, uint32_t w, uint32_t h, const NativeFormat& native)
    {
        ComPtr<IMFMediaType> type;
        if (FAILED(MFCreateMediaType(&type))) return false;
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        type->SetGUID(MF_MT_SUBTYPE, subtype);
        MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, w, h);
        MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, native.rate_num, native.rate_den);
        MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        return SUCCEEDED(reader->SetCurrentMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, type.Get()));
    }

    bool read_format(FrameFormat& out)
    {
        ComPtr<IMFMediaType> current;
        FrameFormat f;
        if (FAILED(reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                                               &current)) ||
            FAILED(MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &f.width, &f.height)) ||
            f.width == 0 || f.height == 0)
            return false;
        UINT32 stride = 0;
        GUID subtype{};
        if (SUCCEEDED(current->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)))
            f.stride = static_cast<LONG>(static_cast<INT32>(stride));
        else if (FAILED(current->GetGUID(MF_MT_SUBTYPE, &subtype)) ||
                 FAILED(MFGetStrideForBitmapInfoHeader(subtype.Data1, f.width, &f.stride)))
            f.stride = static_cast<LONG>(f.width * 4);
        out = f;
        return true;
    }

    bool open(const CameraDevice& dev, const Config& config, std::string& err)
    {
        if (!mf.started) { err = "Media Foundation is unavailable"; return false; }
        if (!create_device()) { err = "could not create a D3D11 device"; return false; }
        if (FAILED(MFCreateDXGIDeviceManager(&reset_token, &manager)) ||
            FAILED(manager->ResetDevice(device.Get(), reset_token)))
            manager.Reset();   // CPU frames only

        ComPtr<IMFAttributes> attrs;
        HRESULT hr = MFCreateAttributes(&attrs, 2);
        if (SUCCEEDED(hr)) hr = attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                              MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        if (SUCCEEDED(hr)) hr = attrs->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                                                dev.symbolic_link.c_str());
        if (SUCCEEDED(hr)) hr = MFCreateDeviceSource(attrs.Get(), &source);
        if (FAILED(hr)) { err = describe("could not open the camera", hr); return false; }

        // Select the device format on the source itself: the reader then
        // builds its converters from exactly this format.
        NativeFormat native;
        {
            ComPtr<IMFPresentationDescriptor> pd;
            ComPtr<IMFStreamDescriptor> sd;
            ComPtr<IMFMediaTypeHandler> handler;
            BOOL selected = FALSE;
            hr = source->CreatePresentationDescriptor(&pd);
            if (SUCCEEDED(hr)) hr = pd->GetStreamDescriptorByIndex(0, &selected, &sd);
            if (SUCCEEDED(hr)) hr = sd->GetMediaTypeHandler(&handler);
            if (FAILED(hr) || !choose_native_format(handler.Get(), config.max_width,
                                                    config.max_height, config.fps, native)) {
                err = describe("the camera reports no usable video format", hr);
                return false;
            }
            hr = handler->SetCurrentMediaType(native.type.Get());
            if (FAILED(hr)) { err = describe("could not select the camera format", hr); return false; }
        }

        uint32_t out_w = 0, out_h = 0;
        fit_box(native.width, native.height, config.max_width, config.max_height, out_w, out_h);

        callback = new CameraReaderCallback(this);
        // GPU first (DXGI device manager: decode, convert and scale on the
        // GPU, frames arrive as textures), system memory second.
        for (const bool use_gpu : {true, false}) {
            if (use_gpu && !manager) continue;
            ComPtr<IMFAttributes> ra;
            if (FAILED(MFCreateAttributes(&ra, 5))) continue;
            ra->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, static_cast<IMFSourceReaderCallback*>(callback));
            ra->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
            // A failed attempt must not shut the source down with its reader.
            ra->SetUINT32(MF_SOURCE_READER_DISCONNECT_MEDIASOURCE_ON_SHUTDOWN, TRUE);
            if (use_gpu) {
                ra->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, manager.Get());
                ra->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
            }
            if (FAILED(MFCreateSourceReaderFromMediaSource(source.Get(), ra.Get(), &reader)))
                continue;
            // ARGB32 is B8G8R8A8 in memory and as a texture: the encoders'
            // input format. RGB32 (X8) is acceptable only in system memory,
            // where it is copied into a B8G8R8A8 texture anyway.
            bool ok = set_output_type(MFVideoFormat_ARGB32, out_w, out_h, native) ||
                      ((out_w != native.width || out_h != native.height) &&
                       set_output_type(MFVideoFormat_ARGB32, native.width, native.height, native));
            if (!ok && !use_gpu)
                ok = set_output_type(MFVideoFormat_RGB32, out_w, out_h, native) ||
                     set_output_type(MFVideoFormat_RGB32, native.width, native.height, native);
            if (ok) { gpu = use_gpu; break; }
            reader.Reset();
        }
        if (!reader) { err = "the camera format cannot be converted for encoding"; return false; }

        if (!read_format(format)) {
            err = "the camera output format is unknown";
            return false;
        }
        width = format.width;
        height = format.height;
        fps = static_cast<uint32_t>(std::lround(rate_of(native)));

        GUID native_subtype{};
        native.type->GetGUID(MF_MT_SUBTYPE, &native_subtype);
        LOG_INFO("Camera '{}': device {}x{} @ {:.2f} fps ({}), delivering {}x{} via {}",
                 dev.name, native.width, native.height, rate_of(native),
                 native_subtype == MFVideoFormat_MJPG ? "MJPEG"
                 : native_subtype == MFVideoFormat_YUY2 ? "YUY2"
                 : native_subtype == MFVideoFormat_NV12 ? "NV12" : "other",
                 width, height, gpu ? "GPU" : "system memory");
        return true;
    }

    void request_next()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!running) return;
        }
        const HRESULT hr = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                                              0, nullptr, nullptr, nullptr, nullptr);
        if (FAILED(hr)) {
            std::lock_guard<std::mutex> lock(mutex);
            if (error.empty()) error = describe("camera capture failed", hr);
            cv.notify_all();
        }
    }

    // MF work queue thread.
    void on_read_sample(HRESULT status, DWORD flags, LONGLONG timestamp, IMFSample* sample) override
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!running) return;
            if (FAILED(status) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) {
                if (error.empty())
                    error = FAILED(status) ? describe("camera capture failed", status)
                                           : "the camera stopped delivering frames";
                cv.notify_all();
                return;   // no further reads
            }
            // A device format switch (some drivers renegotiate on light
            // changes) can change the output geometry from here on.
            if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
                read_format(format);
            if (sample) {
                pending = sample;   // newest wins: an unconsumed frame is dropped
                pending_ts = timestamp;
                pending_format = format;
                cv.notify_all();
            }
        }
        request_next();
    }

    void on_flush() override
    {
        std::lock_guard<std::mutex> lock(mutex);
        flushed = true;
        cv.notify_all();
    }

    bool ensure_frame_texture(uint32_t w, uint32_t h)
    {
        if (frame_tex && frame_w == w && frame_h == h) return true;
        frame_tex.Reset();
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &frame_tex))) return false;
        frame_w = w;
        frame_h = h;
        return true;
    }

    // Copies one sample into frame_tex. False for a frame that cannot be used.
    bool load_frame(IMFSample* sample, const FrameFormat& fmt)
    {
        const uint32_t width = fmt.width, height = fmt.height;
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->GetBufferByIndex(0, &buffer))) return false;

        ComPtr<IMFDXGIBuffer> dxgi;
        if (SUCCEEDED(buffer.As(&dxgi))) {
            ComPtr<ID3D11Texture2D> texture;
            UINT subresource = 0;
            if (FAILED(dxgi->GetResource(IID_PPV_ARGS(&texture)))) return false;
            dxgi->GetSubresourceIndex(&subresource);
            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);
            // The allocator may pad the surface: copy only the picture.
            if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
                desc.Width < width || desc.Height < height)
                return false;
            if (!ensure_frame_texture(width, height)) return false;
            const D3D11_BOX box{0, 0, 0, width, height, 1};
            context->CopySubresourceRegion(frame_tex.Get(), 0, 0, 0, 0, texture.Get(),
                                           subresource, &box);
            return true;
        }

        if (!ensure_frame_texture(width, height)) return false;
        const UINT row_bytes = width * 4;
        ComPtr<IMF2DBuffer> buffer2d;
        BYTE* scan0 = nullptr;
        LONG  pitch = 0;
        bool  locked2d = SUCCEEDED(buffer.As(&buffer2d)) &&
                         SUCCEEDED(buffer2d->Lock2D(&scan0, &pitch));
        BYTE* data = nullptr;
        DWORD length = 0;
        if (!locked2d) {
            if (FAILED(buffer->Lock(&data, nullptr, &length))) return false;
            // A plain buffer follows the type's default stride; negative means
            // bottom-up rows, scan line 0 being the last row in memory.
            pitch = fmt.stride;
            const size_t stride = static_cast<size_t>(std::abs(fmt.stride));
            if (stride < row_bytes || length < stride * height) { buffer->Unlock(); return false; }
            scan0 = pitch < 0 ? data + stride * (height - 1) : data;
        }
        if (pitch == static_cast<LONG>(row_bytes)) {
            context->UpdateSubresource(frame_tex.Get(), 0, nullptr, scan0, row_bytes, 0);
        } else {
            // Bottom-up or padded rows: repack top-down.
            upload.resize(static_cast<size_t>(row_bytes) * height);
            for (uint32_t y = 0; y < height; ++y)
                std::memcpy(upload.data() + static_cast<size_t>(y) * row_bytes,
                            scan0 + static_cast<ptrdiff_t>(y) * pitch, row_bytes);
            context->UpdateSubresource(frame_tex.Get(), 0, nullptr, upload.data(), row_bytes, 0);
        }
        if (locked2d) buffer2d->Unlock2D();
        else          buffer->Unlock();
        return true;
    }

    void worker_loop()
    {
        TracySetThreadName("CameraCapture");
        const ComScope com;
        for (;;) {
            ComPtr<IMFSample> sample;
            int64_t timestamp = 0;
            FrameFormat fmt;
            std::string failure;
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [&] { return !running || pending || !error.empty(); });
                if (!running) break;
                if (pending) {
                    sample = std::move(pending);
                    timestamp = pending_ts;
                    fmt = pending_format;
                } else {
                    failure = error;
                }
            }
            if (!failure.empty()) {
                if (on_error) on_error(failure);
                break;   // the device is gone; stop() finishes the teardown
            }
            ZoneScopedN("CameraCapture::frame");
            if (load_frame(sample.Get(), fmt) && on_frame)
                on_frame(frame_tex.Get(), frame_w, frame_h, timestamp);
        }
    }

    void teardown()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            running = false;
        }
        cv.notify_all();
        if (reader) {
            // Cancel the outstanding read; OnFlush follows once it completed.
            bool flushing = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                flushed = false;
            }
            flushing = SUCCEEDED(reader->Flush(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS)));
            if (flushing) {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait_for(lock, std::chrono::seconds(2), [&] { return flushed; });
            }
        }
        if (worker.joinable()) worker.join();
        if (callback) {
            callback->detach();
            callback->Release();
            callback = nullptr;
        }
        reader.Reset();
        if (source) {
            source->Shutdown();
            source.Reset();
        }
        pending.Reset();
        frame_tex.Reset();
        manager.Reset();
        context.Reset();
        device.Reset();
    }
};

std::vector<CameraDevice> CameraCapture::enumerate()
{
    std::vector<CameraDevice> devices;
    const ComScope com;
    const MfScope mf;
    if (!mf.started) return devices;

    ComPtr<IMFAttributes> attrs;
    if (FAILED(MFCreateAttributes(&attrs, 1)) ||
        FAILED(attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                              MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID)))
        return devices;

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    if (FAILED(MFEnumDeviceSources(attrs.Get(), &activates, &count))) return devices;
    for (UINT32 i = 0; i < count; ++i) {
        WCHAR* name = nullptr;
        WCHAR* link = nullptr;
        UINT32 name_len = 0, link_len = 0;
        if (SUCCEEDED(activates[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
                                                       &name, &name_len)) &&
            SUCCEEDED(activates[i]->GetAllocatedString(
                MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link, &link_len))) {
            CameraDevice device;
            device.name = utf8(name);
            device.symbolic_link.assign(link, link_len);
            // Two identical webcams share a friendly name: keep them apart.
            int same = 1;
            for (const auto& other : devices)
                if (other.name == device.name || other.name.rfind(device.name + " (", 0) == 0) ++same;
            if (same > 1) device.name += " (" + std::to_string(same) + ")";
            devices.push_back(std::move(device));
        }
        CoTaskMemFree(name);
        CoTaskMemFree(link);
        activates[i]->Release();
    }
    CoTaskMemFree(activates);
    return devices;
}

CameraCapture::CameraCapture() = default;
CameraCapture::~CameraCapture() { stop(); }

bool CameraCapture::start(const CameraDevice& device, const Config& config,
                          FrameFn on_frame, ErrorFn on_error)
{
    ZoneScopedN("CameraCapture::start");
    stop();
    const ComScope com;
    auto impl = std::make_unique<Impl>();
    impl->on_frame = std::move(on_frame);
    impl->on_error = std::move(on_error);
    std::string error;
    if (!impl->open(device, config, error)) {
        LOG_WARN("Camera '{}': {}", device.name, error);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        impl->running = true;
    }
    Impl* raw = impl.get();
    impl->worker = std::thread([raw] { raw->worker_loop(); });
    impl->request_next();
    impl_ = std::move(impl);
    return true;
}

void CameraCapture::stop()
{
    if (!impl_) return;
    const ComScope com;
    impl_->teardown();
    impl_.reset();
}

ID3D11Device* CameraCapture::device() const { return impl_ ? impl_->device.Get() : nullptr; }
uint32_t CameraCapture::width() const  { return impl_ ? impl_->width  : 0; }
uint32_t CameraCapture::height() const { return impl_ ? impl_->height : 0; }
uint32_t CameraCapture::fps() const    { return impl_ ? impl_->fps    : 0; }

} // namespace parties::client
