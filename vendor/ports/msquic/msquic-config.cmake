if(TARGET msquic::msquic)
    return()
endif()

get_filename_component(_IMPORT_PREFIX "${CMAKE_CURRENT_LIST_DIR}/../../" ABSOLUTE)

add_library(msquic::msquic UNKNOWN IMPORTED)

find_library(msquic_LIBRARY_RELEASE NAMES msquic PATHS "${_IMPORT_PREFIX}/lib" NO_DEFAULT_PATH)
find_library(msquic_LIBRARY_DEBUG NAMES msquic PATHS "${_IMPORT_PREFIX}/debug/lib" NO_DEFAULT_PATH)

set_target_properties(msquic::msquic PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_IMPORT_PREFIX}/include"
    INTERFACE_COMPILE_DEFINITIONS "QUIC_BUILD_STATIC"
)

if(msquic_LIBRARY_RELEASE)
    set_property(TARGET msquic::msquic APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
    set_target_properties(msquic::msquic PROPERTIES IMPORTED_LOCATION_RELEASE "${msquic_LIBRARY_RELEASE}")
    if(NOT msquic_LIBRARY_DEBUG)
        set_target_properties(msquic::msquic PROPERTIES IMPORTED_LOCATION "${msquic_LIBRARY_RELEASE}")
    endif()
endif()

if(msquic_LIBRARY_DEBUG)
    set_property(TARGET msquic::msquic APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
    set_target_properties(msquic::msquic PROPERTIES IMPORTED_LOCATION_DEBUG "${msquic_LIBRARY_DEBUG}")
endif()

if(NOT msquic_LIBRARY_RELEASE AND NOT msquic_LIBRARY_DEBUG)
    message(FATAL_ERROR "msquic library not found")
endif()

# wolfSSL TLS backend + platform dependencies
find_package(wolfssl CONFIG REQUIRED)

if(WIN32)
    set_property(TARGET msquic::msquic APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES
        wolfssl::wolfssl
        ntdll wbemuuid winmm secur32 onecore crypt32)
else()
    find_package(Threads REQUIRED)
    set_property(TARGET msquic::msquic APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES
        wolfssl::wolfssl
        Threads::Threads
        ${CMAKE_DL_LIBS})
endif()
