cmake_minimum_required(VERSION 3.16)
project(memkit C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(MEMKIT_SANITIZE "Build with ASan + UBSan" OFF)
option(MEMKIT_HARDEN "Enable double-free / foreign-pointer checks in pools" OFF)

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion -Werror)
if(MEMKIT_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

add_library(memkit STATIC src/arena.c src/pool.c src/heap.c)
target_include_directories(memkit PUBLIC include)
if(MEMKIT_HARDEN)
  target_compile_definitions(memkit PUBLIC MEMKIT_HARDEN)
endif()

add_executable(memkit_bench bench/bench.c)
target_link_libraries(memkit_bench PRIVATE memkit)

enable_testing()
add_executable(memkit_tests tests/test_memkit.c)
target_link_libraries(memkit_tests PRIVATE memkit)
add_test(NAME memkit_tests COMMAND memkit_tests)
