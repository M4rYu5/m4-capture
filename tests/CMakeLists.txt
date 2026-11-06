find_package(Qt6 REQUIRED COMPONENTS Test)

# Executables stay at the build root for direct invocation and profiling scripts.
function(add_capture_test name source support label)
  add_executable(${name}-tests ${source})
  target_link_libraries(${name}-tests PRIVATE ${support})
  target_compile_options(${name}-tests PRIVATE -Wall -Wextra -Wpedantic)
  set_target_properties(${name}-tests PROPERTIES
    AUTOMOC ON RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
  add_test(NAME ${name} COMMAND $<TARGET_FILE:${name}-tests>)
  set_tests_properties(${name} PROPERTIES LABELS "${label}" TIMEOUT 120)
  if("obs" IN_LIST label)
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
  endif()
endfunction()

add_library(unit-test-support INTERFACE)
target_include_directories(unit-test-support INTERFACE "${PROJECT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_compile_features(unit-test-support INTERFACE cxx_std_17)
target_link_libraries(unit-test-support INTERFACE Qt6::Test PkgConfig::PIPEWIRE)

  pkg_check_modules(WAYLAND_SERVER REQUIRED IMPORTED_TARGET wayland-server)
  set(TEST_PROTOCOL_HEADERS)
  foreach(protocol ${CAPTURE_PROTOCOLS})
    set(header "${PROJECT_BINARY_DIR}/generated/${protocol}-server-protocol.h")
    set(xml "${PROJECT_SOURCE_DIR}/protocols/${protocol}.xml")
    add_custom_command(OUTPUT "${header}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${PROJECT_BINARY_DIR}/generated"
      COMMAND ${WAYLAND_SCANNER} server-header "${xml}" "${header}"
      DEPENDS "${xml}" VERBATIM)
    list(APPEND TEST_PROTOCOL_HEADERS "${header}")
  endforeach()

  add_library(kwin-test-support STATIC
    support/kwin-fixture.cpp support/kwin-test-fixture.hpp support/capture-test-fixture.hpp
    ${TEST_PROTOCOL_HEADERS})
  target_include_directories(kwin-test-support PRIVATE "${PROJECT_BINARY_DIR}/generated")
  target_link_libraries(kwin-test-support PUBLIC capture-core unit-test-support PkgConfig::WAYLAND_SERVER)
  set_target_properties(kwin-test-support PROPERTIES AUTOMOC ON)

  add_library(pipewire-test-support STATIC
    support/pipewire-server.cpp support/video-fixture.cpp support/audio-fixture.cpp)
  target_link_libraries(pipewire-test-support PUBLIC capture-core unit-test-support)

  add_library(protocol-test-support INTERFACE)
  target_link_libraries(protocol-test-support INTERFACE kwin-test-support pipewire-test-support)
  find_package(OpenGL REQUIRED COMPONENTS EGL)
  find_package(Qt6 REQUIRED COMPONENTS Widgets)
  set(obs_test_sources ${OBS_SOURCES})
  list(TRANSFORM obs_test_sources PREPEND "${PROJECT_SOURCE_DIR}/")
  add_library(obs-test-support STATIC
    support/obs-test-fixture.cpp support/obs-test-fixture.hpp support/live-obs-fixture.hpp
    support/dmabuf-fixture.cpp ${obs_test_sources})
  target_link_libraries(obs-test-support PUBLIC
    capture-core unit-test-support OBS::libobs Qt6::Widgets PkgConfig::DRM OpenGL::GL OpenGL::EGL)
  target_link_options(obs-test-support INTERFACE
    "LINKER:--wrap=gs_texture_create_from_dmabuf" "LINKER:--wrap=gs_texture_destroy")
  set_target_properties(obs-test-support PROPERTIES AUTOMOC ON)

add_library(simulated-obs-test-support STATIC support/simulated-obs-fixture.cpp)
target_link_libraries(simulated-obs-test-support PUBLIC obs-test-support protocol-test-support)
set_target_properties(simulated-obs-test-support PROPERTIES AUTOMOC ON)

foreach(name window-rule frame-buffer video-frame)
  add_capture_test(${name} "${name}-test.cpp;${PROJECT_SOURCE_DIR}/src/${name}.cpp" unit-test-support "unit;headless")
endforeach()
add_capture_test(capture-controller capture-controller-test.cpp protocol-test-support "integration;headless")
add_capture_test(pipewire-audio pipewire-audio-test.cpp pipewire-test-support "integration;headless")
add_executable(m4capture-tests capture-test.cpp)
target_link_libraries(m4capture-tests PRIVATE simulated-obs-test-support)
set_target_properties(m4capture-tests PROPERTIES AUTOMOC ON RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
add_test(NAME obs-integration COMMAND $<TARGET_FILE:m4capture-tests>)
set_tests_properties(obs-integration PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS "obs;graphics" TIMEOUT 120)

add_executable(m4capture-live-tests live-capture-test.cpp)
target_link_libraries(m4capture-live-tests PRIVATE obs-test-support)
set_target_properties(m4capture-live-tests PROPERTIES AUTOMOC ON RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
