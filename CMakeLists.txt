# luavortex — a Lua 5.4 frontend for the Vortex engine.
#
# Fetches the engine via FetchContent at a pinned commit and links the
# frontend plus the lvxc driver against vortex_core.
cmake_minimum_required(VERSION 3.28)

project(luavortex
    VERSION 0.1.0
    DESCRIPTION "Lua 5.4 frontend for the Vortex engine: Lua source to Universal Guest Bytecode"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# CEP&CC 6.3 warning set; warnings are errors for first-party code.
# GCC 14 fires -Wmissing-field-initializers spuriously on C++26 designated
# aggregate initialization (valid, all other members have defaults); the
# targeted disable keeps the CEP&CC 6.3 warning set with -Werror intact.
set(LVX_WARNINGS -Wall -Wextra -Wpedantic -Werror -Wno-missing-field-initializers)

include(FetchContent)

set(VORTEX_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(VORTEX_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    vortex
    GIT_REPOSITORY https://github.com/compilecomp/vortex.git
    GIT_TAG ff37b820799cda46c0051e2252b897dc4694e2f5
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(vortex)

# ---- frontend library ----------------------------------------------------------

add_library(luavortex_frontend STATIC
    src/lexer.cpp
    src/parser.cpp
    src/emitter.cpp
    src/emitter_emit.cpp
    src/emitter_emit_expr.cpp
    src/runtime.cpp
)
target_include_directories(luavortex_frontend PUBLIC include)
target_link_libraries(luavortex_frontend PUBLIC vortex_core)
target_compile_options(luavortex_frontend PRIVATE ${LVX_WARNINGS})

# ---- lvxc driver ---------------------------------------------------------------

add_executable(lvxc tools/lvxc/main.cpp)
target_link_libraries(lvxc PRIVATE luavortex_frontend)
target_compile_options(lvxc PRIVATE ${LVX_WARNINGS})

# ---- tests ---------------------------------------------------------------------

enable_testing()
add_test(NAME luavortex_suite
         COMMAND ${CMAKE_SOURCE_DIR}/tests/run_tests.sh $<TARGET_FILE:lvxc>)
