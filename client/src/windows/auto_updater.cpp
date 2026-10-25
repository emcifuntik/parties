#include <client/auto_updater.h>
#include <parties/version.h>
#include <parties/log.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <winhttp.h>
#include <wintrust.h>
#include <softpub.h>
#include <shellapi.h>

#include <rapidjson/document.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

namespace parties::client {

static constexpr int CHECK_INTERVAL_SECONDS = 15 * 60;
static constexpr const char* GITHUB_OWNER = "emcifuntik";
static constexpr const char* GITHUB_REPO  = "parties";
static constexpr const wchar_t* USER_AGENT = L"Parties-Updater/1.0";

static std::wstring to_wide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), len);
    return w;
}

struct ReleaseInfo {
    std::string tag_name;
    std::string asset_url;
    bool prerelease = false;
};

static bool parse_github_release(const std::string& json, const std::string& expected_asset_prefix, ReleaseInfo& out) {
    rapidjson::Document doc;
    doc.Parse(json.c_str(), json.size());
    if (doc.HasParseError() || !doc.IsObject()) return false;

    if (doc.HasMember("prerelease") && doc["prerelease"].IsBool())
        out.prerelease = doc["prerelease"].GetBool();

    if (doc.HasMember("tag_name") && doc["tag_name"].IsString())
        out.tag_name = doc["tag_name"].GetString();
    else
        return false;

    if (!doc.HasMember("assets") || !doc["assets"].IsArray()) return false;

    std::string expected_name = expected_asset_prefix + out.tag_name + ".zip";
    for (auto& asset : doc["assets"].GetArray()) {
        if (!asset.IsObject()) continue;
        if (!asset.HasMember("name") || !asset["name"].IsString()) continue;
        if (asset["name"].GetString() != expected_name) continue;
        if (asset.HasMember("browser_download_url") && asset["browser_download_url"].IsString()) {
            out.asset_url = asset["browser_download_url"].GetString();
            return true;
        }
    }
    return false;
}

std::string AutoUpdater::http_get(const std::wstring& host, const std::wstring& path,
                                  const wchar_t* accept) {
    HINTERNET hSession = WinHttpOpen(USER_AGENT, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return {};

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return {}; }

    const wchar_t* accept_types[] = { accept ? accept : L"*/*", nullptr };
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(),
                                            nullptr, WINHTTP_NO_REFERER,
                                            accept_types, WINHTTP_FLAG_SECURE);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return {}; }

    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));

    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));

    std::wstring headers;
    if (accept) {
        headers = L"Accept: ";
        headers += accept;
        headers += L"\r\n";
    }

    if (!WinHttpSendRequest(hRequest, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                            (DWORD)headers.size(), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(hRequest, nullptr)) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return {};
    }

    DWORD status = 0, status_size = sizeof(status);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return {};
    }

    std::string body;
    DWORD available = 0;
    while (WinHttpQueryDataAvailable(hRequest, &available) && available > 0) {
        std::string chunk(available, 0);
        DWORD read = 0;
        WinHttpReadData(hRequest, chunk.data(), available, &read);
        body.append(chunk.data(), read);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return body;
}

bool AutoUpdater::http_download(const std::wstring& url, const std::wstring& dest_path,
                                std::atomic<int>& pct_out, std::atomic<bool>& cancel) {
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256]{}, path[2048]{};
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;  uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;

    HINTERNET hSession = WinHttpOpen(USER_AGENT, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    HINTERNET hConnect = WinHttpConnect(hSession, host, uc.nPort, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }

    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path, nullptr,
                                            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(hRequest, nullptr)) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD status = 0, status_size = sizeof(status);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD content_len = 0, cl_size = sizeof(content_len);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &content_len, &cl_size, WINHTTP_NO_HEADER_INDEX);

    std::ofstream file(dest_path, std::ios::binary);
    if (!file) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD total_read = 0, available = 0;
    char buf[65536];
    while (WinHttpQueryDataAvailable(hRequest, &available) && available > 0) {
        if (cancel.load(std::memory_order_relaxed)) break;
        DWORD to_read = (std::min)(available, (DWORD)sizeof(buf));
        DWORD read = 0;
        WinHttpReadData(hRequest, buf, to_read, &read);
        file.write(buf, read);
        total_read += read;
        if (content_len > 0)
            pct_out.store((int)((uint64_t)total_read * 100 / content_len), std::memory_order_relaxed);
    }
    file.close();

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return !cancel.load(std::memory_order_relaxed) &&
           (content_len == 0 || total_read == content_len);
}

