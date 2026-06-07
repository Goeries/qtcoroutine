# Examples are linked against the interface library, which already brings in
# Qt6::Core and Qt6::Concurrent.

add_executable(qtcoroutine-example-basic basic.cpp)
target_link_libraries(qtcoroutine-example-basic PRIVATE qtcoroutine::qtcoroutine)

# Run the example under CTest too (only when tests are enabled, so
# enable_testing() has already run at the top level). Keeps the CI run path
# cross-platform — no per-OS binary-path handling needed.
if(QTCOROUTINE_BUILD_TESTS)
    add_test(NAME example-basic COMMAND qtcoroutine-example-basic)
    set_tests_properties(example-basic PROPERTIES TIMEOUT 60)
endif()
