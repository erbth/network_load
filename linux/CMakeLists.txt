cmake_minimum_required(VERSION 2.6)

find_package(Curses REQUIRED)

add_executable(udp_load udp_load.c)
include_directories(udp_load ${CURSES_INCLUDE_DIR})
target_link_libraries(udp_load ${CURSES_LIBRARIES})

set_property(TARGET udp_load PROPERTY C_STANDARD 11)
