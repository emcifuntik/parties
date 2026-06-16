add_library(parties_plugin_bot_echo SHARED
    bot_echo.cpp
)

target_link_libraries(parties_plugin_bot_echo
    PRIVATE
        parties_plugin_sdk
)

set_target_properties(parties_plugin_bot_echo PROPERTIES
    PREFIX ""
)

file(GENERATE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/plugin.toml"
    CONTENT "id = \"parties.example.bot_echo\"\nname = \"Bot Echo\"\nversion = \"0.1.0\"\napi_version = \"1.0\"\nlibrary = \"$<TARGET_FILE_NAME:parties_plugin_bot_echo>\"\n\nvariables = { echo_prefix = \"env:PARTIES_BOT_ECHO_PREFIX\" }\n\npermissions = [\n  \"read_sessions\",\n  \"read_users\",\n  \"read_channels\",\n  \"read_chat\",\n  \"moderate_chat\",\n  \"create_chat_commands\",\n  \"create_bot_users\",\n  \"send_bot_chat\",\n  \"join_bot_voice\",\n  \"send_bot_audio\"\n]\n"
)

add_custom_command(TARGET parties_plugin_bot_echo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:parties_server>/plugins/bot_echo"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:parties_plugin_bot_echo>" "$<TARGET_FILE_DIR:parties_server>/plugins/bot_echo/$<TARGET_FILE_NAME:parties_plugin_bot_echo>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_BINARY_DIR}/plugin.toml" "$<TARGET_FILE_DIR:parties_server>/plugins/bot_echo/plugin.toml"
)
