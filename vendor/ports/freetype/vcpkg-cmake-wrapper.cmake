cmake_policy(PUSH)
cmake_policy(SET CMP0012 NEW)
cmake_policy(SET CMP0054 NEW)

list(REMOVE_ITEM ARGS "NO_MODULE" "CONFIG" "MODULE")
_find_package(${ARGS} CONFIG)

if(Freetype_FOUND)
    include("${CMAKE_ROOT}/Modules/SelectLibraryConfigurations.cmake")

    get_target_property(_freetype_include_dirs freetype INTERFACE_INCLUDE_DIRECTORIES)

    if (CMAKE_SYSTEM_NAME STREQUAL "Windows" OR CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
        get_target_property(_freetype_location_debug freetype IMPORTED_IMPLIB_DEBUG)
        get_target_property(_freetype_location_release freetype IMPORTED_IMPLIB_RELEASE)
    endif()
    if(NOT _freetype_location_debug AND NOT _freetype_location_release)
        get_target_property(_freetype_location_debug freetype IMPORTED_LOCATION_DEBUG)
        get_target_property(_freetype_location_release freetype IMPORTED_LOCATION_RELEASE)
    endif()

    set(FREETYPE_FOUND TRUE)

    set(FREETYPE_INCLUDE_DIRS "${_freetype_include_dirs}")
    set(FREETYPE_INCLUDE_DIR_ft2build "${_freetype_include_dirs}")
    set(FREETYPE_INCLUDE_DIR_freetype2 "${_freetype_include_dirs}")
    set(FREETYPE_LIBRARY_DEBUG "${_freetype_location_debug}" CACHE INTERNAL "vcpkg")
    set(FREETYPE_LIBRARY_RELEASE "${_freetype_location_release}" CACHE INTERNAL "vcpkg")
    select_library_configurations(FREETYPE)
    set(FREETYPE_LIBRARIES ${FREETYPE_LIBRARY})
    set(FREETYPE_VERSION_STRING "${Freetype_VERSION}")

    unset(_freetype_include_dirs)
    unset(_freetype_location_debug)
    unset(_freetype_location_release)
endif()

cmake_policy(POP)
