cmake_minimum_required(VERSION 3.20)

set(CMAKE_C_COMPILER clang)

project(resurg C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra -Wpedantic -Werror)

if(WIN32)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

# --- Compiler executable ---

file(GLOB COMPILER_SRCS src/*.c src/codegen/*.c src/parser/*.c src/sema/*.c)
file(GLOB COMPILER_HDRS src/*.h src/codegen/*.h src/parser/*.h src/sema/*.h)

add_executable(resurg ${COMPILER_SRCS} ${COMPILER_HDRS})
target_include_directories(resurg PRIVATE src)

# --- Runtime (static library linked into compiled programs) ---

file(GLOB RUNTIME_SRCS runtime/*.c)
file(GLOB RUNTIME_HDRS runtime/*.h)

add_library(resurg_runtime STATIC ${RUNTIME_SRCS} ${RUNTIME_HDRS})
target_include_directories(resurg_runtime PRIVATE runtime)
