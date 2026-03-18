cmake_minimum_required(VERSION 3.22)

project(qtcoroutine-test LANGUAGES CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Network Concurrent)

add_executable(qtcoroutine-test
  main.cpp
  include/qtcoroutine/qtcoroutine.hpp

  include/qtcoroutine/qfuture_coroutine_traits.hpp
  include/qtcoroutine/qtask.hpp
  include/qtcoroutine/utils.hpp
)
target_link_libraries(qtcoroutine-test PRIVATE Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Network Qt${QT_VERSION_MAJOR}::Concurrent)

enable_testing()

add_executable(qtcoroutine-tests
    tests/test_core.cpp
)
target_link_libraries(qtcoroutine-tests PRIVATE Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Network Qt${QT_VERSION_MAJOR}::Concurrent)
add_test(NAME core COMMAND qtcoroutine-tests)

include(GNUInstallDirs)
install(TARGETS qtcoroutine-test
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
