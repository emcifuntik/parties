#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace parties::client {

class Win32SingleInstance {
public:
    enum class Result { Primary, Activated, Error };

    Win32SingleInstance() = default;
    Win32SingleInstance(const Win32SingleInstance&) = delete;
    Win32SingleInstance& operator=(const Win32SingleInstance&) = delete;

    ~Win32SingleInstance() {
        if (owned_) ReleaseMutex(mutex_);
        if (mutex_) CloseHandle(mutex_);
        if (activation_) CloseHandle(activation_);
    }

    Result acquire(const wchar_t* mutex_name = L"Local\\Parties.Client.Instance",
                   const wchar_t* event_name = L"Local\\Parties.Client.Activate",
                   const wchar_t* window_class = L"PartiesClient") {
        activation_ = CreateEventW(nullptr, TRUE, FALSE, event_name);
        if (!activation_) return Result::Error;
        mutex_ = CreateMutexW(nullptr, FALSE, mutex_name);
        if (!mutex_) return Result::Error;
        const DWORD result = WaitForSingleObject(mutex_, 0);
        if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) {
            owned_ = true;
            return Result::Primary;
        }
        if (result != WAIT_TIMEOUT) return Result::Error;
        if (HWND hwnd = FindWindowW(window_class, nullptr)) {
            DWORD process_id = 0;
            GetWindowThreadProcessId(hwnd, &process_id);
            if (process_id) AllowSetForegroundWindow(process_id);
        }
        return SetEvent(activation_) ? Result::Activated : Result::Error;
    }

    HANDLE activation_event() const { return activation_; }

    void restore_if_requested(HWND hwnd) const {
        if (WaitForSingleObject(activation_, 0) == WAIT_OBJECT_0) {
            ResetEvent(activation_);
            ShowWindow(hwnd, IsIconic(hwnd) ? SW_RESTORE : SW_SHOW);
            SetForegroundWindow(hwnd);
        }
    }

private:
    HANDLE mutex_ = nullptr;
    HANDLE activation_ = nullptr;
    bool owned_ = false;
};

}
