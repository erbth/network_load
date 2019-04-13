cmake_minimum_required(VERSION 2.6)

find_package(Curses REQUIRED)

add_executable(network_load network_load.c)
include_directories(network_load ${CURSES_INCLUDE_DIR})
target_link_libraries(network_load ${CURSES_LIBRARIES})

set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -gdwarf-2")

set_property(TARGET network_load PROPERTY C_STANDARD 11)
