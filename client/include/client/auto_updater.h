#pragma once

#include <atomic>
#include <string>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace parties::client {

class AutoUpdater {
public:
    enum class State : int {
        Idle,
        Checking,
        UpdateAvailable,
        Downloading,
        ReadyToInstall,
        Error
    };

    AutoUpdater();
    ~AutoUpdater();

    void start();
    void stop();

    bool poll();

    void download();

    void apply_and_restart();

    static bool handle_update_args(int argc, char** argv);

    State state() const { return static_cast<State>(display_state_.load(std::memory_order_relaxed)); }
    const std::string& latest_version() const { return display_version_; }
    int download_percent() const { return display_pct_.load(std::memory_order_relaxed); }

private:
    void thread_func();
    bool check_for_update();
    bool download_asset();
    bool verify_signature(const std::wstring& exe_path);
    bool parse_release_json(const std::string& json);
    static bool is_newer(const std::string& remote, const std::string& local);
    static std::string http_get(const std::wstring& host, const std::wstring& path, const wchar_t* accept = nullptr);
    static bool http_download(const std::wstring& url, const std::wstring& dest_path,
                              std::atomic<int>& pct_out, std::atomic<bool>& cancel);

    std::thread thread_;
    HANDLE wake_event_ = nullptr;

    std::atomic<int>  state_{static_cast<int>(State::Idle)};
    std::atomic<bool> state_changed_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> download_requested_{false};
    std::atomic<int>  download_pct_{0};
    std::atomic<bool> download_cancel_{false};

    std::string version_;
    std::string download_url_;
    std::wstring download_path_;

    std::atomic<int> display_state_{static_cast<int>(State::Idle)};
    std::string display_version_;
    std::atomic<int> display_pct_{0};
};

}
