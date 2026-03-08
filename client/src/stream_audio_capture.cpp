#include <client/stream_audio_capture.h>
#include <parties/profiler.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <cstdio>
#include <cstring>
#include <condition_variable>
#include <mutex>

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::FtmBase;

namespace parties::client {

// COM completion handler for ActivateAudioInterfaceAsync
class ActivationHandler
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IActivateAudioInterfaceCompletionHandler, FtmBase> {
public:
    ActivationHandler() = default;

    STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation* operation) override {
        HRESULT hr_activate = S_OK;
        ComPtr<IUnknown> unknown;

        HRESULT hr = operation->GetActivateResult(&hr_activate, &unknown);
        if (SUCCEEDED(hr) && SUCCEEDED(hr_activate) && unknown) {
            unknown.As(&audio_client_);
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            completed_ = true;
            result_ = SUCCEEDED(hr) ? hr_activate : hr;
        }
        cv_.notify_one();
        return S_OK;
    }

    bool wait(DWORD timeout_ms = 5000) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                            [this] { return completed_; });
    }

    HRESULT result() const { return result_; }
    ComPtr<IAudioClient> audio_client() const { return audio_client_; }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool completed_ = false;
    HRESULT result_ = E_FAIL;
    ComPtr<IAudioClient> audio_client_;
};

struct StreamAudioCapture::WasapiState {
    ComPtr<IAudioClient> audio_client;
    ComPtr<IAudioCaptureClient> capture_client;
    HANDLE audio_event = nullptr;
    WAVEFORMATEX* mix_format = nullptr;

    ~WasapiState() {
        if (audio_client) audio_client->Stop();
        if (mix_format) CoTaskMemFree(mix_format);
        if (audio_event) CloseHandle(audio_event);
    }
};

StreamAudioCapture::StreamAudioCapture() = default;

StreamAudioCapture::~StreamAudioCapture() {
    shutdown();
}

bool StreamAudioCapture::init(uint32_t target_pid) {
	ZoneScopedN("StreamAudioCapture::init");
    // Ensure COM is initialized on the calling thread (needed for IMMDeviceEnumerator)
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    if (!encoder_.init_encoder(kSampleRate, kChannels, 64000)) {
        std::fprintf(stderr, "[StreamAudioCapture] Failed to init Opus encoder\n");
        return false;
    }
    encoder_initialized_ = true;

    capture_buf_.resize(kFrameSize * kChannels, 0.0f);
    capture_pos_ = 0;

    // Build activation params for process loopback
    AUDIOCLIENT_ACTIVATION_PARAMS ac_params = {};
    ac_params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;

    if (target_pid != 0) {
        ac_params.ProcessLoopbackParams.TargetProcessId = target_pid;
        ac_params.ProcessLoopbackParams.ProcessLoopbackMode =
            PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    } else {
        ac_params.ProcessLoopbackParams.TargetProcessId = GetCurrentProcessId();
        ac_params.ProcessLoopbackParams.ProcessLoopbackMode =
            PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;
    }

    PROPVARIANT activate_params = {};
    activate_params.vt = VT_BLOB;
    activate_params.blob.cbSize = sizeof(ac_params);
    activate_params.blob.pBlobData = reinterpret_cast<BYTE*>(&ac_params);

    auto handler = Microsoft::WRL::Make<ActivationHandler>();
    ComPtr<IActivateAudioInterfaceAsyncOperation> async_op;

    HRESULT hr = ActivateAudioInterfaceAsync(
        VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
        __uuidof(IAudioClient),
        &activate_params,
        handler.Get(),
        &async_op);

    if (FAILED(hr)) {
        std::fprintf(stderr, "[StreamAudioCapture] ActivateAudioInterfaceAsync failed: 0x%08lX\n", hr);
        return false;
    }

    if (!handler->wait(5000)) {
        std::fprintf(stderr, "[StreamAudioCapture] Activation timed out\n");
        return false;
    }

    if (FAILED(handler->result())) {
        std::fprintf(stderr, "[StreamAudioCapture] Activation failed: 0x%08lX\n", handler->result());
        return false;
    }

    auto state = new WasapiState();
    state->audio_client = handler->audio_client();

    // Request exactly the format Opus needs: 48kHz stereo float32.
    // AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM tells WASAPI to convert from the
    // system mix format (which may be 44100 Hz, 7.1 surround, etc.)
    WAVEFORMATEX* fmt = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
    fmt->wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    fmt->nChannels = kChannels;
    fmt->nSamplesPerSec = kSampleRate;
    fmt->wBitsPerSample = 32;
    fmt->nBlockAlign = fmt->nChannels * fmt->wBitsPerSample / 8;
    fmt->nAvgBytesPerSec = fmt->nSamplesPerSec * fmt->nBlockAlign;
    fmt->cbSize = 0;
    state->mix_format = fmt;

    std::printf("[StreamAudioCapture] Requested format: %lu Hz, %d ch, %d bits (float)\n",
                fmt->nSamplesPerSec, fmt->nChannels, fmt->wBitsPerSample);

    // Create event for buffer-ready notification
    state->audio_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!state->audio_event) {
        std::fprintf(stderr, "[StreamAudioCapture] CreateEvent failed\n");
        delete state;
        return false;
    }

    // Initialize in shared mode with automatic format conversion
    hr = state->audio_client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
        200000,  // 20ms in 100ns units
        0,       // periodicity (must be 0 for shared mode)
        state->mix_format,
        nullptr);

    if (FAILED(hr)) {
        std::fprintf(stderr, "[StreamAudioCapture] IAudioClient::Initialize failed: 0x%08lX\n", hr);
        delete state;
        return false;
    }

    hr = state->audio_client->SetEventHandle(state->audio_event);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[StreamAudioCapture] SetEventHandle failed: 0x%08lX\n", hr);
        delete state;
        return false;
    }

    hr = state->audio_client->GetService(__uuidof(IAudioCaptureClient),
                                          reinterpret_cast<void**>(state->capture_client.GetAddressOf()));
    if (FAILED(hr)) {
        std::fprintf(stderr, "[StreamAudioCapture] GetService(IAudioCaptureClient) failed: 0x%08lX\n", hr);
        delete state;
        return false;
    }

    wasapi_ = state;

    std::printf("[StreamAudioCapture] Loopback: %s PID %u (%lu Hz, %d ch)\n",
                target_pid != 0 ? "include" : "exclude-self",
                target_pid != 0 ? target_pid : static_cast<uint32_t>(GetCurrentProcessId()),
                state->mix_format->nSamplesPerSec, state->mix_format->nChannels);
    return true;
}

