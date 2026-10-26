#pragma once

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

namespace parties::server {

class AttachmentStoragePath {
public:
    static AttachmentStoragePath for_new_attachment(std::time_t now_utc, uint64_t message_id,
                                                    int index, std::string_view client_file_name) {
        std::tm tm_buf{};
#ifdef _MSC_VER
        gmtime_s(&tm_buf, &now_utc);
#else
        gmtime_r(&now_utc, &tm_buf);
#endif
        char month_dir[16];
        std::strftime(month_dir, sizeof(month_dir), "%Y-%m", &tm_buf);
        return AttachmentStoragePath(std::string(month_dir) + "/" + std::to_string(message_id) + "_" +
                                     std::to_string(index) + extension_of(client_file_name));
    }

    static AttachmentStoragePath from_database(std::string stored) {
        return AttachmentStoragePath(std::move(stored));
    }

    const std::string& relative() const { return relative_; }

    std::filesystem::path under(const std::filesystem::path& storage_root) const {
        return storage_root / relative_;
    }

private:
    static constexpr size_t kMaxExtensionChars = 8;

    explicit AttachmentStoragePath(std::string relative) : relative_(std::move(relative)) {}

    static std::string extension_of(std::string_view file_name) {
        const size_t dot = file_name.find_last_of('.');
        if (dot == std::string_view::npos || dot + 1 >= file_name.size()) return {};
        std::string ext;
        for (char c : file_name.substr(dot)) {
            if (c >= 'A' && c <= 'Z')
                ext += static_cast<char>(c - 'A' + 'a');
            else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.')
                ext += c;
            if (ext.size() >= kMaxExtensionChars) break;
        }
        return ext;
    }

    std::string relative_;
};

}
