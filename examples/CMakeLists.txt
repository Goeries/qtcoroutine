find_package(Qt6 REQUIRED COMPONENTS Core Concurrent)

add_executable(qtcoroutine_example main.cpp)
target_link_libraries(qtcoroutine_example PRIVATE
    qtcoroutine::qtcoroutine
    Qt6::Core
    Qt6::Concurrent
)
