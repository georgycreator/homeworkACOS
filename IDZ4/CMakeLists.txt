cmake_minimum_required(VERSION 3.16)
project(IDZ4 C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_definitions(_POSIX_C_SOURCE=200809L)
add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wformat=2)

# Общая библиотека: сеть, протокол, лог и доменные модули (территория, артиллерия, конфигурация).
add_library(idz4_common STATIC
  src/common/util.c src/common/proto.c src/common/net.c src/common/log.c
  src/common/io.c src/common/colors.c src/common/signals.c
  src/common/grid.c src/common/artillery.c src/common/side.c src/common/config.c)
target_include_directories(idz4_common PUBLIC include)

add_executable(idz4_server src/server/server.c src/server/battle.c)
target_link_libraries(idz4_server idz4_common)

foreach(client side artillery observer)
  add_executable(idz4_${client} src/client/${client}_main.c src/client/client_common.c)
  target_link_libraries(idz4_${client} idz4_common)
endforeach()
