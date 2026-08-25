cmake_minimum_required(VERSION 3.16)
project(GST_EventLoop VERSION 0.1.0 LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(GST_EVENTLOOP_USE_GST_LOG "Build GST_EventLoop with GST_log support" ON)
option(GST_EVENTLOOP_BUILD_STRESS_TESTS "Build standalone EventLoop stress executable" OFF)
option(GST_EVENTLOOP_BUILD_BENCHMARKS "Build standalone EventLoop benchmark executable" OFF)
set(GST_LOG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../GST_log" CACHE PATH "Path to GST_log")

add_library(GST_EventLoop STATIC
    src/EventLoop.cpp
    src/EventLoopEngine.cpp
    src/Poller.cpp
    src/ThreadPool.cpp
)
add_library(GST::EventLoop ALIAS GST_EventLoop)

target_include_directories(GST_EventLoop
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(GST_EventLoop PUBLIC cxx_std_17)

find_package(Threads REQUIRED)
target_link_libraries(GST_EventLoop PUBLIC Threads::Threads)

if (GST_EVENTLOOP_USE_GST_LOG)
    if (NOT EXISTS "${GST_LOG_DIR}/CMakeLists.txt")
        message(FATAL_ERROR "GST_log not found at ${GST_LOG_DIR}")
    endif()

    add_subdirectory("${GST_LOG_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/GST_log")
    target_compile_definitions(GST_EventLoop PRIVATE GST_EVENTLOOP_USE_GST_LOG)
    target_link_libraries(GST_EventLoop PUBLIC Tlog)
endif()

if (BUILD_TESTING)
    add_executable(GST_EventLoop_tests
        tests/test_eventloop.cpp
    )
    target_link_libraries(GST_EventLoop_tests PRIVATE GST::EventLoop)
    add_test(NAME GST_EventLoop_tests COMMAND GST_EventLoop_tests)
endif()

if (GST_EVENTLOOP_BUILD_STRESS_TESTS)
    add_executable(GST_EventLoop_stress
        tests/stress_eventloop.cpp
    )
    target_link_libraries(GST_EventLoop_stress PRIVATE GST::EventLoop)
endif()

if (GST_EVENTLOOP_BUILD_BENCHMARKS)
    add_executable(GST_EventLoop_benchmark
        tests/benchmark_eventloop.cpp
    )
    target_link_libraries(GST_EventLoop_benchmark PRIVATE GST::EventLoop)
endif()
