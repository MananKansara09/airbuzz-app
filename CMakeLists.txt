cmake_minimum_required(VERSION 3.19)
project(AirBuzz LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(AB_SANITIZERS "Instrument host tests; disable only for native leaks analysis" ON)
if(NOT ANDROID AND NOT CMAKE_CROSSCOMPILING AND AB_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
    if(NOT APPLE)
        # Vendored CoAP code calls memcpy(dst, NULL, 0) for empty tokens. glibc marks
        # memcpy arguments nonnull, so UBSan flags it here but not against Apple headers.
        add_compile_options(-fno-sanitize=nonnull-attribute)
    endif()
endif()

# The vendored snapshot has no nested Git metadata.
set(GIT_COMMIT_HASH b8a6f25)
set(GG_GIT_COMMIT_HASH b8a6f25)
set(GG_GIT_BRANCH_NAME vendored)
set(GG_GIT_COMMIT_COUNT 0)
if(NOT APPLE)
    include(Core/gg_posix_ports.cmake)
endif()
add_subdirectory(Vendor/golden-gate/xp)
if(APPLE)
    # The pinned Apple port releases dispatch storage but leaks its C allocation.
    # Keep vendor sources untouched and wrap that one destructor in ab_core.
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/Vendor/golden-gate/xp/common/ports/apple/gg_apple_semaphore.c"
        DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/Vendor/golden-gate/xp/common"
        PROPERTIES COMPILE_DEFINITIONS "GG_Semaphore_Destroy=AB_GG_Semaphore_DestroyStorage")
else()
    # GNU ld resolves static archives in order, and gg-module calls into
    # mbedtls through the generated init list. State that edge explicitly.
    cmake_policy(SET CMP0079 NEW)
    target_link_libraries(gg-module PUBLIC gg-mbedtls)
endif()
find_package(Threads REQUIRED)
add_library(ab_core STATIC Core/ab_session.c Core/ab_settings.c)
target_include_directories(ab_core PUBLIC Core)
target_link_libraries(ab_core PUBLIC gg-runtime Threads::Threads)
target_compile_options(ab_core PRIVATE -Wall -Wextra -Werror)
if(NOT APPLE)
    # Strict C11 hides clock_gettime and friends on glibc.
    target_compile_definitions(ab_core PRIVATE _POSIX_C_SOURCE=200809L)
endif()

if(ANDROID)
    add_library(airbuzz_jni SHARED Android/app/src/main/cpp/ab_jni.c)
    target_link_libraries(airbuzz_jni PRIVATE ab_core log)
    target_compile_options(airbuzz_jni PRIVATE -Wall -Wextra -Werror)
    # Android 15 devices use 16 KB pages.
    target_link_options(airbuzz_jni PRIVATE -Wl,-z,max-page-size=16384)
elseif(NOT CMAKE_CROSSCOMPILING)
    target_compile_definitions(ab_core PUBLIC AB_TESTING=1)
    add_executable(loopback_test Core/tests/loopback_test.c)
    target_link_libraries(loopback_test PRIVATE ab_core)
    target_compile_options(loopback_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    if(NOT APPLE)
        target_compile_definitions(loopback_test PRIVATE _POSIX_C_SOURCE=200809L)
    endif()
    enable_testing()
    add_test(NAME loopback COMMAND loopback_test)
endif()
