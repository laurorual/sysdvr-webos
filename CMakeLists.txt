cmake_minimum_required(VERSION 3.16)

project(sysdvr_webos VERSION 1.1.0 LANGUAGES C)

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
    pkg_check_modules(NDL QUIET NDL_directmedia)

    # Some current webosbrew NDK bundles do not ship the proprietary NDL
    # headers/link stub even though the real library is present on webOS TVs.
    # Fall back to webos-userland's declarations and symbol-only stub. Nothing
    # from that repository is bundled at runtime; the TV supplies the library.
    if(NOT NDL_FOUND)
        include(FetchContent)
        message(STATUS "NDL_directmedia missing from NDK; fetching webos-userland link stub")
        FetchContent_Declare(webos_userland
            GIT_REPOSITORY https://github.com/webosbrew/webos-userland.git
            GIT_TAG 00a5f8721e15d5c10030b99b2f3e73e0c527bcce
            GIT_SHALLOW TRUE
            SOURCE_SUBDIR do-not-add-this-project
        )
        FetchContent_MakeAvailable(webos_userland)

        add_library(webos_ndl_directmedia_stub SHARED
            "${webos_userland_SOURCE_DIR}/src/dummy.c"
        )
        set_target_properties(webos_ndl_directmedia_stub PROPERTIES
            OUTPUT_NAME NDL_directmedia
            SOVERSION 1
        )
        target_link_options(webos_ndl_directmedia_stub PRIVATE
            "${webos_userland_SOURCE_DIR}/src/NDL_directmedia.lds"
        )

        set(NDL_INCLUDE_DIRS "${webos_userland_SOURCE_DIR}/include/libndl-media")
        set(NDL_LIBRARY_DIRS "")
        set(NDL_LIBRARIES webos_ndl_directmedia_stub)
        set(NDL_CFLAGS_OTHER "")
    endif()

    # NDL DirectMedia API 2 headers in older toolchains declared Init(app_id,
    # callback); current webos-userland correctly declares Init(app_id). Detect
    # the declaration at configure time so both developer toolchains build.
    include(CheckCSourceCompiles)
    set(_SYSDVR_OLD_TRY_COMPILE_TARGET_TYPE "${CMAKE_TRY_COMPILE_TARGET_TYPE}")
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    set(CMAKE_REQUIRED_INCLUDES "${NDL_INCLUDE_DIRS}")
    set(CMAKE_REQUIRED_DEFINITIONS "-DNDL_DIRECTMEDIA_API_VERSION=2")
    check_c_source_compiles(
        "#include <NDL_directmedia.h>\nint main(void) { return NDL_DirectMediaInit(\"io.github.sysdvrwebos.client\"); }"
        SYSDVR_NDL_INIT_ONE_ARG
    )
    unset(CMAKE_REQUIRED_INCLUDES)
    unset(CMAKE_REQUIRED_DEFINITIONS)
    if(_SYSDVR_OLD_TRY_COMPILE_TARGET_TYPE)
        set(CMAKE_TRY_COMPILE_TARGET_TYPE "${_SYSDVR_OLD_TRY_COMPILE_TARGET_TYPE}")
    else()
        unset(CMAKE_TRY_COMPILE_TARGET_TYPE)
    endif()

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
        src/frametap_backend.c
        src/sft_audio.c
        src/sft_discovery.c
        src/sft_protocol.c
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

    if(NDL_LIBRARY_DIRS)
        target_link_directories(sysdvr-webos PRIVATE
            ${NDL_LIBRARY_DIRS}
        )
    endif()
    target_link_directories(sysdvr-webos PRIVATE
        ${SDL2_LIBRARY_DIRS}
    )

    target_compile_definitions(sysdvr-webos PRIVATE
        NDL_DIRECTMEDIA_API_VERSION=2
        _POSIX_C_SOURCE=200809L
        SYSDVR_APP_VERSION="1.1.0-beta"
    )

    if(SYSDVR_NDL_INIT_ONE_ARG)
        target_compile_definitions(sysdvr-webos PRIVATE
            SYSDVR_NDL_INIT_ONE_ARG=1
        )
    endif()

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
