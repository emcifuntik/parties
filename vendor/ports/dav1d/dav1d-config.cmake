if(TARGET dav1d::dav1d)
    return()
endif()

get_filename_component(_IMPORT_PREFIX "${CMAKE_CURRENT_LIST_DIR}/../../" ABSOLUTE)

add_library(dav1d::dav1d UNKNOWN IMPORTED)

find_library(dav1d_LIBRARY_RELEASE NAMES dav1d PATHS "${_IMPORT_PREFIX}/lib" NO_DEFAULT_PATH)
find_library(dav1d_LIBRARY_DEBUG NAMES dav1d PATHS "${_IMPORT_PREFIX}/debug/lib" NO_DEFAULT_PATH)

set_target_properties(dav1d::dav1d PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_IMPORT_PREFIX}/include"
)

if(dav1d_LIBRARY_RELEASE)
    set_property(TARGET dav1d::dav1d APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
    set_target_properties(dav1d::dav1d PROPERTIES IMPORTED_LOCATION_RELEASE "${dav1d_LIBRARY_RELEASE}")
    if(NOT dav1d_LIBRARY_DEBUG)
        set_target_properties(dav1d::dav1d PROPERTIES IMPORTED_LOCATION "${dav1d_LIBRARY_RELEASE}")
    endif()
endif()

if(dav1d_LIBRARY_DEBUG)
    set_property(TARGET dav1d::dav1d APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
    set_target_properties(dav1d::dav1d PROPERTIES IMPORTED_LOCATION_DEBUG "${dav1d_LIBRARY_DEBUG}")
endif()

if(NOT dav1d_LIBRARY_RELEASE AND NOT dav1d_LIBRARY_DEBUG)
    message(FATAL_ERROR "dav1d library not found")
endif()

include(CMakeFindDependencyMacro)
find_dependency(Threads)
set_property(TARGET dav1d::dav1d APPEND PROPERTY INTERFACE_LINK_LIBRARIES Threads::Threads)
