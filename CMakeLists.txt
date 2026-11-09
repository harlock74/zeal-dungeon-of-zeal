cmake_minimum_required(VERSION 3.16)

# -------------------------------
# CMake Setup
# -------------------------------
set(APP_VERSION_MAJOR 0)
set(APP_VERSION_MINOR 0)
set(APP_VERSION_PATCH 0)
set(APP_NAME "zdungeon")
set(target_name "zdungeon")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/bin)
set(RUNTIME_ASSETS ${CMAKE_BINARY_DIR}/runtime/assets)
# Remove legacy test executables when upgrading an existing checkout.
file(REMOVE "${CMAKE_CURRENT_LIST_DIR}/bin/zdungeon_boot"
    "${CMAKE_CURRENT_LIST_DIR}/bin/zdungeon_test")

# -------------------------------
# Toolchain
# -------------------------------
set(ZOS_TOOLCHAIN sdcc)
include($ENV{ZOS_PATH}/cmake/zos_init.cmake)

# -------------------------------
# Project
# -------------------------------
project(${target_name} C ASM)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# -------------------------------
# Executable
# -------------------------------
set(ROOM_WALL_SOURCE "${CMAKE_BINARY_DIR}/generated/room_walls.c")
set_source_files_properties(${ROOM_WALL_SOURCE} PROPERTIES GENERATED TRUE)
set(GAME_SOURCES src/main.c src/game.c src/input.c src/video.c src/sound.c src/world.c
    ${ROOM_WALL_SOURCE})
foreach(app zdungeon zdungeon_boot zdungeon_test)
    add_executable(${app} ${GAME_SOURCES})
    target_include_directories(${app} PRIVATE src)
    # Naked assembly and the installed SDK use SDCC's register ABI.
    target_compile_options(${app} PRIVATE --sdcccall 1)
    if(NOT app STREQUAL zdungeon)
        target_compile_definitions(${app} PRIVATE ZDUNGEON_AUTOSTART_RESET=1)
        set_target_properties(${app} PROPERTIES EXCLUDE_FROM_ALL TRUE
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test-bin")
    endif()
    if(app STREQUAL zdungeon_test)
        target_sources(${app} PRIVATE src/autotest.c)
        target_compile_definitions(${app} PRIVATE ZDUNGEON_TEST=1 EMBER_TEST=1)
    endif()
endforeach()

# -------------------------------
# Include and link SDKs
# -------------------------------
zos_use(coreutils)
find_package(COREUTILS REQUIRED)
zos_use(zgdk)
find_package(ZGDK REQUIRED)
zos_use(zvb)
find_package(ZVB_SDK REQUIRED)
foreach(app zdungeon zdungeon_boot zdungeon_test)
    target_link_libraries(${app} PUBLIC zvb_gfx zvb_sound core zgdk)
endforeach()

# -------------------------------
# Generated Course Asset Symbols
# -------------------------------
# Tower streams rooms rather than embedding MiniGolf's courses. The SDK still
# owns all tile conversion; project packers add collision and typed objects.
set(SDK_TILED_CONVERTER "$ENV{ZVB_SDK_PATH}/tools/tiled2zeal/tiled2zeal.py")
add_custom_target(art_validate ALL
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/validate_assets.py --art
    VERBATIM)
add_custom_target(sdk_room_sources
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/rooms.py --extract
    DEPENDS art_validate VERBATIM)
set(SDK_ROOM_MAPS)
foreach(floor RANGE 1 8)
    foreach(row RANGE 0 2)
        foreach(column RANGE 0 2)
            set(room_name "f${floor}_r${row}_${column}")
            set(room_source "${CMAKE_BINARY_DIR}/maps/source/${room_name}.tmx")
            set(room_output "${CMAKE_BINARY_DIR}/maps/${room_name}.ztm")
            add_custom_command(OUTPUT "${room_output}"
                COMMAND ${Python3_EXECUTABLE} "${SDK_TILED_CONVERTER}"
                    -i "${room_source}" -o "${room_output}"
                DEPENDS assets/dungeon.tmx "${SDK_TILED_CONVERTER}"
                    assets/zdungeon.tsx sdk_room_sources
                VERBATIM)
            list(APPEND SDK_ROOM_MAPS "${room_output}")
        endforeach()
    endforeach()
endforeach()
set(SDK_SPLASH_MAPS)
foreach(layer RANGE 0 1)
    # SDK -l selects the saved Tiled layer ID, not its zero-based position.
    math(EXPR tiled_layer_id "${layer} + 1")
    set(splash_output "${CMAKE_BINARY_DIR}/maps/splash-layer${layer}.ztm")
    add_custom_command(OUTPUT "${splash_output}"
        COMMAND ${Python3_EXECUTABLE} "${SDK_TILED_CONVERTER}"
            -i "${CMAKE_SOURCE_DIR}/assets/splash.tmx"
            -o "${splash_output}" -l ${tiled_layer_id}
        DEPENDS assets/splash.tmx assets/zdungeon.tsx
            "${SDK_TILED_CONVERTER}" art_validate
        VERBATIM)
    list(APPEND SDK_SPLASH_MAPS "${splash_output}")
endforeach()
add_custom_target(sdk_maps DEPENDS ${SDK_ROOM_MAPS} ${SDK_SPLASH_MAPS})
add_custom_target(rooms ALL
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/rooms.py
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/splash.py
    DEPENDS sdk_maps VERBATIM)

# -------------------------------
# Assets
# -------------------------------
gif2zeal(${target_name} FILES
    "assets/zdungeon.gif"
    STRIP 69
    BIT 4
    COLORS 16
)
add_dependencies(zdungeon_zdungeon_asset art_validate)
# Keep the existing runtime paths; no game source/path changes are needed.
add_custom_command(OUTPUT ${RUNTIME_ASSETS}/zdungeon.zts ${RUNTIME_ASSETS}/zdungeon.ztp
    COMMAND ${CMAKE_COMMAND} -E make_directory ${RUNTIME_ASSETS}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_BINARY_DIR}/assets/zdungeon.zts ${RUNTIME_ASSETS}/zdungeon.zts
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_BINARY_DIR}/assets/zdungeon.ztp ${RUNTIME_ASSETS}/zdungeon.ztp
    DEPENDS ${CMAKE_BINARY_DIR}/assets/zdungeon.zts ${CMAKE_BINARY_DIR}/assets/zdungeon.ztp
    VERBATIM)
