#pragma once

#include <string>

namespace parties {

bool crash_reporter_is_crashpad_handler(int argc, char* argv[]);

void crash_reporter_init(const char* dsn, const char* exe_path = nullptr);

void crash_reporter_set_user(const std::string& user_id, const std::string& display_name);

void crash_reporter_shutdown();

}
