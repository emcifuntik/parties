#include <client/screen_capture.h>
#include <roapi.h>
#include <dwmapi.h>

#include <atomic>
#include <cstdio>
#include <future>
#include <string>
#include <thread>

namespace {
constexpr wchar_t kClass[] = L"PartiesCaptureLifecycleTest";
LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcW(window, message, wparam, lparam);
}

int run_source_window() {
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = kClass;
    if (!RegisterClassW(&window_class)) return 1;
    HWND window = CreateWindowExW(0, kClass, L"Capture source", WS_OVERLAPPEDWINDOW,
        -10000, -10000, 320, 200, nullptr, nullptr, window_class.hInstance, nullptr);
    if (!window) return 2;
    ShowWindow(window, SW_SHOWNOACTIVATE);
    UpdateWindow(window);
    DwmFlush();
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}

struct SourceWindow {
    HWND handle = nullptr;
    PROCESS_INFORMATION process{};
    SourceWindow() {
        wchar_t executable[MAX_PATH]{};
        if (!GetModuleFileNameW(nullptr, executable, MAX_PATH)) return;
        std::wstring command = L"\"" + std::wstring(executable) + L"\" --capture-source";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                            nullptr, nullptr, &startup, &process)) return;
        CloseHandle(process.hThread);
        process.hThread = nullptr;
        for (int attempt = 0; attempt < 100 && !handle; ++attempt) {
            EnumWindows([](HWND window, LPARAM data) -> BOOL {
                auto& source = *reinterpret_cast<SourceWindow*>(data);
                DWORD process_id = 0;
                GetWindowThreadProcessId(window, &process_id);
                wchar_t class_name[128]{};
                GetClassNameW(window, class_name, 128);
                if (process_id == source.process.dwProcessId &&
                    std::wstring(class_name) == kClass && IsWindowVisible(window)) {
                    source.handle = window;
                    return FALSE;
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(this));
            if (!handle) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    bool close() {
        if (!process.hProcess) return true;
        if (handle) PostMessageW(handle, WM_CLOSE, 0, 0);
        const bool exited = WaitForSingleObject(process.hProcess, 3000) == WAIT_OBJECT_0;
        if (!exited) TerminateProcess(process.hProcess, 3);
        CloseHandle(process.hProcess);
        process.hProcess = nullptr;
        handle = nullptr;
        return exited;
    }
    ~SourceWindow() { close(); }
};
bool check(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "Capture lifecycle: %s\n", message);
    return condition;
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--capture-source") return run_source_window();
    using namespace parties::client;
    const HRESULT apartment = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(apartment)) return 77;
    bool passed = true;
    {
        for (int iteration = 0; iteration < 4; ++iteration) {
            auto capture = std::make_shared<ScreenCapture>();
            if (!capture->init()) return 77;
            // WGC requires a shown window. Keep the source offscreen and never
            // activate it. A child process matches a captured application exit.
            SourceWindow source;
            const HWND window = source.handle;
            if (!window) return 2;
            CaptureTarget target{CaptureTarget::Type::Window, "Capture source", window};
            // Match Parties: activate WGC on a temporary MTA startup worker.
            const bool started = std::async(std::launch::async, [&] {
                const HRESULT worker_apartment = RoInitialize(RO_INIT_MULTITHREADED);
                if (FAILED(worker_apartment)) return false;
                const bool result = capture->start(target, 30);
                RoUninitialize();
                return result;
            }).get();
            if (!check(started, "capture did not start")) return 3;
            passed &= check(capture->is_capturing() && !capture->target_lost(), "live source was lost");
            SetWindowPos(window, nullptr, 0, 0, 400, 240, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            passed &= check(!capture->target_lost(), "resizing a live source ended capture");
            std::atomic<int> frames{0};
            capture->set_frame_callback([&](ID3D11Texture2D*, uint32_t, uint32_t) { ++frames; });
            // Do not wait for process teardown: stop as soon as its HWND dies,
            // while the source may still be running its remaining cleanup.
            DWORD_PTR result = 0;
            passed &= check(SendMessageTimeoutW(window, WM_CLOSE, 0, 0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 3000, &result) != 0, "source window did not close");
            if (!IsWindow(window)) source.handle = nullptr;
            // Check immediately, without pumping a Closed notification or
            // waiting for another frame from the now-dead application window.
            passed &= check(capture->target_lost(), "closed source was not detected");
            auto stopped = ScreenCapture::shutdown_async(capture);
            passed &= check(!capture->is_capturing(), "frame delivery remained active during cleanup");
            const int stopped_frames = frames.load();
            while (stopped.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready) {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            }
            stopped.get();
            passed &= check(!capture->is_capturing(), "capture remained active after stop");
            capture->stop();
            passed &= check(source.close(), "source process did not exit");
            passed &= check(frames.load() == stopped_frames, "callback ran after asynchronous shutdown");
            if (!capture->init()) return 77;
            passed &= check(!capture->start(target, 30), "destroyed source was restarted");
            capture->shutdown();
        }
    }
    // Keep the main apartment alive through process teardown, including WGC's
    // asynchronous internal cleanup and module-static WinRT factory references.
    return passed ? 0 : 1;
}
