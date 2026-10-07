cmake_minimum_required(VERSION 3.20)
project(IDZ1 C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(SOURCES
    src/main.c
    src/io.c
    src/log.c
    src/signals.c
    src/grid.c
    src/artillery.c
    src/side.c
    src/config.c
    src/colors.c
)

add_executable(IDZ1 ${SOURCES})

target_include_directories(IDZ1 PRIVATE include)

target_compile_options(IDZ1 PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -g
)
