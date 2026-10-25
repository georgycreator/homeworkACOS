cmake_minimum_required(VERSION 3.20)
project(IDZ2 C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(SOURCES
    src/main.c
    src/simulation.c
    src/io.c
    src/log.c
    src/signals.c
    src/grid.c
    src/artillery.c
    src/side.c
    src/config.c
    src/colors.c
)

add_executable(IDZ2 ${SOURCES})

target_include_directories(IDZ2 PRIVATE include)

find_package(Threads REQUIRED)
target_link_libraries(IDZ2 PRIVATE Threads::Threads)

target_compile_options(IDZ2 PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -g
)
