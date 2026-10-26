#pragma once

#include <string_view>

namespace parties::client {

constexpr bool is_http_url(std::string_view s) {
    return s.starts_with("http://") || s.starts_with("https://");
}

}
