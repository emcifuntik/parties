#include <client/webcam_capture.h>
#include <parties/log.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace parties::client {

namespace {

std::mutex   g_mf_mutex;
int          g_mf_refcount = 0;

bool mf_acquire() {
    std::lock_guard<std::mutex> lock(g_mf_mutex);
    if (g_mf_refcount == 0) {
        HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (FAILED(hr)) {
            LOG_ERROR("MFStartup failed: {:#010x}", static_cast<unsigned>(hr));
            return false;
        }
    }
    ++g_mf_refcount;
    return true;
}

void mf_release() {
    std::lock_guard<std::mutex> lock(g_mf_mutex);
    if (g_mf_refcount == 0)
        return;
    if (--g_mf_refcount == 0)
        MFShutdown();
}

std::string to_utf8(const wchar_t* wide) {
    if (!wide || !*wide)
        return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1)
        return std::string();
    std::string out(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), len, nullptr, nullptr);
    return out;
}

} // namespace

struct WebcamCapture::Impl {
    ComPtr<IMFMediaSource> source;
    ComPtr<IMFSourceReader> reader;

    std::thread worker;
    std::atomic<bool> running{false};

    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;     // absolute bytes per row delivered to the callback
    bool bottom_up = false;  // true if MF buffer rows are bottom-up
    bool mf_held = false;    // whether this instance holds an MFStartup ref

    std::vector<uint8_t> flip_buffer;
};

// WebcamCapture impl

WebcamCapture::WebcamCapture() : impl_(std::make_unique<Impl>()) {}

WebcamCapture::~WebcamCapture() {
    stop();
}

std::vector<WebcamCapture::DeviceInfo> WebcamCapture::enumerate_devices() {
    std::vector<DeviceInfo> results;

    if (!mf_acquire())
        return results;

    ComPtr<IMFAttributes> attrs;
    HRESULT hr = MFCreateAttributes(&attrs, 1);
    if (FAILED(hr)) {
        LOG_ERROR("MFCreateAttributes failed: {:#010x}", static_cast<unsigned>(hr));
        mf_release();
        return results;
    }

    hr = attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (FAILED(hr)) {
        LOG_ERROR("SetGUID(SOURCE_TYPE) failed: {:#010x}", static_cast<unsigned>(hr));
        mf_release();
        return results;
    }

    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    hr = MFEnumDeviceSources(attrs.Get(), &devices, &count);
    if (FAILED(hr)) {
        LOG_ERROR("MFEnumDeviceSources failed: {:#010x}", static_cast<unsigned>(hr));
        mf_release();
        return results;
    }

    for (UINT32 i = 0; i < count; ++i) {
        DeviceInfo info;

        WCHAR* friendly = nullptr;
        UINT32 friendly_len = 0;
        if (SUCCEEDED(devices[i]->GetAllocatedString(
                MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &friendly, &friendly_len))) {
            info.name = to_utf8(friendly);
            CoTaskMemFree(friendly);
        }

        WCHAR* symlink = nullptr;
        UINT32 symlink_len = 0;
        if (SUCCEEDED(devices[i]->GetAllocatedString(
                MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                &symlink, &symlink_len))) {
            info.symbolic_link.assign(symlink, symlink_len);
            CoTaskMemFree(symlink);
        }

        results.push_back(std::move(info));
        devices[i]->Release();
    }
    CoTaskMemFree(devices);

    mf_release();
    return results;
}

