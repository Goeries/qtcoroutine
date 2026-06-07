cmake_minimum_required(VERSION 3.22)

project(qtcoroutine
    VERSION 0.1.0
    LANGUAGES CXX
)

# --- Interface library (what consumers link against) ---

add_library(qtcoroutine INTERFACE)
add_library(qtcoroutine::qtcoroutine ALIAS qtcoroutine)

target_include_directories(qtcoroutine INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/QtCoroutine>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/QtCoroutine>
)

target_compile_features(qtcoroutine INTERFACE cxx_std_23)

find_package(Qt6 REQUIRED COMPONENTS Core Concurrent)
target_link_libraries(qtcoroutine INTERFACE Qt6::Core Qt6::Concurrent)

# --- Install ---

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS qtcoroutine
    EXPORT qtcoroutineTargets
)

install(DIRECTORY include/QtCoroutine
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT qtcoroutineTargets
    FILE qtcoroutineTargets.cmake
    NAMESPACE qtcoroutine::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qtcoroutine
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/qtcoroutineConfigVersion.cmake
    COMPATIBILITY SameMajorVersion
)

file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/qtcoroutineConfig.cmake
    "include(\"\${CMAKE_CURRENT_LIST_DIR}/qtcoroutineTargets.cmake\")\n"
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/qtcoroutineConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/qtcoroutineConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qtcoroutine
)

# --- Tests (opt-in) ---

option(QTCOROUTINE_BUILD_TESTS "Build tests" OFF)
if(QTCOROUTINE_BUILD_TESTS)
    enable_testing()

    set(CMAKE_AUTOMOC ON)

    find_package(Qt6 REQUIRED COMPONENTS Network)

    add_executable(qtcoroutine-tests tests/test_core.cpp)
    target_link_libraries(qtcoroutine-tests PRIVATE qtcoroutine::qtcoroutine Qt6::Network)
    add_test(NAME core COMMAND qtcoroutine-tests)
    set_tests_properties(core PROPERTIES TIMEOUT 180)
endif()

# --- Examples (opt-in) ---

option(QTCOROUTINE_BUILD_EXAMPLES "Build examples" OFF)
if(QTCOROUTINE_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
