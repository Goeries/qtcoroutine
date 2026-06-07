# Examples are linked against the interface library, which already brings in
# Qt6::Core and Qt6::Concurrent.

add_executable(qtcoroutine-example-basic basic.cpp)
target_link_libraries(qtcoroutine-example-basic PRIVATE qtcoroutine::qtcoroutine)
