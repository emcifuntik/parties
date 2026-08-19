#include <client/win32_power_request.h>

#include <iostream>

using parties::client::Win32PowerRequest;

namespace {

// The power request is a kernel handle, so a leak shows up in the process
// handle count. The count is stable in this single-threaded test process, but
// the loader may still allocate a handle or two in the background, so compare
// with a small tolerance rather than for exact equality.
constexpr DWORD kHandleSlack = 4;

DWORD handle_count() {
    DWORD count = 0;
    if (!GetProcessHandleCount(GetCurrentProcess(), &count)) {
        std::cerr << "GetProcessHandleCount failed: " << GetLastError() << "\n";
        return 0;
    }
    return count;
}

} // namespace

int main() {
    const DWORD baseline = handle_count();
    if (baseline == 0) return 1;

    {
        Win32PowerRequest request;
        if (request.held()) {
            std::cerr << "request reported as held before being acquired\n";
            return 1;
        }

        request.set(true);
        if (!request.held()) {
            std::cerr << "request not held after acquiring\n";
            return 1;
        }
        if (!request.using_power_request()) {
            // Windows 7 and later always provide PowerCreateRequest; falling
            // back here means the modern path broke, not that the OS is old.
            std::cerr << "PowerCreateRequest unavailable — skipping\n";
            return 77;
        }

        // The refresh path re-issues the request rather than creating another
        // one; a call every 30 seconds would otherwise leak a handle a minute.
        const DWORD after_acquire = handle_count();
        for (int i = 0; i < 100; ++i)
            request.set(true);
        if (handle_count() > after_acquire) {
            std::cerr << "refreshing the request allocated additional handles\n";
            return 1;
        }

        request.set(false);
        if (request.held()) {
            std::cerr << "request still held after release\n";
            return 1;
        }
        // Releasing twice must not double-close the handle.
        request.set(false);

        // Join/leave cycles: a call per channel switch must not accumulate.
        for (int i = 0; i < 200; ++i) {
            request.set(true);
            request.set(false);
        }
        if (handle_count() > baseline + kHandleSlack) {
            std::cerr << "handles leaked across acquire/release cycles: "
                      << baseline << " -> " << handle_count() << "\n";
            return 1;
        }

        // Left held on purpose — the destructor has to clear it, which is what
        // keeps a crash or an early exit from leaving the machine awake.
        request.set(true);
    }

    if (handle_count() > baseline + kHandleSlack) {
        std::cerr << "destructor did not release the power request: "
                  << baseline << " -> " << handle_count() << "\n";
        return 1;
    }

    std::cout << "Win32 call power request passed\n";
    return 0;
}
