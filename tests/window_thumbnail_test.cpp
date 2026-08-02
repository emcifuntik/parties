#include <client/screen_capture.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dwmapi.h>

#include <algorithm>
#include <cstdint>
#include <iostream>
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
        return 0;
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

int main() {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    const wchar_t* class_name = L"PartiesThumbnailCaptureTest";
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = ThumbnailWindowProc;
    window_class.hInstance = instance;
    window_class.lpszClassName = class_name;
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    if (!RegisterClassW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        std::cerr << "RegisterClassW failed\n";
        return 1;
    }

    HWND window = CreateWindowExW(
        0, class_name, L"Parties application thumbnail test",
        WS_OVERLAPPEDWINDOW, 80, 80, 480, 300,
        nullptr, nullptr, instance, nullptr);
    if (!window) {
        std::cerr << "CreateWindowExW failed\n";
        return 1;
    }

    ShowWindow(window, SW_SHOWNOACTIVATE);
    UpdateWindow(window);
    DwmFlush();

    parties::client::CaptureTarget target;
    target.type = parties::client::CaptureTarget::Type::Window;
    target.name = "thumbnail test";
    target.handle = window;
    parties::client::CaptureThumbnail thumbnail;
    std::jthread capture_worker([&] {
        // Match the production lifecycle: GPU/WinRT preview resources are
        // created, used, and destroyed inside the worker before it is joined.
        parties::client::ScreenCapture::ThumbnailSession session;
        thumbnail = session.capture(target, 240, 135);
    });
    capture_worker.join();

    DestroyWindow(window);
    UnregisterClassW(class_name, instance);

    if (!thumbnail) {
        std::cerr << "Window thumbnail is empty\n";
        return 1;
    }
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
