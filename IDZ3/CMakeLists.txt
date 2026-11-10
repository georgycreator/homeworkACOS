cmake_minimum_required(VERSION 3.20)
project(IDZ3 C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

add_compile_definitions(_POSIX_C_SOURCE=200809L)

set(IDZ3_CORE_SOURCES
    src/process_simulation.c
    src/ipc.c
    src/io.c
    src/log.c
    src/signals.c
    src/grid.c
    src/artillery.c
    src/side.c
    src/config.c
    src/colors.c
)

add_library(idz3_core STATIC ${IDZ3_CORE_SOURCES})
target_include_directories(idz3_core PUBLIC include)
target_compile_options(idz3_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2 -g)
target_link_libraries(idz3_core PUBLIC pthread rt)

add_executable(IDZ3 src/main.c)
target_compile_definitions(IDZ3 PRIVATE DEFAULT_IPC_MODE=IPC_SEM_NAMED)
target_link_libraries(IDZ3 PRIVATE idz3_core)

add_executable(IDZ3_named src/main.c)
target_compile_definitions(IDZ3_named PRIVATE DEFAULT_IPC_MODE=IPC_SEM_NAMED)
target_link_libraries(IDZ3_named PRIVATE idz3_core)

add_executable(IDZ3_unnamed src/main.c)
target_compile_definitions(IDZ3_unnamed PRIVATE DEFAULT_IPC_MODE=IPC_SEM_UNNAMED)
target_link_libraries(IDZ3_unnamed PRIVATE idz3_core)

add_executable(IDZ3_init src/independent/init_main.c)
target_link_libraries(IDZ3_init PRIVATE idz3_core)
add_executable(IDZ3_coordinator src/independent/coordinator_main.c)
target_link_libraries(IDZ3_coordinator PRIVATE idz3_core)
add_executable(IDZ3_side src/independent/side_main.c)
target_link_libraries(IDZ3_side PRIVATE idz3_core)
add_executable(IDZ3_artillery src/independent/artillery_main.c)
target_link_libraries(IDZ3_artillery PRIVATE idz3_core)

add_executable(IDZ3_cleanup src/independent/cleanup_main.c)
target_link_libraries(IDZ3_cleanup PRIVATE idz3_core)
