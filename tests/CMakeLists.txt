find_package(Qt6 REQUIRED COMPONENTS Core Concurrent Test)

set(CMAKE_AUTOMOC ON)

add_executable(test_qfuture_coroutine test_qfuture_coroutine.cpp)
target_link_libraries(test_qfuture_coroutine PRIVATE
    qtcoroutine::qtcoroutine
    Qt6::Core
    Qt6::Concurrent
    Qt6::Test
)
add_test(NAME test_qfuture_coroutine COMMAND test_qfuture_coroutine)

add_executable(test_qtask test_qtask.cpp)
target_link_libraries(test_qtask PRIVATE
    qtcoroutine::qtcoroutine
    Qt6::Core
    Qt6::Concurrent
    Qt6::Test
)
add_test(NAME test_qtask COMMAND test_qtask)
