#include <client/win32_single_instance.h>

#include <cstdio>
#include <string>

using parties::client::Win32SingleInstance;

bool check(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "%s\n", message);
    return condition;
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 3) {
        Win32SingleInstance instance;
        return instance.acquire(argv[1], argv[2], L"PartiesSingleInstanceTest") ==
            Win32SingleInstance::Result::Activated ? 0 : 1;
    }

    const std::wstring prefix = L"Local\\Parties.SingleInstanceTest." +
        std::to_wstring(GetCurrentProcessId());
    const std::wstring mutex_name = prefix + L".Mutex";
    const std::wstring event_name = prefix + L".Activate";
    bool passed = true;
    {
        Win32SingleInstance primary;
        passed &= check(primary.acquire(mutex_name.c_str(), event_name.c_str()) ==
            Win32SingleInstance::Result::Primary, "First launch must own the instance");

        wchar_t executable[MAX_PATH]{};
        GetModuleFileNameW(nullptr, executable, MAX_PATH);
        std::wstring command = L"\"" + std::wstring(executable) + L"\" \"" +
            mutex_name + L"\" \"" + event_name + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!check(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                                  CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process),
                   "Could not launch second process")) return 1;
        const DWORD wait = WaitForSingleObject(process.hProcess, 5000);
        DWORD exit_code = 1;
        GetExitCodeProcess(process.hProcess, &exit_code);
        passed &= check(wait == WAIT_OBJECT_0 && exit_code == 0,
                        "Second launch must request activation and exit");
        if (wait != WAIT_OBJECT_0) TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);

        HANDLE event = primary.activation_event();
        passed &= check(MsgWaitForMultipleObjectsEx(1, &event, 0, QS_ALLINPUT,
                            MWMO_INPUTAVAILABLE) == WAIT_OBJECT_0,
                        "Activation before window creation must wake the message loop");
        HWND hwnd = CreateWindowExW(0, L"STATIC", L"Single instance test",
            WS_OVERLAPPEDWINDOW, 0, 0, 320, 200, nullptr, nullptr, nullptr, nullptr);
        if (!check(hwnd != nullptr, "Could not create test window")) return 1;
        primary.restore_if_requested(hwnd);
        passed &= check(IsWindowVisible(hwnd) && !IsIconic(hwnd),
                        "Pending launch must show a hidden window");
        passed &= check(WaitForSingleObject(event, 0) == WAIT_TIMEOUT,
                        "Activation must be consumed once");

        ShowWindow(hwnd, SW_MINIMIZE);
        SetEvent(event);
        primary.restore_if_requested(hwnd);
        passed &= check(IsWindowVisible(hwnd) && !IsIconic(hwnd),
                        "Relaunch must restore a minimized window");

        ShowWindow(hwnd, SW_MAXIMIZE);
        SetEvent(event);
        primary.restore_if_requested(hwnd);
        passed &= check(IsZoomed(hwnd), "Relaunch must preserve maximization");
        ShowWindow(hwnd, SW_HIDE);
        SetEvent(event);
        primary.restore_if_requested(hwnd);
        passed &= check(IsWindowVisible(hwnd) && IsZoomed(hwnd),
                        "Relaunch must restore a tray-hidden maximized window");
        DestroyWindow(hwnd);
    }
    Win32SingleInstance restarted;
    passed &= check(restarted.acquire(mutex_name.c_str(), event_name.c_str()) ==
        Win32SingleInstance::Result::Primary, "Client must start again after shutdown");
    return passed ? 0 : 1;
}
