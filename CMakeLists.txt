cmake_minimum_required(VERSION 3.24)

project(KWinCapture VERSION 0.3.0 LANGUAGES C CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "KWinCapture currently targets Linux.")
endif()

find_package(libobs 32 REQUIRED CONFIG)
find_package(obs-frontend-api REQUIRED CONFIG)
find_package(Qt6 REQUIRED COMPONENTS Core)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3>=1.2)
pkg_check_modules(DRM REQUIRED IMPORTED_TARGET libdrm)
find_package(OpenGL REQUIRED)
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
  src/capture-runtime.cpp
  src/kwin-connection.cpp src/kwin-connection.hpp
  src/pipewire-capture.cpp src/pipewire-capture.hpp
  src/video-frame.cpp src/video-frame.hpp
  src/frame-buffer.cpp src/frame-buffer.hpp
  src/window-rule.cpp src/window-rule.hpp
  ${PROTOCOL_SOURCES}
)

add_library(capture-core STATIC ${CAPTURE_SOURCES})
target_include_directories(capture-core PUBLIC src PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(capture-core PUBLIC Qt6::Core PkgConfig::PIPEWIRE PkgConfig::WAYLAND)
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
  src/focus-order.cpp
  src/kwincapture-source.cpp
  src/obs-dmabuf.cpp src/obs-dmabuf.hpp
  src/obs-video.cpp src/obs-video.hpp
)
add_library(kwincapture MODULE src/plugin.cpp ${OBS_SOURCES})
target_link_libraries(kwincapture PRIVATE
  capture-core OBS::libobs OBS::obs-frontend-api PkgConfig::DRM OpenGL::GL
)
target_compile_features(kwincapture PRIVATE cxx_std_17)
target_compile_options(kwincapture PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(kwincapture PRIVATE
  KWINCAPTURE_VERSION="${PROJECT_VERSION}"
)
set_target_properties(kwincapture PROPERTIES
  # Keep the installed module filename stable for existing installations.
  OUTPUT_NAME "waylandconnect"
  PREFIX ""
  C_VISIBILITY_PRESET hidden
  CXX_VISIBILITY_PRESET hidden
  AUTOMOC ON
)

# CMAKE_INSTALL_PREFIX is the individual plugin directory in OBS's user plugins.
install(TARGETS kwincapture LIBRARY DESTINATION bin/64bit)
install(DIRECTORY data/ DESTINATION data)
install(FILES LICENSE DESTINATION .)

option(KWINCAPTURE_BUILD_TESTS "Build window matching and capture lifecycle tests" OFF)
if(KWINCAPTURE_BUILD_TESTS)
  enable_testing()
  find_package(OpenGL REQUIRED COMPONENTS EGL)
  find_package(Qt6 REQUIRED COMPONENTS Test Widgets)
  add_executable(window-rule-tests tests/window-rule-test.cpp src/window-rule.cpp)
  target_include_directories(window-rule-tests PRIVATE src)
  target_compile_features(window-rule-tests PRIVATE cxx_std_17)
  target_link_libraries(window-rule-tests PRIVATE Qt6::Test)
  set_target_properties(window-rule-tests PROPERTIES AUTOMOC ON)
  add_test(NAME window-rule COMMAND $<TARGET_FILE:window-rule-tests>)
  add_executable(frame-buffer-tests tests/frame-buffer-test.cpp src/frame-buffer.cpp)
  target_include_directories(frame-buffer-tests PRIVATE src)
  target_compile_features(frame-buffer-tests PRIVATE cxx_std_17)
  target_link_libraries(frame-buffer-tests PRIVATE Qt6::Test PkgConfig::PIPEWIRE)
  set_target_properties(frame-buffer-tests PROPERTIES AUTOMOC ON)
  add_test(NAME frame-buffer COMMAND $<TARGET_FILE:frame-buffer-tests>)
  add_executable(video-frame-tests tests/video-frame-test.cpp src/video-frame.cpp)
  target_include_directories(video-frame-tests PRIVATE src)
  target_compile_features(video-frame-tests PRIVATE cxx_std_17)
  target_link_libraries(video-frame-tests PRIVATE Qt6::Test PkgConfig::PIPEWIRE)
  set_target_properties(video-frame-tests PROPERTIES AUTOMOC ON)
  add_test(NAME video-frame COMMAND $<TARGET_FILE:video-frame-tests>)
  pkg_check_modules(WAYLAND_SERVER REQUIRED IMPORTED_TARGET wayland-server)
  set(TEST_PROTOCOL_HEADERS)
  foreach(PROTOCOL plasma-window-management zkde-screencast-unstable-v1)
    set(HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/${PROTOCOL}-server-protocol.h")
    add_custom_command(OUTPUT "${HEADER}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
      COMMAND ${WAYLAND_SCANNER} server-header "${CMAKE_CURRENT_SOURCE_DIR}/protocols/${PROTOCOL}.xml" "${HEADER}"
      DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/protocols/${PROTOCOL}.xml" VERBATIM)
    list(APPEND TEST_PROTOCOL_HEADERS "${HEADER}")
  endforeach()
  add_executable(capture-controller-tests
    tests/capture-controller-test.cpp tests/video-fixture.cpp tests/kwin-fixture.cpp
    ${TEST_PROTOCOL_HEADERS}
  )
  target_include_directories(capture-controller-tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
  target_link_libraries(capture-controller-tests PRIVATE capture-core Qt6::Test PkgConfig::WAYLAND_SERVER)
  target_compile_options(capture-controller-tests PRIVATE -Wall -Wextra -Wpedantic)
  set_target_properties(capture-controller-tests PROPERTIES AUTOMOC ON)
  add_test(NAME capture-controller COMMAND $<TARGET_FILE:capture-controller-tests>)

  add_executable(kwincapture-tests
    tests/capture-test.cpp tests/video-fixture.cpp tests/kwin-fixture.cpp tests/dmabuf-fixture.cpp
    ${OBS_SOURCES}
    ${TEST_PROTOCOL_HEADERS}
  )
  target_include_directories(kwincapture-tests PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/generated")
  target_compile_features(kwincapture-tests PRIVATE cxx_std_17)
  target_link_libraries(kwincapture-tests PRIVATE
    capture-core OBS::libobs Qt6::Widgets Qt6::Test PkgConfig::WAYLAND_SERVER PkgConfig::DRM OpenGL::GL OpenGL::EGL
  )
  target_link_options(kwincapture-tests PRIVATE
    "LINKER:--wrap=gs_texture_create_from_dmabuf" "LINKER:--wrap=gs_texture_destroy"
  )
  set_target_properties(kwincapture-tests PROPERTIES AUTOMOC ON)
  add_test(NAME obs-integration COMMAND $<TARGET_FILE:kwincapture-tests>)
  set_tests_properties(obs-integration PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
