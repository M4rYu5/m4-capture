cmake_minimum_required(VERSION 3.24)

project(waylandconnect VERSION 0.2.0 LANGUAGES C CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "WaylandConnect currently targets Linux.")
endif()

find_package(libobs REQUIRED CONFIG)
find_package(obs-frontend-api REQUIRED CONFIG)
find_package(Qt6 REQUIRED COMPONENTS Widgets DBus)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3>=0.3.40)

add_library(waylandconnect MODULE
  src/plugin.cpp
  src/waylandconnect-source.cpp
  src/capture.cpp
  src/capture.hpp
  src/pipewire-capture.cpp
  src/pipewire-capture.hpp
  src/source-list.cpp
)
target_link_libraries(waylandconnect PRIVATE
  OBS::libobs OBS::obs-frontend-api Qt6::Widgets Qt6::DBus PkgConfig::PIPEWIRE
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

option(WAYLANDCONNECT_BUILD_TESTS "Build isolated portal and source lifecycle tests" OFF)
if(WAYLANDCONNECT_BUILD_TESTS)
  enable_testing()
  find_package(Qt6 REQUIRED COMPONENTS Test)
  pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client)
  add_executable(waylandconnect-tests
    tests/capture-test.cpp tests/video-fixture.cpp
    src/capture.cpp src/capture.hpp
    src/pipewire-capture.cpp src/pipewire-capture.hpp
    src/waylandconnect-source.cpp
    src/source-list.cpp
  )
  target_include_directories(waylandconnect-tests PRIVATE src)
  target_compile_features(waylandconnect-tests PRIVATE cxx_std_17)
  target_link_libraries(waylandconnect-tests PRIVATE
    OBS::libobs Qt6::Widgets Qt6::DBus Qt6::Test PkgConfig::PIPEWIRE PkgConfig::WAYLAND
  )
  set_target_properties(waylandconnect-tests PROPERTIES AUTOMOC ON)
  add_test(NAME capture-lifecycle COMMAND dbus-run-session -- $<TARGET_FILE:waylandconnect-tests>)
  set_tests_properties(capture-lifecycle PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