void StreamAudioCapture::shutdown() {
    stop();
    delete wasapi_;
    wasapi_ = nullptr;
    encoder_initialized_ = false;
}

bool StreamAudioCapture::start() {
    if (!wasapi_ || running_) return false;

    capture_pos_ = 0;

    HRESULT hr = wasapi_->audio_client->Start();
    if (FAILED(hr)) {
        std::fprintf(stderr, "[StreamAudioCapture] IAudioClient::Start failed: 0x%08lX\n", hr);
        return false;
    }

    running_ = true;
    capture_thread_ = std::thread(&StreamAudioCapture::capture_thread_func, this);
    return true;
}

void StreamAudioCapture::stop() {
    running_ = false;
    if (capture_thread_.joinable()) {
        if (wasapi_ && wasapi_->audio_event)
            SetEvent(wasapi_->audio_event);
        capture_thread_.join();
    }
    if (wasapi_ && wasapi_->audio_client)
        wasapi_->audio_client->Stop();
}

void StreamAudioCapture::capture_thread_func() {
	TracySetThreadName("StreamAudioCapture");
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    const int mix_channels = wasapi_->mix_format->nChannels;
    const bool is_float = (wasapi_->mix_format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
        (wasapi_->mix_format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
         wasapi_->mix_format->cbSize >= 22);

    const int samples_per_frame = kFrameSize * kChannels;

    while (running_) {
        ZoneScopedN("StreamAudioCapture::capture_thread_func");
        DWORD wait_result = WaitForSingleObject(wasapi_->audio_event, 100);
        if (!running_) break;
        if (wait_result != WAIT_OBJECT_0) continue;

        UINT32 packet_length = 0;
        while (SUCCEEDED(wasapi_->capture_client->GetNextPacketSize(&packet_length)) && packet_length > 0) {
            BYTE* data = nullptr;
            UINT32 frames_available = 0;
            DWORD flags = 0;

            HRESULT hr = wasapi_->capture_client->GetBuffer(&data, &frames_available, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;

            bool is_silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;

            for (UINT32 i = 0; i < frames_available; i++) {
                float left = 0.0f, right = 0.0f;

                if (!is_silent && data) {
                    if (is_float) {
                        const float* samples = reinterpret_cast<const float*>(data) + i * mix_channels;
                        left = samples[0];
                        right = mix_channels >= 2 ? samples[1] : samples[0];
                    } else {
                        const int16_t* samples = reinterpret_cast<const int16_t*>(data) + i * mix_channels;
                        left = samples[0] / 32768.0f;
                        right = mix_channels >= 2 ? samples[1] / 32768.0f : left;
                    }
                }

                capture_buf_[capture_pos_++] = left;
                capture_buf_[capture_pos_++] = right;

                if (capture_pos_ >= static_cast<size_t>(samples_per_frame)) {
                    if (encoder_initialized_ && on_encoded_frame) {
                        int encoded = encoder_.encode(capture_buf_.data(), kFrameSize,
                                                       opus_buf_, audio::MAX_OPUS_PACKET);
                        if (encoded > 0)
                            on_encoded_frame(opus_buf_, static_cast<size_t>(encoded));
                    }
                    capture_pos_ = 0;
                }
            }

            wasapi_->capture_client->ReleaseBuffer(frames_available);
        }
    }

    CoUninitialize();
}

} // namespace parties::client
