cmake_minimum_required(VERSION 2.6)

add_executable(udp_load udp_load.c)

set_property(TARGET udp_load PROPERTY C_STANDARD 11)
