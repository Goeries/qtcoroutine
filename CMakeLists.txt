cmake_minimum_required(VERSION 3.14)

project(qtcoroutine
    VERSION 1.0.0
    LANGUAGES CXX
)

add_library(qtcoroutine INTERFACE)
add_library(qtcoroutine::qtcoroutine ALIAS qtcoroutine)

target_include_directories(qtcoroutine INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(qtcoroutine INTERFACE cxx_std_20)

# Install
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS qtcoroutine
    EXPORT qtcoroutineTargets
)

install(DIRECTORY include/qtcoroutine
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT qtcoroutineTargets
    FILE qtcoroutineTargets.cmake
    NAMESPACE qtcoroutine::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qtcoroutine
)

configure_package_config_file(
    cmake/qtcoroutineConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/qtcoroutineConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qtcoroutine
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/qtcoroutineConfigVersion.cmake
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/qtcoroutineConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/qtcoroutineConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qtcoroutine
)

# Examples
option(QTCOROUTINE_BUILD_EXAMPLES "Build example applications" OFF)
if(QTCOROUTINE_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# Tests
option(QTCOROUTINE_BUILD_TESTS "Build tests" OFF)
if(QTCOROUTINE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
