#pragma once

// Webcam capture on Windows through Media Foundation's Source Reader.
//
// The device is opened in the native format closest to the requested size at
// (up to) the requested frame rate; the reader decodes MJPEG / converts YUY2
// and NV12 to BGRA and scales down (aspect preserved, never up) to fit the
// requested box. With a DXGI device manager that work runs on the GPU and the
// frames arrive as textures of this capture's own D3D11 device, which is also
// the device the encoder must be created on; drivers that refuse it fall back
// to system-memory frames that are uploaded to the same texture.
//
// Threading: start() blocks while the device opens (seconds on some cameras:
// call it off the UI thread). Frames are delivered on the capture's own worker
// thread, newest first (a frame the worker has not consumed yet is replaced,
// never queued). stop() is synchronous and leaves nothing running.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct ID3D11Device;
struct ID3D11Texture2D;

namespace parties::client {

struct CameraDevice {
    std::string  name;            // friendly name, UTF-8
    std::wstring symbolic_link;   // stable device id for MFCreateDeviceSource
};

class CameraCapture {
public:
    struct Config {
        uint32_t max_width  = 1280;
        uint32_t max_height = 720;
        uint32_t fps        = 30;
    };
    // BGRA (DXGI_FORMAT_B8G8R8A8_UNORM) texture of device(); valid only for
    // the duration of the call. timestamp in 100 ns units, monotonic.
    using FrameFn = std::function<void(ID3D11Texture2D* bgra, uint32_t width, uint32_t height,
                                       int64_t timestamp_100ns)>;
    // The camera went away or failed mid-capture (unplugged, taken by
    // another application, driver error). Called once, on the worker thread.
    using ErrorFn = std::function<void(const std::string& reason)>;

    // Video capture devices currently present, in a stable order.
    static std::vector<CameraDevice> enumerate();

    CameraCapture();
    ~CameraCapture();
    CameraCapture(const CameraCapture&) = delete;
    CameraCapture& operator=(const CameraCapture&) = delete;

    bool start(const CameraDevice& device, const Config& config, FrameFn on_frame, ErrorFn on_error);
    void stop();

    ID3D11Device* device() const;
    uint32_t width() const;    // delivered frame size (after scaling)
    uint32_t height() const;
    uint32_t fps() const;      // frame rate the device was opened at

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace parties::client
