cmake_minimum_required(VERSION 3.20)

project(gdal-jp2emuella VERSION 0.1.0 LANGUAGES CXX)

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(GNUInstallDirs)
include(CTest)

find_package(GDAL 3.13 REQUIRED CONFIG)
find_package(EmuellaJ2K REQUIRED)
find_package(Threads REQUIRED)

add_library(gdal_JP2Emuella MODULE
    src/jp2emuella_dataset.cpp
)
target_compile_features(gdal_JP2Emuella PRIVATE cxx_std_17)
target_compile_options(gdal_JP2Emuella PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)
target_link_libraries(gdal_JP2Emuella PRIVATE
    GDAL::GDAL
    EmuellaJ2K::EmuellaJ2K
    Threads::Threads
)
set_target_properties(gdal_JP2Emuella PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)

set(GDAL_PLUGIN_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/gdalplugins"
    CACHE PATH "Installation directory for the GDAL plugin module")
install(TARGETS gdal_JP2Emuella
    LIBRARY DESTINATION "${GDAL_PLUGIN_INSTALL_DIR}"
    RUNTIME DESTINATION "${GDAL_PLUGIN_INSTALL_DIR}"
)

if(BUILD_TESTING)
    add_executable(jp2emuella_native_tests tests/native_tests.cpp)
    target_compile_features(jp2emuella_native_tests PRIVATE cxx_std_17)
    target_compile_options(jp2emuella_native_tests PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
    )
    target_compile_definitions(jp2emuella_native_tests PRIVATE
        JP2EMUELLA_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
    )
    target_link_libraries(jp2emuella_native_tests PRIVATE GDAL::GDAL Threads::Threads)
    add_test(NAME jp2emuella_native COMMAND jp2emuella_native_tests)
    set_tests_properties(jp2emuella_native PROPERTIES
        ENVIRONMENT "GDAL_DRIVER_PATH=$<TARGET_FILE_DIR:gdal_JP2Emuella>"
    )
endif()
