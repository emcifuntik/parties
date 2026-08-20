#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace parties::client {

class Win32PowerRequest {
public:
    Win32PowerRequest() = default;
    ~Win32PowerRequest() { set(false); }

    Win32PowerRequest(const Win32PowerRequest&) = delete;
    Win32PowerRequest& operator=(const Win32PowerRequest&) = delete;

    void set(bool active) noexcept {
        if (active)
            acquire();
        else
            release();
    }

    bool held() const noexcept { return request_ != nullptr || legacy_; }

    // False while held means the power request could not be created and the
    // legacy fallback is carrying the inhibit.
    bool using_power_request() const noexcept { return request_ != nullptr; }

private:
    static constexpr DWORD legacy_flags =
        ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED;

    void acquire() noexcept {
        // Re-issuing an already-held request is how a refresh restores one that
        // a power transition dropped; when nothing was dropped it does nothing.
        if (request_ != nullptr) {
            PowerSetRequest(request_, PowerRequestDisplayRequired);
            PowerSetRequest(request_, PowerRequestSystemRequired);
            return;
        }
        if (legacy_) {
            SetThreadExecutionState(legacy_flags);
            return;
        }

        REASON_CONTEXT reason = {};
        reason.Version = POWER_REQUEST_CONTEXT_VERSION;
        reason.Flags   = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
        // The API takes a non-const string it never writes to.
        reason.Reason.SimpleReasonString = const_cast<LPWSTR>(L"Voice call in progress");

        HANDLE request = PowerCreateRequest(&reason);
        if (request == nullptr || request == INVALID_HANDLE_VALUE) {
            legacy_ = true;
            SetThreadExecutionState(legacy_flags);
            return;
        }

        PowerSetRequest(request, PowerRequestDisplayRequired);
        PowerSetRequest(request, PowerRequestSystemRequired);
        request_ = request;
    }

    void release() noexcept {
        if (request_ != nullptr) {
            PowerClearRequest(request_, PowerRequestDisplayRequired);
            PowerClearRequest(request_, PowerRequestSystemRequired);
            CloseHandle(request_);
            request_ = nullptr;
        }
        if (legacy_) {
            // Back to plain ES_CONTINUOUS: the idle timers restart from now.
            SetThreadExecutionState(ES_CONTINUOUS);
            legacy_ = false;
        }
    }

    HANDLE request_ = nullptr;
    bool   legacy_  = false;
};

} // namespace parties::client
