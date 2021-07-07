cmake_minimum_required(VERSION 3.0)

project(JanusVideoroomStreamer)

if(DEFINED ENV{SNAPCRAFT_BUILD_ENVIRONMENT})
    add_definitions(-DSNAPCRAFT_BUILD=1)
    set(SNAPCRAFT_BUILD YES)
endif()

add_subdirectory(Helpers)
add_subdirectory(RtStreaming)
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/RtStreaming/deps)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_search_module(SPDLOG REQUIRED spdlog)
pkg_search_module(LIBCONFIG REQUIRED libconfig)
pkg_search_module(JANSSON REQUIRED jansson)

file(GLOB SOURCES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    *.cpp
    *.h
    *.conf.sample
    *.cmake
    snap/snapcraft.yaml
    snap/hooks/*
    )

file(GLOB_RECURSE SNAP RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    snap/*)

add_executable(${PROJECT_NAME} ${SOURCES} ${SNAP})
target_include_directories(${PROJECT_NAME} PUBLIC
    ${LIBCONFIG_INCLUDE_DIRS}
    ${SPDLOG_INCLUDE_DIRS}
    ${JANSSON_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME}
    ${LIBCONFIG_LIBRARIES}
    ${SPDLOG_LDFLAGS}
    ${JANSSON_LDFLAGS}
    Helpers
    RtStreaming)

if(SNAPCRAFT_BUILD)
    install(TARGETS ${PROJECT_NAME} DESTINATION bin)
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/janus-videoroom-streamer.conf.sample DESTINATION etc)
endif()
