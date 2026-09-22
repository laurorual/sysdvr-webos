cmake_minimum_required(VERSION 3.16)

project(sysdvr_webos VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(SYSDVR_BUILD_HOST_TEST "Build Linux protocol smoke test" ON)
option(SYSDVR_BUILD_WEBOS "Build native webOS client" OFF)

add_library(sysdvr_core STATIC
    src/net.c
    src/protocol.c
    src/discovery.c
    src/session_manager.c
    src/log.c
)

target_include_directories(sysdvr_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_definitions(sysdvr_core PRIVATE
    _POSIX_C_SOURCE=200112L
)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(sysdvr_core PRIVATE
        -Wall -Wextra -Wpedantic -Wshadow
    )
endif()

if(SYSDVR_BUILD_HOST_TEST)
    add_executable(sysdvr-net-test tools/net_test.c)
    target_link_libraries(sysdvr-net-test PRIVATE sysdvr_core)
endif()

if(SYSDVR_BUILD_WEBOS)
    find_package(PkgConfig REQUIRED)

    pkg_check_modules(SDL2 REQUIRED sdl2)
    pkg_check_modules(NDL REQUIRED NDL_directmedia)

    find_path(SDL2_TTF_INCLUDE_DIR
        NAMES SDL_ttf.h
        HINTS ${SDL2_INCLUDE_DIRS}
        PATH_SUFFIXES SDL2
    )

    find_library(SDL2_TTF_LIBRARY
        NAMES SDL2_ttf SDL2_ttf-2.0
    )

    if(NOT SDL2_TTF_INCLUDE_DIR OR NOT SDL2_TTF_LIBRARY)
        message(FATAL_ERROR
            "SDL2_ttf development files were not found in the webOS toolchain")
    endif()

    add_executable(sysdvr-webos
        src/webos_main.c
        src/ndl_media.c
        src/sdl_shell.c
        src/ui.c
    )

    target_include_directories(sysdvr-webos PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${SDL2_INCLUDE_DIRS}
        ${NDL_INCLUDE_DIRS}
        ${SDL2_TTF_INCLUDE_DIR}
    )

    target_link_directories(sysdvr-webos PRIVATE
        ${SDL2_LIBRARY_DIRS}
        ${NDL_LIBRARY_DIRS}
    )

    target_compile_definitions(sysdvr-webos PRIVATE
        NDL_DIRECTMEDIA_API_VERSION=2
        _POSIX_C_SOURCE=200809L
        SYSDVR_APP_VERSION="1.0.0"
    )

    target_compile_options(sysdvr-webos PRIVATE
        ${SDL2_CFLAGS_OTHER}
        ${NDL_CFLAGS_OTHER}
        -Wno-deprecated-declarations
    )

    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(sysdvr-webos PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow
        )
    endif()

    target_link_libraries(sysdvr-webos PRIVATE
        sysdvr_core
        ${SDL2_LIBRARIES}
        ${SDL2_TTF_LIBRARY}
        ${NDL_LIBRARIES}
        pthread
        m
    )

    set_target_properties(sysdvr-webos PROPERTIES
        INSTALL_RPATH "$ORIGIN"
        BUILD_WITH_INSTALL_RPATH TRUE
    )
endif()
