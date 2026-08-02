#include <client/screen_capture.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dwmapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>

namespace {

LRESULT CALLBACK ThumbnailWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        RECT left = client;
        left.right = (client.left + client.right) / 2;
        RECT right = client;
        right.left = left.right;
        HBRUSH red = CreateSolidBrush(RGB(232, 48, 54));
        HBRUSH green = CreateSolidBrush(RGB(36, 214, 142));
        FillRect(dc, &left, red);
        FillRect(dc, &right, green);
        DeleteObject(red);
        DeleteObject(green);
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_DESTROY)
    {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

constexpr wchar_t kClassName[] = L"PartiesThumbnailCaptureTest";
constexpr wchar_t kWindowTitle[] = L"Parties cross-process application thumbnail test";

bool RegisterThumbnailWindowClass(HINSTANCE instance) {
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = ThumbnailWindowProc;
    window_class.hInstance = instance;
    window_class.lpszClassName = kClassName;
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    return RegisterClassW(&window_class) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

int RunThumbnailWindowChild() {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    if (!RegisterThumbnailWindowClass(instance))
        return 2;

    HWND window = CreateWindowExW(
        0, kClassName, kWindowTitle,
        WS_OVERLAPPEDWINDOW, 80, 80, 480, 300,
        nullptr, nullptr, instance, nullptr);
    if (!window)
        return 3;

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

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--thumbnail-child")
        return RunThumbnailWindowChild();

    wchar_t executable[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, executable, MAX_PATH) == 0) {
        std::cerr << "GetModuleFileNameW failed\n";
        return 1;
    }

    std::wstring command = L"\"" + std::wstring(executable) + L"\" --thumbnail-child";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, &startup, &process)) {
        std::cerr << "CreateProcessW failed\n";
        return 1;
    }
    CloseHandle(process.hThread);

    HWND window = nullptr;
    for (int attempt = 0; attempt < 100 && !window; ++attempt) {
        window = FindWindowW(kClassName, kWindowTitle);
        if (!window)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!window) {
        TerminateProcess(process.hProcess, 4);
        CloseHandle(process.hProcess);
        std::cerr << "Child thumbnail window did not appear\n";
        return 1;
    }

    parties::client::CaptureTarget target;
    target.type = parties::client::CaptureTarget::Type::Window;
    target.name = "thumbnail test";
    target.handle = window;
    // Match the production picker: a bounded set of workers, each reusing its
    // own session for more than one application capture.
    std::array<parties::client::CaptureThumbnail, 4> thumbnails;
    std::array<std::jthread, 4> capture_workers;
    for (size_t worker = 0; worker < capture_workers.size(); ++worker) {
        capture_workers[worker] = std::jthread([&, worker] {
            parties::client::ScreenCapture::ThumbnailSession session;
            for (int capture = 0; capture < 2; ++capture) {
                auto next = session.capture(target, 240, 135);
                if (!next) {
                    thumbnails[worker] = {};
                    return;
                }
                thumbnails[worker] = std::move(next);
            }
        });
    }
    for (auto& worker : capture_workers)
        worker.join();

    PostMessageW(window, WM_CLOSE, 0, 0);
    if (WaitForSingleObject(process.hProcess, 3000) != WAIT_OBJECT_0)
        TerminateProcess(process.hProcess, 5);
    CloseHandle(process.hProcess);

    for (const auto& candidate : thumbnails) {
        if (!candidate) {
            std::cerr << "Concurrent window thumbnail is empty\n";
            return 1;
        }
    }
    const auto& thumbnail = thumbnails.front();
    if (thumbnail.width > 240 || thumbnail.height > 135 ||
        thumbnail.rgba.size() != static_cast<size_t>(thumbnail.width) * thumbnail.height * 4) {
        std::cerr << "Window thumbnail dimensions are invalid\n";
        return 1;
    }
    if (thumbnail.source != parties::client::CaptureThumbnail::Source::WindowsGraphicsCapture) {
        std::cerr << "Modern WGC path was not used\n";
        return 1;
    }

    size_t red_pixels = 0;
    size_t green_pixels = 0;
    uint8_t maximum_red = 0;
    uint8_t maximum_green = 0;
    uint8_t maximum_blue = 0;
    uint64_t red_sum = 0;
    uint64_t green_sum = 0;
    uint64_t blue_sum = 0;
    for (size_t pixel = 0; pixel < thumbnail.rgba.size(); pixel += 4) {
        const uint8_t red = thumbnail.rgba[pixel + 0];
        const uint8_t green = thumbnail.rgba[pixel + 1];
        const uint8_t blue = thumbnail.rgba[pixel + 2];
        maximum_red = (std::max)(maximum_red, red);
        maximum_green = (std::max)(maximum_green, green);
        maximum_blue = (std::max)(maximum_blue, blue);
        red_sum += red;
        green_sum += green;
        blue_sum += blue;
        if (red > 150 && red > green * 2 && red > blue * 2)
            ++red_pixels;
        if (green > 130 && green > red * 2 && green > blue + 40)
            ++green_pixels;
    }
    const size_t pixel_count = static_cast<size_t>(thumbnail.width) * thumbnail.height;
    if (red_pixels < pixel_count / 20 || green_pixels < pixel_count / 20) {
        std::cerr << "Captured pixels do not contain the rendered window content: "
                  << "max=" << static_cast<int>(maximum_red) << ','
                  << static_cast<int>(maximum_green) << ','
                  << static_cast<int>(maximum_blue) << " avg="
                  << red_sum / pixel_count << ',' << green_sum / pixel_count << ','
                  << blue_sum / pixel_count << " red=" << red_pixels
                  << " green=" << green_pixels << '\n';
        return 1;
    }

    std::cout << "WGC window thumbnail " << thumbnail.width << 'x' << thumbnail.height
              << ", red=" << red_pixels << ", green=" << green_pixels << '\n';
    return 0;
}
