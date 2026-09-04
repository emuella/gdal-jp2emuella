# Find the experimental Emuella JPEG 2000 C ABI.
#
# Cache inputs:
#   EmuellaJ2K_ROOT         installation prefix
#   EmuellaJ2K_INCLUDE_DIR  directory containing emuella_j2k.h
#   EmuellaJ2K_LIBRARY      C ABI library
#   EmuellaJ2K_SOURCE_DIR   optional source checkout used to verify the revision

set(EmuellaJ2K_REQUIRED_REVISION
    "de9234cbe6b56579f0eb4b21f5cfadf553c6baba"
    CACHE STRING "Required emuella-j2k source revision")
set(EmuellaJ2K_SOURCE_DIR "" CACHE PATH
    "Optional emuella-j2k checkout for exact revision verification")

find_path(EmuellaJ2K_INCLUDE_DIR
    NAMES emuella_j2k.h
    HINTS "${EmuellaJ2K_ROOT}"
    PATH_SUFFIXES include crates/emuella-j2k-capi/include
)
find_library(EmuellaJ2K_LIBRARY
    NAMES emuella_j2k_capi
    HINTS "${EmuellaJ2K_ROOT}" "${EmuellaJ2K_SOURCE_DIR}/target/release"
    PATH_SUFFIXES lib lib64 target/release
)

if(EmuellaJ2K_SOURCE_DIR)
    execute_process(
        COMMAND git rev-parse HEAD
        WORKING_DIRECTORY "${EmuellaJ2K_SOURCE_DIR}"
        RESULT_VARIABLE _emuella_git_result
        OUTPUT_VARIABLE _emuella_git_revision
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(NOT _emuella_git_result EQUAL 0)
        message(FATAL_ERROR
            "Could not determine the revision of EmuellaJ2K_SOURCE_DIR: "
            "${EmuellaJ2K_SOURCE_DIR}")
    endif()
    if(NOT _emuella_git_revision STREQUAL EmuellaJ2K_REQUIRED_REVISION)
        message(FATAL_ERROR
            "emuella-j2k revision ${_emuella_git_revision} does not match required "
            "revision ${EmuellaJ2K_REQUIRED_REVISION}")
    endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(EmuellaJ2K
    REQUIRED_VARS EmuellaJ2K_INCLUDE_DIR EmuellaJ2K_LIBRARY
)

if(EmuellaJ2K_FOUND AND NOT TARGET EmuellaJ2K::EmuellaJ2K)
    add_library(EmuellaJ2K::EmuellaJ2K SHARED IMPORTED)
    set_target_properties(EmuellaJ2K::EmuellaJ2K PROPERTIES
        IMPORTED_LOCATION "${EmuellaJ2K_LIBRARY}"
        IMPORTED_NO_SONAME TRUE
        INTERFACE_INCLUDE_DIRECTORIES "${EmuellaJ2K_INCLUDE_DIR}"
    )
endif()

mark_as_advanced(EmuellaJ2K_INCLUDE_DIR EmuellaJ2K_LIBRARY)
