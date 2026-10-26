cmake_minimum_required(VERSION 3.24)

project(waylandconnect VERSION 0.3.0 LANGUAGES C CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "WaylandConnect currently targets Linux.")
endif()

find_package(libobs REQUIRED CONFIG)
find_package(obs-frontend-api REQUIRED CONFIG)
find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3>=0.3.40)
pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client)
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
set(PROTOCOL_SOURCES)
foreach(PROTOCOL plasma-window-management zkde-screencast-unstable-v1)
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
  src/kwin-connection.cpp src/kwin-connection.hpp
  src/pipewire-capture.cpp src/pipewire-capture.hpp
  src/window-rule.cpp src/window-rule.hpp
  src/focus-order.cpp
  src/waylandconnect-source.cpp src/source-list.cpp
  ${PROTOCOL_SOURCES}
)

add_library(waylandconnect MODULE
  src/plugin.cpp
  ${CAPTURE_SOURCES}
)
target_include_directories(waylandconnect PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(waylandconnect PRIVATE
  OBS::libobs OBS::obs-frontend-api Qt6::Widgets PkgConfig::PIPEWIRE PkgConfig::WAYLAND
)
target_compile_features(waylandconnect PRIVATE cxx_std_17)
target_compile_options(waylandconnect PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(waylandconnect PRIVATE
  WAYLANDCONNECT_VERSION="${PROJECT_VERSION}"
)
set_target_properties(waylandconnect PROPERTIES
  PREFIX ""
  C_VISIBILITY_PRESET hidden
  CXX_VISIBILITY_PRESET hidden
  AUTOMOC ON
)

# CMAKE_INSTALL_PREFIX is the individual plugin directory in OBS's user plugins.
install(TARGETS waylandconnect LIBRARY DESTINATION bin/64bit)
install(DIRECTORY data/ DESTINATION data)
install(FILES LICENSE DESTINATION .)

option(WAYLANDCONNECT_BUILD_TESTS "Build window matching and capture lifecycle tests" OFF)
if(WAYLANDCONNECT_BUILD_TESTS)
  enable_testing()
  find_package(Qt6 REQUIRED COMPONENTS Test)
  pkg_check_modules(WAYLAND_SERVER REQUIRED IMPORTED_TARGET wayland-server)
  set(TEST_PROTOCOL_HEADERS)
  foreach(PROTOCOL plasma-window-management zkde-screencast-unstable-v1)
    set(HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/${PROTOCOL}-server-protocol.h")
    add_custom_command(OUTPUT "${HEADER}"
      COMMAND ${WAYLAND_SCANNER} server-header "${CMAKE_CURRENT_SOURCE_DIR}/protocols/${PROTOCOL}.xml" "${HEADER}"
      DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/protocols/${PROTOCOL}.xml" VERBATIM)
    list(APPEND TEST_PROTOCOL_HEADERS "${HEADER}")
  endforeach()
  add_executable(waylandconnect-tests
    tests/capture-test.cpp tests/video-fixture.cpp tests/kwin-fixture.cpp
    ${CAPTURE_SOURCES}
    ${TEST_PROTOCOL_HEADERS}
  )
  target_include_directories(waylandconnect-tests PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/generated")
  target_compile_features(waylandconnect-tests PRIVATE cxx_std_17)
  target_link_libraries(waylandconnect-tests PRIVATE
    OBS::libobs Qt6::Widgets Qt6::Test PkgConfig::PIPEWIRE PkgConfig::WAYLAND PkgConfig::WAYLAND_SERVER
  )
  set_target_properties(waylandconnect-tests PROPERTIES AUTOMOC ON)
  add_test(NAME capture-lifecycle COMMAND $<TARGET_FILE:waylandconnect-tests>)
  set_tests_properties(capture-lifecycle PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
