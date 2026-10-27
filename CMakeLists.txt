cmake_minimum_required(VERSION 3.16)
project(GST_EventLoop VERSION 0.1.0 LANGUAGES CXX)

if (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "GST_EventLoop currently implements only the Linux backend")
endif()

include(GNUInstallDirs)
include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(GST_EVENTLOOP_USE_GST_LOG "Build GST_EventLoop with GST_log support" OFF)
set(GST_LOG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../GST_log" CACHE PATH "Path to GST_log")

add_library(GST_EventLoop STATIC
    src/EventLoop.cpp
    src/EventLoopEngine.cpp
    src/Poller.cpp
    src/ThreadPool.cpp
    src/NativeHandle.cpp
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
    if (NOT TARGET GST::Log)
        if (NOT EXISTS "${GST_LOG_DIR}/CMakeLists.txt")
            message(FATAL_ERROR "GST_log not found at ${GST_LOG_DIR}")
        endif()
        add_subdirectory("${GST_LOG_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/GST_log")
    endif()
    if (NOT TARGET GST::Log)
        message(FATAL_ERROR "GST_log did not provide GST::Log")
    endif()
    target_compile_definitions(GST_EventLoop PRIVATE GST_EVENTLOOP_USE_GST_LOG)
    target_link_libraries(GST_EventLoop PRIVATE GST::Log)
endif()

if (BUILD_TESTING)
    file(GLOB test_sources CONFIGURE_DEPENDS "tests/*Tests.cpp")
    add_executable(GST_EventLoop_tests
        tests/TestMain.cpp
        ${test_sources}
    )
    target_link_libraries(GST_EventLoop_tests PRIVATE GST::EventLoop)
    target_compile_definitions(GST_EventLoop PRIVATE GST_EVENTLOOP_TESTING)
    target_compile_definitions(GST_EventLoop_tests PRIVATE GST_EVENTLOOP_TESTING)
    foreach(test_source IN LISTS test_sources)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${test_source}")
        file(READ "${test_source}" test_contents)
        string(REGEX MATCHALL "TEST_CASE\\([A-Za-z_][A-Za-z_0-9]*\\)" test_cases "${test_contents}")
        foreach(test_declaration IN LISTS test_cases)
            string(REGEX REPLACE "TEST_CASE\\(([A-Za-z_][A-Za-z_0-9]*)\\)" "\\1" test_case "${test_declaration}")
            add_test(NAME eventloop.${test_case} COMMAND GST_EventLoop_tests ${test_case})
            set_tests_properties(eventloop.${test_case} PROPERTIES TIMEOUT 20)
        endforeach()
    endforeach()
endif()

option(GST_EVENTLOOP_BUILD_EXAMPLES "Build the TCP example with a Linux resource provider" ON)
if (GST_EVENTLOOP_BUILD_EXAMPLES)
    add_executable(GST_EventLoop_example examples/main.cpp examples/NetworkProvider.cpp)
    target_link_libraries(GST_EventLoop_example PRIVATE GST::EventLoop)
endif()

install(TARGETS GST_EventLoop
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(FILES
    src/EventLoop.h
    src/EventTypes.h
    src/IoRegistration.h
    src/NativeHandle.h
    src/ThreadPool.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
