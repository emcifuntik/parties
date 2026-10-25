#pragma once

#include <atomic>
#include <filesystem>
#include <string>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace parties::client {

enum class UpdateLaunchMode {
    Normal,
    CleanupAfterRename,
    // "--update-replace <old>" is produced only by updaters older than commit
    // 62571ef, which launched parties_client_new.exe. Kept so those clients can
    // still finish updating.
    LegacyReplace,
};

struct UpdateLaunch {
    UpdateLaunchMode mode = UpdateLaunchMode::Normal;
    std::filesystem::path other_exe;
};

UpdateLaunch parse_update_launch(int argc, char** argv);

enum class UpdateLaunchResult { Continue, ExitWithFailure };

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

    static UpdateLaunchResult finish_update_launch(const UpdateLaunch& launch);

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
    static void finish_cleanup_after_rename(const std::filesystem::path& old_exe);
    static UpdateLaunchResult run_legacy_replace_migration(const std::filesystem::path& old_exe);
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