bool AutoUpdater::verify_signature(const std::wstring& exe_path) {
    WINTRUST_FILE_INFO fi{};
    fi.cbStruct = sizeof(fi);
    fi.pcwszFilePath = exe_path.c_str();

    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA wd{};
    wd.cbStruct = sizeof(wd);
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    wd.dwProvFlags = WTD_SAFER_FLAG;

    LONG result = WinVerifyTrust(nullptr, &policy, &wd);

    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &policy, &wd);

    return result == ERROR_SUCCESS;
}

AutoUpdater::AutoUpdater() {
    wake_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

AutoUpdater::~AutoUpdater() {
    stop();
    if (wake_event_) CloseHandle(wake_event_);
}

void AutoUpdater::start() {
    thread_ = std::thread([this] { thread_func(); });
}

void AutoUpdater::stop() {
    stop_requested_.store(true, std::memory_order_release);
    download_cancel_.store(true, std::memory_order_release);
    if (wake_event_) SetEvent(wake_event_);
    if (thread_.joinable()) thread_.join();
}

bool AutoUpdater::poll() {
    if (!state_changed_.load(std::memory_order_acquire)) return false;
    state_changed_.store(false, std::memory_order_relaxed);
    display_state_.store(state_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    display_version_ = version_;
    display_pct_.store(download_pct_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    return true;
}

void AutoUpdater::download() {
    if (state_.load(std::memory_order_relaxed) == static_cast<int>(State::UpdateAvailable)) {
        download_cancel_.store(false, std::memory_order_relaxed);
        download_requested_.store(true, std::memory_order_release);
        if (wake_event_) SetEvent(wake_event_);
    }
}

bool AutoUpdater::parse_release_json(const std::string& json) {
    LOG_INFO("[Updater] Parsing release JSON ({} bytes)", json.size());

    ReleaseInfo info;
    if (!parse_github_release(json, "parties-client-windows-", info)) {
        LOG_WARN("[Updater] Failed to parse release or no matching Windows asset");
        return false;
    }

    if (info.prerelease) {
        LOG_INFO("[Updater] Skipping pre-release {}", info.tag_name);
        return false;
    }

    std::string remote_ver = (!info.tag_name.empty() && info.tag_name[0] == 'v')
                               ? info.tag_name.substr(1) : info.tag_name;
    std::string local_ver = PARTIES_VERSION_STR;

    LOG_INFO("[Updater] Remote: {} | Local: {} | Newer: {}", remote_ver, local_ver, is_newer(remote_ver, local_ver));

    if (!is_newer(remote_ver, local_ver)) return false;

    LOG_INFO("[Updater] Asset URL: {}", info.asset_url);

    version_ = remote_ver;
    download_url_ = info.asset_url;
    state_.store(static_cast<int>(State::UpdateAvailable), std::memory_order_relaxed);
    state_changed_.store(true, std::memory_order_release);
    return true;
}

static void parse_version(const std::string& s, int out[4]) {
    out[0] = out[1] = out[2] = out[3] = 0;
    int part = 0;
    for (char c : s) {
        if (c >= '0' && c <= '9') {
            out[part] = out[part] * 10 + (c - '0');
        } else if (c == '.' && part < 3) {
            part++;
        }
    }
}

bool AutoUpdater::is_newer(const std::string& remote, const std::string& local) {
    int rv[4], lv[4];
    parse_version(remote, rv);
    parse_version(local, lv);
    for (int i = 0; i < 4; i++) {
        if (rv[i] > lv[i]) return true;
        if (rv[i] < lv[i]) return false;
    }
    return false;
}

bool AutoUpdater::check_for_update() {
    LOG_INFO("[Updater] Checking for updates (current: {})", PARTIES_VERSION_STR);
    state_.store(static_cast<int>(State::Checking), std::memory_order_relaxed);
    state_changed_.store(true, std::memory_order_release);

    std::wstring host = L"api.github.com";
    std::wstring path = L"/repos/" + to_wide(GITHUB_OWNER) + L"/" + to_wide(GITHUB_REPO) + L"/releases/latest";

    LOG_INFO("[Updater] GET https://api.github.com{}", std::string(path.begin(), path.end()));
    std::string body = http_get(host, path, L"application/vnd.github+json");
    if (body.empty()) {
        LOG_WARN("[Updater] API request failed (empty response)");
        state_.store(static_cast<int>(State::Idle), std::memory_order_relaxed);
        state_changed_.store(true, std::memory_order_release);
        return false;
    }
    LOG_INFO("[Updater] Got {} bytes response", body.size());

    if (!parse_release_json(body)) {
        if (state_.load(std::memory_order_relaxed) != static_cast<int>(State::UpdateAvailable)) {
            state_.store(static_cast<int>(State::Idle), std::memory_order_relaxed);
            state_changed_.store(true, std::memory_order_release);
        }
        return false;
    }

    LOG_INFO("[Updater] Update available: v{} (current: {})", version_, PARTIES_VERSION_STR);
    return true;
}

bool AutoUpdater::download_asset() {
    std::string url = download_url_;
    std::string ver = version_;
    state_.store(static_cast<int>(State::Downloading), std::memory_order_relaxed);
    state_changed_.store(true, std::memory_order_release);
    download_pct_.store(0, std::memory_order_relaxed);

    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    std::wstring zip_path = std::wstring(temp) + L"parties-update-v" + to_wide(ver) + L".zip";

    LOG_INFO("[Updater] Downloading update to: {}", std::filesystem::path(zip_path).string());

    if (!http_download(to_wide(url), zip_path, download_pct_, download_cancel_)) {
        LOG_ERROR("[Updater] Download failed");
        DeleteFileW(zip_path.c_str());
        state_.store(static_cast<int>(State::Error), std::memory_order_relaxed);
        state_changed_.store(true, std::memory_order_release);
        return false;
    }

    std::wstring extract_dir = std::wstring(temp) + L"parties-update-extract";
    std::error_code ec;
    std::filesystem::remove_all(extract_dir, ec);
    std::filesystem::create_directories(extract_dir, ec);

    std::wstring ps_cmd = L"powershell -NoProfile -Command \"Expand-Archive -Force '"
                          + zip_path + L"' '" + extract_dir + L"'\"";
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, ps_cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        LOG_ERROR("[Updater] Failed to extract update zip");
        state_.store(static_cast<int>(State::Error), std::memory_order_relaxed);
        state_changed_.store(true, std::memory_order_release);
        return false;
    }
    WaitForSingleObject(pi.hProcess, 30000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    std::wstring exe_path = extract_dir + L"\\parties_client.exe";
    if (!std::filesystem::exists(exe_path)) {
        LOG_ERROR("[Updater] parties_client.exe not found in update zip");
        std::filesystem::remove_all(extract_dir, ec);
        DeleteFileW(zip_path.c_str());
        state_.store(static_cast<int>(State::Error), std::memory_order_relaxed);
        state_changed_.store(true, std::memory_order_release);
        return false;
    }

    if (!verify_signature(exe_path)) {
        LOG_ERROR("[Updater] Update exe failed Authenticode signature verification");
        std::filesystem::remove_all(extract_dir, ec);
        DeleteFileW(zip_path.c_str());
        state_.store(static_cast<int>(State::Error), std::memory_order_relaxed);
        state_changed_.store(true, std::memory_order_release);
        return false;
    }

    LOG_INFO("[Updater] Update v{} downloaded and verified", ver);

    download_path_ = zip_path;
    download_pct_.store(100, std::memory_order_relaxed);
    state_.store(static_cast<int>(State::ReadyToInstall), std::memory_order_relaxed);
    state_changed_.store(true, std::memory_order_release);
    return true;
}

void AutoUpdater::apply_and_restart() {
    stop();

    if (state_.load(std::memory_order_relaxed) != static_cast<int>(State::ReadyToInstall)) return;
    std::wstring zip_path = download_path_;

    wchar_t exe_buf[MAX_PATH];
    GetModuleFileNameW(nullptr, exe_buf, MAX_PATH);
    std::filesystem::path current_exe{exe_buf};
    std::filesystem::path old_exe = current_exe;
    old_exe.replace_extension(L".old.exe");

    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    std::filesystem::path extract_dir = std::filesystem::path{temp} / "parties-update-extract";
    std::filesystem::path extracted = extract_dir / "parties_client.exe";

    if (!std::filesystem::exists(extracted)) {
        LOG_ERROR("[Updater] Extracted exe not found: {}", extracted.string());
        return;
    }

    std::error_code ec;

    std::filesystem::remove(old_exe, ec);

    // Windows allows renaming a running exe.
    std::filesystem::rename(current_exe, old_exe, ec);
    if (ec) {
        LOG_ERROR("[Updater] Failed to rename self to .old: {}", ec.message());
        return;
    }

    std::filesystem::copy_file(extracted, current_exe, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        LOG_ERROR("[Updater] Failed to place new exe: {}", ec.message());
        std::filesystem::rename(old_exe, current_exe, ec);
        return;
    }

    std::filesystem::remove_all(extract_dir, ec);
    DeleteFileW(zip_path.c_str());

    LOG_INFO("[Updater] Launching updated exe with --update-cleanup");
    spdlog::default_logger()->flush();

    std::wstring args = L"--update-cleanup \"" + old_exe.wstring() + L"\"";
    ShellExecuteW(nullptr, L"open", current_exe.wstring().c_str(), args.c_str(), nullptr, SW_SHOWNORMAL);

    ExitProcess(0);
}

UpdateLaunch parse_update_launch(int argc, char** argv) {
    UpdateLaunch launch;
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "--update-cleanup" && i + 1 < argc) {
            launch = {UpdateLaunchMode::CleanupAfterRename, argv[++i]};
        } else if (std::string(argv[i]) == "--update-replace" && i + 1 < argc) {
            launch = {UpdateLaunchMode::LegacyReplace, argv[++i]};
        }
    }
    return launch;
}

UpdateLaunchResult AutoUpdater::finish_update_launch(const UpdateLaunch& launch) {
    switch (launch.mode) {
    case UpdateLaunchMode::Normal:
        return UpdateLaunchResult::Continue;
    case UpdateLaunchMode::CleanupAfterRename:
        finish_cleanup_after_rename(launch.other_exe);
        return UpdateLaunchResult::Continue;
    case UpdateLaunchMode::LegacyReplace:
        return run_legacy_replace_migration(launch.other_exe);
    }
    return UpdateLaunchResult::Continue;
}

void AutoUpdater::finish_cleanup_after_rename(const std::filesystem::path& old_exe) {
    for (int attempt = 0; attempt < 10; attempt++) {
        std::error_code ec;
        if (std::filesystem::remove(old_exe, ec)) break;
        Sleep(500);
    }
}

UpdateLaunchResult AutoUpdater::run_legacy_replace_migration(const std::filesystem::path& old_exe) {
    wchar_t self_buf[MAX_PATH];
    GetModuleFileNameW(nullptr, self_buf, MAX_PATH);
    std::filesystem::path self{self_buf};

    for (int i = 0; i < 20; i++) {
        std::error_code ec;
        if (std::filesystem::remove(old_exe, ec)) break;
        Sleep(500);
    }

    std::error_code ec;
    std::filesystem::copy_file(self, old_exe, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        MessageBoxA(nullptr, ("Update failed: " + ec.message()).c_str(), "Parties Update", MB_OK | MB_ICONERROR);
        return UpdateLaunchResult::ExitWithFailure;
    }

    std::wstring args = L"--update-cleanup \"" + self.wstring() + L"\"";
    ShellExecuteW(nullptr, L"open", old_exe.wstring().c_str(), args.c_str(), nullptr, SW_SHOWNORMAL);
    ExitProcess(0);
}

void AutoUpdater::thread_func() {
    check_for_update();

    while (!stop_requested_.load(std::memory_order_relaxed)) {
        if (download_requested_.exchange(false, std::memory_order_acquire)) {
            download_asset();
            continue;
        }

        WaitForSingleObject(wake_event_, CHECK_INTERVAL_SECONDS * 1000);
        ResetEvent(wake_event_);

        if (stop_requested_.load(std::memory_order_relaxed)) break;

        if (download_requested_.exchange(false, std::memory_order_acquire)) {
            download_asset();
            continue;
        }

        int s = state_.load(std::memory_order_relaxed);
        if (s == static_cast<int>(State::UpdateAvailable) ||
            s == static_cast<int>(State::Downloading) ||
            s == static_cast<int>(State::ReadyToInstall)) {
            continue;
        }

        check_for_update();
    }
}

}
