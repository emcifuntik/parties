#include <server/attachment_storage_path.h>

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

using parties::server::AttachmentStoragePath;

namespace {

constexpr std::time_t MARCH_2024_UTC = 1'709'900'000;

int failures = 0;

void expect_eq(const std::string& got, const std::string& want, const char* what) {
    if (got != want) {
        std::cerr << "FAIL: " << what << ": got '" << got << "', want '" << want << "'\n";
        ++failures;
    }
}

std::string path_for(std::string_view client_name, uint64_t message_id = 42, int index = 0) {
    return AttachmentStoragePath::for_new_attachment(MARCH_2024_UTC, message_id, index, client_name).relative();
}

}

int main() {
    expect_eq(path_for("photo.png"), "2024-03/42_0.png", "month directory, message id, index and extension");
    expect_eq(path_for("photo.png", 7, 3), "2024-03/7_3.png", "message id and index come from the server");
    expect_eq(path_for("Report.PDF"), "2024-03/42_0.pdf", "extension is lowercased");
    expect_eq(path_for("archive.tar.gz"), "2024-03/42_0.gz", "extension starts at the last dot");
    expect_eq(path_for("noext"), "2024-03/42_0", "no dot means no extension");
    expect_eq(path_for("trailing."), "2024-03/42_0", "a trailing dot means no extension");
    expect_eq(path_for("x.abcdefghijk"), "2024-03/42_0.abcdefg", "extension keeps at most 8 characters");

    expect_eq(path_for("../../etc/passwd"), "2024-03/42_0.etcpass", "parent segments in the name never reach the path");
    expect_eq(path_for("evil./../../x"), "2024-03/42_0.x", "separators after the last dot are dropped");
    expect_eq(path_for("a.p/n\\g"), "2024-03/42_0.png", "slashes inside the extension are dropped");
    expect_eq(path_for("C:\\dir\\file.e x:e"), "2024-03/42_0.exe", "spaces, colons and drive letters are dropped");
    expect_eq(path_for("name.\xC3\xA9xe"), "2024-03/42_0.xe", "non-ASCII bytes are dropped");

    const auto stored = AttachmentStoragePath::from_database("2023-12/9_1.txt");
    expect_eq(stored.relative(), "2023-12/9_1.txt", "database value round-trips");
    expect_eq(stored.under("files").generic_string(), "files/2023-12/9_1.txt", "path joins under the storage root");

    if (failures) return 1;
    std::cout << "attachment_storage_path: all checks passed\n";
    return 0;
}
