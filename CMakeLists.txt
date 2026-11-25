cmake_minimum_required(VERSION 3.25)
project(matching_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(ME_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

add_compile_options(-Wall -Wextra -Wpedantic -Werror)
if(ME_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=address,undefined)
  add_compile_definitions(ME_SANITIZE=1)
endif()

# The engine is header-only: include/me/*.hpp
add_library(me_core INTERFACE)
target_include_directories(me_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# The flags string the benchmark prints (P1, P5): exactly what this build compiles with.
string(TOUPPER "${CMAKE_BUILD_TYPE}" ME_BUILD_TYPE_UPPER)
get_directory_property(ME_COMPILE_OPTIONS COMPILE_OPTIONS)
string(JOIN " " ME_COMPILE_OPTIONS_STR ${ME_COMPILE_OPTIONS})
set(ME_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${ME_BUILD_TYPE_UPPER}} -std=c++20 ${ME_COMPILE_OPTIONS_STR}")
string(STRIP "${ME_CXX_FLAGS}" ME_CXX_FLAGS)
target_compile_definitions(me_core INTERFACE
  ME_CXX_FLAGS="${ME_CXX_FLAGS}"
  ME_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
  ME_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")

# The command-line tool.
add_executable(me src/cli/main.cpp)
target_link_libraries(me PRIVATE me_core)

enable_testing()
add_subdirectory(tests)
