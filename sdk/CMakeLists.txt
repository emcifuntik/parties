add_library(parties_plugin_sdk INTERFACE)

target_include_directories(parties_plugin_sdk
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

install(DIRECTORY include/ DESTINATION include)

install(TARGETS parties_plugin_sdk
    EXPORT parties_plugin_sdk_targets
)

install(EXPORT parties_plugin_sdk_targets
    NAMESPACE parties::
    DESTINATION lib/cmake/parties
)
