cmake_minimum_required(VERSION 3.16)
project(GST_EventLoop VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)

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
    # Prefer the log target supplied by the outermost project. This keeps one
    # process-wide GST_log instance shared by the application and its libraries.
    # Loading the target here only satisfies the build dependency; GST_EventLoop
    # never initializes or shuts down the process-wide logger.
    if (NOT TARGET GST::Log)
        if (NOT EXISTS "${GST_LOG_DIR}/CMakeLists.txt")
            message(FATAL_ERROR
                "GST_log target was not provided and GST_log was not found at ${GST_LOG_DIR}"
            )
        endif()

        add_subdirectory("${GST_LOG_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/GST_log")
    endif()

    if (NOT TARGET GST::Log)
        message(FATAL_ERROR "GST_log did not provide the GST::Log target")
    endif()

    target_compile_definitions(GST_EventLoop PRIVATE GST_EVENTLOOP_USE_GST_LOG)
    target_link_libraries(GST_EventLoop PRIVATE GST::Log)
endif()

install(TARGETS GST_EventLoop
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(FILES
    src/EventLoop.h
    src/EventTypes.h
    src/IoHandle.h
    src/IoToken.h
    src/NativeHandle.h
    src/ThreadPool.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