bool WebcamCapture::start(int device_index, uint32_t req_width, uint32_t req_height, uint32_t fps) {
    if (impl_->running.load())
        return false;

    if (!mf_acquire())
        return false;
    impl_->mf_held = true;

    ComPtr<IMFAttributes> attrs;
    HRESULT hr = MFCreateAttributes(&attrs, 1);
    if (SUCCEEDED(hr))
        hr = attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (FAILED(hr)) {
        LOG_ERROR("device attrs setup failed: {:#010x}", static_cast<unsigned>(hr));
        stop();
        return false;
    }

    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    hr = MFEnumDeviceSources(attrs.Get(), &devices, &count);
    if (FAILED(hr)) {
        LOG_ERROR("MFEnumDeviceSources failed: {:#010x}", static_cast<unsigned>(hr));
        stop();
        return false;
    }

    if (device_index < 0 || static_cast<UINT32>(device_index) >= count) {
        LOG_ERROR("webcam device_index {} out of range (have {})", device_index, count);
        for (UINT32 i = 0; i < count; ++i)
            devices[i]->Release();
        CoTaskMemFree(devices);
        stop();
        return false;
    }

    hr = devices[device_index]->ActivateObject(IID_PPV_ARGS(&impl_->source));
    for (UINT32 i = 0; i < count; ++i)
        devices[i]->Release();
    CoTaskMemFree(devices);

    if (FAILED(hr)) {
        LOG_ERROR("ActivateObject(IMFMediaSource) failed: {:#010x}", static_cast<unsigned>(hr));
        stop();
        return false;
    }

    // Create a source reader with video processing enabled
    // MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING lets MF color-convert native
    // camera formats (NV12, YUY2, MJPG, ...) to RGB32 for us.
    ComPtr<IMFAttributes> reader_attrs;
    hr = MFCreateAttributes(&reader_attrs, 1);
    if (SUCCEEDED(hr))
        hr = reader_attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    if (FAILED(hr)) {
        LOG_ERROR("reader attrs setup failed: {:#010x}", static_cast<unsigned>(hr));
        stop();
        return false;
    }

    hr = MFCreateSourceReaderFromMediaSource(
        impl_->source.Get(), reader_attrs.Get(), &impl_->reader);
    if (FAILED(hr)) {
        LOG_ERROR("MFCreateSourceReaderFromMediaSource failed: {:#010x}",
                  static_cast<unsigned>(hr));
        stop();
        return false;
    }

    ComPtr<IMFMediaType> desired;
    hr = MFCreateMediaType(&desired);
    if (SUCCEEDED(hr))
        hr = desired->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr))
        hr = desired->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (FAILED(hr)) {
        LOG_ERROR("RGB32 media type setup failed: {:#010x}", static_cast<unsigned>(hr));
        stop();
        return false;
    }

    bool type_set = false;
    if (req_width > 0 && req_height > 0) {
        ComPtr<IMFMediaType> sized;
        if (SUCCEEDED(MFCreateMediaType(&sized))) {
            sized->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            sized->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
            MFSetAttributeSize(sized.Get(), MF_MT_FRAME_SIZE, req_width, req_height);
            if (fps > 0)
                MFSetAttributeRatio(sized.Get(), MF_MT_FRAME_RATE, fps, 1);

            HRESULT shr = impl_->reader->SetCurrentMediaType(
                MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, sized.Get());
            if (SUCCEEDED(shr)) {
                type_set = true;
            } else {
                LOG_INFO("requested {}x{}@{} rejected ({:#010x}); using native size",
                         req_width, req_height, fps, static_cast<unsigned>(shr));
            }
        }
    }

    if (!type_set) {
        hr = impl_->reader->SetCurrentMediaType(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, desired.Get());
        if (FAILED(hr)) {
            LOG_ERROR("SetCurrentMediaType(RGB32) failed: {:#010x}",
                      static_cast<unsigned>(hr));
            stop();
            return false;
        }
    }

    ComPtr<IMFMediaType> actual;
    hr = impl_->reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &actual);
    if (FAILED(hr)) {
        LOG_ERROR("GetCurrentMediaType failed: {:#010x}", static_cast<unsigned>(hr));
        stop();
        return false;
    }

    UINT32 w = 0, h = 0;
    hr = MFGetAttributeSize(actual.Get(), MF_MT_FRAME_SIZE, &w, &h);
    if (FAILED(hr) || w == 0 || h == 0) {
        LOG_ERROR("could not read MF_MT_FRAME_SIZE: {:#010x}", static_cast<unsigned>(hr));
        stop();
        return false;
    }
    impl_->width = w;
    impl_->height = h;

    // Determine row stride / orientation. RGB32 from the source reader can be
    // bottom-up: a negative MF_MT_DEFAULT_STRIDE signals that. We always deliver
    // top-down BGRA to the consumer, flipping in software when needed.
    LONG default_stride = 0;
    bool have_stride = false;

    UINT32 raw_stride = 0;
    if (SUCCEEDED(actual->GetUINT32(MF_MT_DEFAULT_STRIDE, &raw_stride))) {
        default_stride = static_cast<LONG>(raw_stride);
        have_stride = true;
    } else {
        LONG computed = 0;
        if (SUCCEEDED(MFGetStrideForBitmapInfoHeader(
                MFVideoFormat_RGB32.Data1, w, &computed))) {
            default_stride = computed;
            have_stride = true;
        }
    }

    if (have_stride && default_stride < 0) {
        impl_->bottom_up = true;
        impl_->stride = static_cast<uint32_t>(-default_stride);
    } else if (have_stride && default_stride > 0) {
        impl_->bottom_up = false;
        impl_->stride = static_cast<uint32_t>(default_stride);
    } else {
        impl_->bottom_up = false;
        impl_->stride = w * 4;
    }

    if (impl_->stride < w * 4)
        impl_->stride = w * 4;

    if (impl_->bottom_up)
        impl_->flip_buffer.resize(static_cast<size_t>(impl_->stride) * h);

    LOG_INFO("webcam started: {}x{} stride={} {}",
             impl_->width, impl_->height, impl_->stride,
             impl_->bottom_up ? "bottom-up" : "top-down");

    // Capture worker thread
    impl_->running.store(true);
    impl_->worker = std::thread([this]() {
        Impl* impl = impl_.get();

        while (impl->running.load()) {
            DWORD stream_flags = 0;
            LONGLONG timestamp = 0;
            ComPtr<IMFSample> sample;

            HRESULT rhr = impl->reader->ReadSample(
                MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr,
                &stream_flags, &timestamp, &sample);

            if (FAILED(rhr)) {
                LOG_ERROR("ReadSample failed: {:#010x}", static_cast<unsigned>(rhr));
                break;
            }

            if (stream_flags & MF_SOURCE_READERF_ENDOFSTREAM)
                break;

            if (!sample)
                continue;

            ComPtr<IMFMediaBuffer> buffer;
            if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) || !buffer)
                continue;

            BYTE* data = nullptr;
            DWORD max_len = 0, cur_len = 0;
            if (FAILED(buffer->Lock(&data, &max_len, &cur_len)) || !data)
                continue;

            if (on_frame) {
                const uint32_t w = impl->width;
                const uint32_t h = impl->height;
                const uint32_t stride = impl->stride;

                if (impl->bottom_up) {
                    uint8_t* dst = impl->flip_buffer.data();
                    for (uint32_t row = 0; row < h; ++row) {
                        const uint8_t* src = data + static_cast<size_t>(h - 1 - row) * stride;
                        memcpy(dst + static_cast<size_t>(row) * stride, src, stride);
                    }
                    on_frame(impl->flip_buffer.data(), w, h, stride);
                } else {
                    on_frame(data, w, h, stride);
                }
            }

            buffer->Unlock();
        }

        impl->running.store(false);
    });

    return true;
}

void WebcamCapture::stop() {
    if (impl_->running.load()) {
        impl_->running.store(false);
        if (impl_->worker.joinable())
            impl_->worker.join();
    } else if (impl_->worker.joinable()) {
        impl_->worker.join();
    }

    impl_->reader.Reset();
    if (impl_->source) {
        impl_->source->Shutdown();
        impl_->source.Reset();
    }

    impl_->flip_buffer.clear();
    impl_->flip_buffer.shrink_to_fit();
    impl_->width = 0;
    impl_->height = 0;
    impl_->stride = 0;
    impl_->bottom_up = false;

    if (impl_->mf_held) {
        mf_release();
        impl_->mf_held = false;
    }
}

bool WebcamCapture::running() const {
    return impl_->running.load();
}

uint32_t WebcamCapture::width() const {
    return impl_->width;
}

uint32_t WebcamCapture::height() const {
    return impl_->height;
}

} // namespace parties::client