add_custom_target(art ALL DEPENDS ${RUNTIME_ASSETS}/zdungeon.zts ${RUNTIME_ASSETS}/zdungeon.ztp)
# SDK copy_assets flattens basenames, so preserve the streamed rooms directory.
add_custom_target(runtime_copy ALL
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_SOURCE_DIR}/bin/assets
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${RUNTIME_ASSETS} ${CMAKE_SOURCE_DIR}/bin/assets
    DEPENDS art rooms VERBATIM)
foreach(app zdungeon zdungeon_boot zdungeon_test)
    add_dependencies(${app} art rooms)
endforeach()

# -------------------------------
# Convert to binary
# -------------------------------
foreach(app zdungeon zdungeon_boot zdungeon_test)
    if(app STREQUAL zdungeon)
        set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")
    else()
        set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test-bin")
    endif()
    zos_add_outputs(${app})
    coreutils_add_post_build_artifacts(${app}
        OUT_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
        DEBUG_DIR "${CMAKE_BINARY_DIR}/debug" STRIP_BIN_EXTENSION ON)
    if(app STREQUAL zdungeon)
        add_custom_target(${app}_check ALL
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_memory.py
                ${CMAKE_BINARY_DIR}/debug/${app}.map ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${app}
            DEPENDS ${app}_bin VERBATIM)
    else()
        set_target_properties(${app}_bin PROPERTIES EXCLUDE_FROM_ALL TRUE)
        add_custom_target(${app}_check
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_memory.py
                ${CMAKE_BINARY_DIR}/debug/${app}.map ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${app}
            DEPENDS ${app}_bin VERBATIM)
    endif()
endforeach()
