cmake_minimum_required(VERSION 3.24)

project(waylandconnect VERSION 0.1.0 LANGUAGES C)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "WaylandConnect currently targets Linux.")
endif()

find_package(libobs REQUIRED CONFIG)

add_library(waylandconnect MODULE
  src/plugin.c
  src/waylandconnect-source.c
)
target_link_libraries(waylandconnect PRIVATE OBS::libobs)
target_compile_features(waylandconnect PRIVATE c_std_17)
target_compile_options(waylandconnect PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(waylandconnect PRIVATE
  WAYLANDCONNECT_VERSION="${PROJECT_VERSION}"
)
set_target_properties(waylandconnect PROPERTIES
  PREFIX ""
  C_VISIBILITY_PRESET hidden
)

# CMAKE_INSTALL_PREFIX is the individual plugin directory in OBS's user plugins.
install(TARGETS waylandconnect LIBRARY DESTINATION bin/64bit)
install(DIRECTORY data/ DESTINATION data)
install(FILES LICENSE DESTINATION .)
