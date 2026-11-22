cmake_minimum_required(VERSION 3.19)
project(AirBuzz LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(AB_SANITIZERS "Instrument macOS tests; disable only for native leaks analysis" ON)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND AB_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# The vendored snapshot has no nested Git metadata.
set(GIT_COMMIT_HASH b8a6f25)
set(GG_GIT_COMMIT_HASH b8a6f25)
set(GG_GIT_BRANCH_NAME vendored)
set(GG_GIT_COMMIT_COUNT 0)
add_subdirectory(Vendor/golden-gate/xp)
# The pinned Apple port releases dispatch storage but leaks its C allocation.
# Keep vendor sources untouched and wrap that one destructor in ab_core.
set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/Vendor/golden-gate/xp/common/ports/apple/gg_apple_semaphore.c"
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/Vendor/golden-gate/xp/common"
    PROPERTIES COMPILE_DEFINITIONS "GG_Semaphore_Destroy=AB_GG_Semaphore_DestroyStorage")
find_package(Threads REQUIRED)
add_library(ab_core STATIC Core/ab_session.c Core/ab_settings.c)
target_include_directories(ab_core PUBLIC Core)
target_link_libraries(ab_core PUBLIC gg-runtime Threads::Threads)
target_compile_options(ab_core PRIVATE -Wall -Wextra -Werror)

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_compile_definitions(ab_core PUBLIC AB_TESTING=1)
    add_executable(loopback_test Core/tests/loopback_test.c)
    target_link_libraries(loopback_test PRIVATE ab_core)
    target_compile_options(loopback_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    enable_testing()
    add_test(NAME loopback COMMAND loopback_test)
endif()
