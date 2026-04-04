cmake_minimum_required(VERSION 3.20)

project(resurg C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra -Wpedantic -Werror)

if(WIN32)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

# --- Compiler library (librsg) ---

file(GLOB_RECURSE LIB_SRCS CONFIGURE_DEPENDS lib/*.c)
file(GLOB_RECURSE LIB_HDRS CONFIGURE_DEPENDS lib/*.h include/*.h)

add_library(rsg STATIC ${LIB_SRCS} ${LIB_HDRS})
target_include_directories(rsg PUBLIC include PRIVATE lib)

# --- Compiler driver ---

add_executable(resurg driver/main.c)
target_link_libraries(resurg PRIVATE rsg)
target_include_directories(resurg PRIVATE lib)

# --- Runtime (static library linked into compiled programs) ---

file(GLOB_RECURSE RUNTIME_SRCS CONFIGURE_DEPENDS runtime/*.c)
file(GLOB_RECURSE RUNTIME_HDRS CONFIGURE_DEPENDS runtime/*.h)

add_library(resurg_runtime STATIC ${RUNTIME_SRCS} ${RUNTIME_HDRS})
target_include_directories(resurg_runtime PRIVATE runtime/core)
