cmake_minimum_required(VERSION 3.16)

if (TARGET GST::EventLoop)
    return()
endif()

if (TARGET GST_EventLoop)
    message(FATAL_ERROR "GST_EventLoop target already exists without the GST::EventLoop alias")
endif()

project(GST_EventLoop VERSION 0.1.0 LANGUAGES CXX)

if (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "GST_EventLoop currently implements only the Linux backend")
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if (CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(GST_EVENTLOOP_STANDALONE ON)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    include(CTest)
    set(GST_EVENTLOOP_BUILD_TESTS_DEFAULT ${BUILD_TESTING})
else()
    set(GST_EVENTLOOP_STANDALONE OFF)
    set(GST_EVENTLOOP_BUILD_TESTS_DEFAULT OFF)
endif()

option(GST_EVENTLOOP_USE_GST_LOG "Build GST_EventLoop with GST-Log support" OFF)
option(GST_EVENTLOOP_BUILD_TESTS "Build GST_EventLoop tests" ${GST_EVENTLOOP_BUILD_TESTS_DEFAULT})
option(GST_EVENTLOOP_BUILD_EXAMPLES "Build GST_EventLoop examples" ${GST_EVENTLOOP_STANDALONE})
if (NOT DEFINED GST_LOG_DIR)
    set(GST_LOG_DIR "" CACHE PATH "Optional path to the GST-Log source tree")
endif()

add_library(GST_EventLoop STATIC
    src/EventLoop.cpp
    src/EventLoopEngine.cpp
    src/EventTypes.cpp
    src/IoRegistration.cpp
    src/IoRegistrationState.cpp
    src/Poller.cpp
    src/ThreadPool.cpp
    src/NativeHandle.cpp
    src/TimerEngine.cpp
    src/TimerRegistration.cpp
    src/TimerRegistrationState.cpp
    src/TimerTypes.cpp
)
add_library(GST::EventLoop ALIAS GST_EventLoop)
set_target_properties(GST_EventLoop PROPERTIES EXPORT_NAME EventLoop)

target_include_directories(GST_EventLoop
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(GST_EventLoop PUBLIC cxx_std_17)

find_package(Threads REQUIRED)
target_link_libraries(GST_EventLoop PUBLIC Threads::Threads)

if (GST_EVENTLOOP_USE_GST_LOG)
    if (NOT TARGET GST::Log)
        if (GST_LOG_DIR)
            if (NOT EXISTS "${GST_LOG_DIR}/CMakeLists.txt")
                message(FATAL_ERROR "GST-Log not found at ${GST_LOG_DIR}")
            endif()
            add_subdirectory("${GST_LOG_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/GST-Log")
        else()
            find_package(GSTLog CONFIG REQUIRED)
        endif()
    endif()
    if (NOT TARGET GST::Log)
        message(FATAL_ERROR "GST-Log did not provide GST::Log")
    endif()
    target_compile_definitions(GST_EventLoop PRIVATE GST_EVENTLOOP_USE_GST_LOG)
    target_link_libraries(GST_EventLoop PRIVATE GST::Log)
endif()

if (GST_EVENTLOOP_BUILD_TESTS)
    enable_testing()
    file(GLOB test_sources CONFIGURE_DEPENDS "tests/*Tests.cpp")
    add_executable(GST_EventLoop_tests
        tests/TestMain.cpp
        tests/EventLoopTestAccess.cpp
        tests/TestSupport.cpp
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

if (GST_EVENTLOOP_BUILD_EXAMPLES)
    add_executable(GST_EventLoop_example examples/main.cpp examples/NetworkProvider.cpp)
    target_link_libraries(GST_EventLoop_example PRIVATE GST::EventLoop)
endif()

install(TARGETS GST_EventLoop EXPORT GSTEventLoopTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(FILES
    src/EventLoop.h
    src/EventTypes.h
    src/IoRegistration.h
    src/NativeHandle.h
    src/ThreadPool.h
    src/TimerRegistration.h
    src/TimerTypes.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT GSTEventLoopTargets
    NAMESPACE GST::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/GSTEventLoop
)

set(GST_EVENTLOOP_PACKAGE_USES_GST_LOG ${GST_EVENTLOOP_USE_GST_LOG})
configure_package_config_file(
    cmake/GSTEventLoopConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/GSTEventLoopConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/GSTEventLoop
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/GSTEventLoopConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/GSTEventLoopConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/GSTEventLoopConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/GSTEventLoop
)
