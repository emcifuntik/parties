find_package(RmlUi CONFIG REQUIRED)

set(DESIGNER_SOURCES
    src/main.cpp
    src/designer_app.cpp
    src/file_watcher.cpp
    src/embedded_fonts.cpp

    # Reuse from client
    ${CMAKE_SOURCE_DIR}/client/src/slug_glyph_cache.cpp
    ${CMAKE_SOURCE_DIR}/client/src/slug_font_engine.cpp
    ${CMAKE_SOURCE_DIR}/client/src/dx12/RmlUi_Renderer_DX12.cpp
    ${CMAKE_SOURCE_DIR}/client/src/windows/RmlUi_Platform_Win32.cpp
)

add_executable(rmlui_designer ${DESIGNER_SOURCES})

target_include_directories(rmlui_designer PRIVATE
    ${CMAKE_SOURCE_DIR}/client/include      # slug_font_engine.h, slug_glyph_cache.h
    ${CMAKE_SOURCE_DIR}/client/src          # RmlUi_RenderInterface_Extended.h
    ${CMAKE_SOURCE_DIR}/client/src/dx12     # RmlUi_Renderer_DX12.h
    ${CMAKE_SOURCE_DIR}/client/src/windows  # RmlUi_Platform_Win32.h
    ${CMAKE_SOURCE_DIR}/common/include      # parties/profiler.h (no-op stubs)
)

target_link_libraries(rmlui_designer PRIVATE
    RmlUi::RmlUi
    RmlUi::Debugger
    d3d12 dxgi d3dcompiler dcomp dwmapi
)

target_link_options(rmlui_designer PRIVATE /MANIFEST:NO)

# stb_truetype header — used by slug_font_engine.cpp
target_include_directories(rmlui_designer PRIVATE
    ${CMAKE_SOURCE_DIR}/client/src  # for font/stb_truetype.h
)

# #embed dependencies — recompile when embedded font files change
file(GLOB DESIGNER_EMBED_FONTS ${CMAKE_SOURCE_DIR}/client/ui/fonts/*.ttf)
set_source_files_properties(src/embedded_fonts.cpp PROPERTIES
    OBJECT_DEPENDS "${DESIGNER_EMBED_FONTS}"
)
