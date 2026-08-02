#include <client/win32_window_lifecycle.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

namespace {

std::atomic<DWORD> destroyed_on_thread{0};
std::atomic<DWORD> resized_on_thread{0};
std::atomic<int> resized_width{0};
std::atomic<int> resized_height{0};

LRESULT CALLBACK test_window_proc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) {
    if (parties::client::OwnerThreadWindowDestroy::handle_message(hwnd, message))
        return 0;
    if (parties::client::OwnerThreadWindowResize::handle_message(
            hwnd, message, w_param, l_param)) {
        RECT rect{};
        GetWindowRect(hwnd, &rect);
        resized_width.store(rect.right - rect.left, std::memory_order_release);
        resized_height.store(rect.bottom - rect.top, std::memory_order_release);
        resized_on_thread.store(GetCurrentThreadId(), std::memory_order_release);
        return 0;
    }
    if (message == WM_DESTROY)
        destroyed_on_thread.store(GetCurrentThreadId(), std::memory_order_release);
    return DefWindowProcW(hwnd, message, w_param, l_param);
}

} // namespace

int main() {
    const DWORD owner_thread = GetCurrentThreadId();
    constexpr wchar_t class_name[] = L"PartiesContextWindowLifecycleTest";

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpfnWndProc = test_window_proc;
    window_class.lpszClassName = class_name;
    if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        std::fprintf(stderr, "failed to register lifecycle test window\n");
        return 1;
    }

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, class_name, L"", WS_POPUP,
        0, 0, 64, 64, nullptr, nullptr, window_class.hInstance, nullptr);
    if (!hwnd) {
        std::fprintf(stderr, "failed to create lifecycle test window\n");
        return 1;
    }

    std::atomic<bool> resize_requested{false};
    std::thread render_resize_thread([hwnd, &resize_requested] {
        resize_requested.store(
            parties::client::OwnerThreadWindowResize::request(hwnd, 96, 80),
            std::memory_order_release);
    });

    const auto resize_deadline = std::chrono::steady_clock::now() +
                                 std::chrono::milliseconds(250);
    while (!resize_requested.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < resize_deadline) {
        std::this_thread::yield();
    }
    if (!resize_requested.load(std::memory_order_acquire)) {
        // Pump once so a regressed synchronous cross-thread SetWindowPos can
        // unwind and the test can report the failure instead of hanging CTest.
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            DispatchMessageW(&message);
        render_resize_thread.join();
        std::fprintf(stderr, "cross-thread resize request blocked\n");
        return 1;
    }
    render_resize_thread.join();

    const auto resized_deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(2);
    while (resized_on_thread.load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < resized_deadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        std::this_thread::yield();
    }
    if (resized_on_thread.load(std::memory_order_acquire) != owner_thread ||
        resized_width.load(std::memory_order_acquire) != 96 ||
        resized_height.load(std::memory_order_acquire) != 80) {
        std::fprintf(stderr, "popup resize was not applied on the HWND owner thread\n");
        return 1;
    }

    const auto request_started = std::chrono::steady_clock::now();
    std::thread render_thread([hwnd] {
        parties::client::OwnerThreadWindowDestroy::destroy(hwnd);
    });
    render_thread.join();
    const auto request_elapsed = std::chrono::steady_clock::now() - request_started;
    if (request_elapsed > std::chrono::milliseconds(250)) {
        std::fprintf(stderr, "cross-thread destroy request blocked\n");
        return 1;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (IsWindow(hwnd) && std::chrono::steady_clock::now() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        std::this_thread::yield();
    }

    if (IsWindow(hwnd)) {
        std::fprintf(stderr, "owner thread did not destroy posted HWND\n");
        return 1;
    }
    if (destroyed_on_thread.load(std::memory_order_acquire) != owner_thread) {
        std::fprintf(stderr, "DestroyWindow ran on the wrong thread\n");
        return 1;
    }
    return 0;
}
