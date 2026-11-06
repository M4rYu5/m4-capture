cmake_minimum_required(VERSION 3.24)

project(M4Capture VERSION 0.3.1 LANGUAGES C CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "M4Capture currently targets Linux.")
endif()

find_package(libobs REQUIRED CONFIG)
if(libobs_VERSION VERSION_LESS 32)
  message(FATAL_ERROR "OBS Studio 32 or newer is required.")
endif()
find_package(obs-frontend-api REQUIRED CONFIG)
find_package(Qt6 REQUIRED COMPONENTS Core)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3>=1.2)
pkg_check_modules(DRM REQUIRED IMPORTED_TARGET libdrm)
find_package(OpenGL REQUIRED)
pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client)
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
set(PROTOCOL_SOURCES)
set(CAPTURE_PROTOCOLS plasma-window-management zkde-screencast-unstable-v1 xdg-output-unstable-v1)
foreach(PROTOCOL ${CAPTURE_PROTOCOLS})
  set(XML "${CMAKE_CURRENT_SOURCE_DIR}/protocols/${PROTOCOL}.xml")
  set(HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/${PROTOCOL}-client-protocol.h")
  set(CODE "${CMAKE_CURRENT_BINARY_DIR}/generated/${PROTOCOL}-protocol.c")
  add_custom_command(OUTPUT "${HEADER}" "${CODE}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND ${WAYLAND_SCANNER} client-header "${XML}" "${HEADER}"
    COMMAND ${WAYLAND_SCANNER} private-code "${XML}" "${CODE}"
    DEPENDS "${XML}" VERBATIM)
  list(APPEND PROTOCOL_SOURCES "${HEADER}" "${CODE}")
endforeach()

set(CAPTURE_SOURCES
  src/capture.cpp src/capture.hpp
  src/capture-runtime.cpp
  src/display-info.hpp
  src/kwin-connection.cpp src/kwin-connection.hpp
  src/pipewire-capture.cpp src/pipewire-capture.hpp
  src/pipewire-audio.cpp src/pipewire-audio.hpp
  src/video-frame.cpp src/video-frame.hpp
  src/frame-buffer.cpp src/frame-buffer.hpp
  src/window-rule.cpp src/window-rule.hpp
  ${PROTOCOL_SOURCES}
)

add_library(capture-core STATIC ${CAPTURE_SOURCES})
target_include_directories(capture-core PUBLIC src PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(capture-core PUBLIC Qt6::Core Threads::Threads PkgConfig::PIPEWIRE PkgConfig::WAYLAND)
target_compile_features(capture-core PUBLIC cxx_std_17)
target_compile_options(capture-core PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(capture-core PROPERTIES
  AUTOMOC ON
  POSITION_INDEPENDENT_CODE ON
  C_VISIBILITY_PRESET hidden
  CXX_VISIBILITY_PRESET hidden
)

set(OBS_SOURCES
  src/obs-capture.cpp src/obs-capture.hpp
  src/obs-graphics.hpp
  src/focus-order.cpp
  src/m4capture-source.cpp
  src/obs-dmabuf.cpp src/obs-dmabuf.hpp
  src/obs-video.cpp src/obs-video.hpp
)
add_library(m4capture MODULE src/plugin.cpp ${OBS_SOURCES})
target_link_libraries(m4capture PRIVATE
  capture-core OBS::libobs OBS::obs-frontend-api PkgConfig::DRM OpenGL::GL
)
target_compile_features(m4capture PRIVATE cxx_std_17)
target_compile_options(m4capture PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(m4capture PRIVATE
  M4CAPTURE_VERSION="${PROJECT_VERSION}"
)
set_target_properties(m4capture PROPERTIES
  PREFIX ""
  C_VISIBILITY_PRESET hidden
  CXX_VISIBILITY_PRESET hidden
  AUTOMOC ON
)

# CMAKE_INSTALL_PREFIX is the individual plugin directory in OBS's user plugins.
install(TARGETS m4capture LIBRARY DESTINATION bin/64bit)
install(DIRECTORY data/ DESTINATION data)
install(FILES LICENSE DESTINATION .)

option(M4CAPTURE_BUILD_TESTS "Build window matching and capture lifecycle tests" OFF)
if(M4CAPTURE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
