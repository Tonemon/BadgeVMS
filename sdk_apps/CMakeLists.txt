cmake_minimum_required(VERSION 3.16)

get_property(SDK_LIBRARIES GLOBAL PROPERTY SDK_LIBRARIES)
get_property(SDK_STAGING_DIR GLOBAL PROPERTY SDK_STAGING_DIR)
set(SDK_LIB_DIR ${SDK_STAGING_DIR}/lib)
set(SDK_INCLUDE_DIR ${SDK_STAGING_DIR}/include)

set(APP_COMPILE_FLAGS
    -O2
    -fPIC
    -flto
    -fdata-sections
    -ffunction-sections
    -fno-builtin
    -fno-builtin-function
    -fno-jump-tables
    -fno-tree-switch-conversion
    -fstrict-volatile-bitfields
    -fvisibility=hidden
    -g3
    -mabi=ilp32f
    -march=rv32imafc_zicsr_zifencei
    -nostartfiles
    -nostdlib
    -shared
)

set(APP_LINK_FLAGS
    -Wl,--strip-debug
    -Wl,--gc-sections
    -e main
)

set(APP_ELF_DIR ${CMAKE_BINARY_DIR}/app_elfs)
file(MAKE_DIRECTORY ${APP_ELF_DIR})

function(build_app app_name)
    cmake_parse_arguments(APP "PREINSTALL" "" "SOURCES;LIBRARIES;COMPILE_DEFINITIONS" ${ARGN})

    if(NOT APP_SOURCES)
        message(FATAL_ERROR "build_app: SOURCES must be specified for ${app_name}")
    endif()

    set(APP_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/${app_name})

    # Process sources
    set(ABSOLUTE_SOURCES "")
    foreach(SOURCE ${APP_SOURCES})
        if(IS_ABSOLUTE ${SOURCE})
            list(APPEND ABSOLUTE_SOURCES ${SOURCE})
        else()
            list(APPEND ABSOLUTE_SOURCES ${APP_SOURCE_DIR}/${SOURCE})
        endif()
    endforeach()

    set(LIBRARY_FLAGS "")
    if(APP_LIBRARIES)
        list(APPEND LIBRARY_FLAGS -L${SDK_LIB_DIR})
        foreach(LIB ${APP_LIBRARIES})
            list(APPEND LIBRARY_FLAGS -l${LIB})
            # This prevents unused symbols from ending up in the final elf
            list(APPEND LIBRARY_FLAGS -Wl,--exclude-libs,lib${LIB}.a)
        endforeach()
    endif()

    set(DEFINE_FLAGS "")
    if(APP_COMPILE_DEFINITIONS)
        foreach(DEF ${APP_COMPILE_DEFINITIONS})
            list(APPEND DEFINE_FLAGS -D${DEF})
        endforeach()
    endif()

    add_custom_command(
        OUTPUT ${APP_ELF_DIR}/${app_name}.elf
        COMMAND ${CMAKE_C_COMPILER}
            ${APP_COMPILE_FLAGS}
            ${APP_LINK_FLAGS}
            -isystem${SDK_INCLUDE_DIR}
            -o ${APP_ELF_DIR}/${app_name}.elf
            -I ${APP_SOURCE_DIR}
            ${ABSOLUTE_SOURCES}
            ${LIBRARY_FLAGS}
            ${DEFINE_FLAGS}
        DEPENDS
         ${ABSOLUTE_SOURCES}
         final_sdk_staging
        COMMENT "Building app ELF: ${app_name}"
        VERBATIM
    )
    add_custom_target(build_app_${app_name} ALL
        DEPENDS
         ${APP_ELF_DIR}/${app_name}.elf
    )

    if(APP_PREINSTALL)
        get_property(STORAGE_STAGING_DIR GLOBAL PROPERTY STORAGE_STAGING_DIR)
        set(STAGING_APPS_DIR ${STORAGE_STAGING_DIR}/BADGEVMS/APPS)
        add_custom_target(storage_staging_add_app_${app_name} ALL
            COMMAND ${CMAKE_COMMAND} -E rm -rf -- ${STORAGE_STAGING_DIR}/${app_name}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${STORAGE_STAGING_DIR}
            COMMAND ${CMAKE_COMMAND} -E copy ${APP_SOURCE_DIR}/manifest.json
                                            ${STAGING_APPS_DIR}/${app_name}.json
            COMMAND ${CMAKE_COMMAND} -E copy_directory ${APP_SOURCE_DIR}/storage_skel
                                                    ${STAGING_APPS_DIR}/${app_name}
            COMMAND ${CMAKE_COMMAND} -E copy ${APP_ELF_DIR}/${app_name}.elf
                                            ${STAGING_APPS_DIR}/${app_name}/${app_name}.elf
            COMMAND ${CMAKE_STRIP} ${STAGING_APPS_DIR}/${app_name}/${app_name}.elf
            DEPENDS
            build_app_${app_name}
            init_storage_staging
            ${APP_ELF_DIR}/${app_name}.elf
            BYPRODUCTS
            ${STORAGE_STAGING_DIR}/BADGEVMS/APPS/${app_name}.json
            ${STORAGE_STAGING_DIR}/BADGEVMS/APPS/${app_name}
            COMMENT "Copying ${app_name} manifest, skel and stripped elf to storage staging"
            VERBATIM
        )
        add_dependencies(final_storage_staging storage_staging_add_app_${app_name})
    endif()

    set_property(GLOBAL APPEND PROPERTY SDK_APP_ELFS ${APP_ELF_DIR}/${app_name}.elf)
    set_property(GLOBAL APPEND PROPERTY SDK_APP_BUILD_TARGETS build_app_${app_name})
endfunction()

#
# Core apps
#

# Old OTA updater preinstalled at handout, doesn't build anymore due to api changes
#build_app(ota_wifi_update
#    SOURCES
#     ota_wifi_update.c
#     thirdparty/microui.c
#)

# Current updater app
build_app(why2025_ota PREINSTALL
    SOURCES
     main.c
     window.c
     ota_update.c
     thirdparty/cJSON.c
    LIBRARIES
     sdl3
)

build_app(badgevms_launcher PREINSTALL
    SOURCES
     main.c
     image.c
)

build_app(badgevms_settings PREINSTALL
    SOURCES
     main.c
    LIBRARIES
     sdl3
)

#
# Regular apps
#

build_app(why2025_sponsors PREINSTALL
    SOURCES
     main.c
     image.c
)

build_app(why2025_namebadge PREINSTALL
    SOURCES
     main.c
     image.c
)

build_app(doomgeneric PREINSTALL
    SOURCES
     doomgeneric/am_map.c
     doomgeneric/d_event.c
     doomgeneric/d_items.c
     doomgeneric/d_iwad.c
     doomgeneric/d_loop.c
     doomgeneric/d_main.c
     doomgeneric/d_mode.c
     doomgeneric/d_net.c
     doomgeneric/doomdef.c
     doomgeneric/doomgeneric.c
     doomgeneric/doomgeneric_badgevms.c
     doomgeneric/doomstat.c
     doomgeneric/dstrings.c
     doomgeneric/dummy.c
     doomgeneric/f_finale.c
     doomgeneric/f_wipe.c
     doomgeneric/g_game.c
     doomgeneric/gusconf.c
     doomgeneric/hu_lib.c
     doomgeneric/hu_stuff.c
     doomgeneric/i_cdmus.c
     doomgeneric/icon.c
     doomgeneric/i_endoom.c
     doomgeneric/i_input.c
     doomgeneric/i_joystick.c
     doomgeneric/info.c
     doomgeneric/i_scale.c
     doomgeneric/i_sound.c
     doomgeneric/i_system.c
     doomgeneric/i_timer.c
     doomgeneric/i_video.c
     doomgeneric/m_argv.c
     doomgeneric/m_bbox.c
     doomgeneric/m_cheat.c
     doomgeneric/m_config.c
     doomgeneric/m_controls.c
     doomgeneric/memio.c
     doomgeneric/m_fixed.c
     doomgeneric/m_menu.c
     doomgeneric/m_misc.c
     doomgeneric/m_random.c
     doomgeneric/mus2mid.c
     doomgeneric/p_ceilng.c
     doomgeneric/p_doors.c
     doomgeneric/p_enemy.c
     doomgeneric/p_floor.c
     doomgeneric/p_inter.c
     doomgeneric/p_lights.c
     doomgeneric/p_map.c
     doomgeneric/p_maputl.c
     doomgeneric/p_mobj.c
     doomgeneric/p_plats.c
     doomgeneric/p_pspr.c
     doomgeneric/p_saveg.c
     doomgeneric/p_setup.c
     doomgeneric/p_sight.c
     doomgeneric/p_spec.c
     doomgeneric/p_switch.c
     doomgeneric/p_telept.c
     doomgeneric/p_tick.c
     doomgeneric/p_user.c
     doomgeneric/r_bsp.c
     doomgeneric/r_data.c
     doomgeneric/r_draw.c
     doomgeneric/r_main.c
     doomgeneric/r_plane.c
     doomgeneric/r_segs.c
     doomgeneric/r_sky.c
     doomgeneric/r_things.c
     doomgeneric/sha1.c
     doomgeneric/sounds.c
     doomgeneric/s_sound.c
     doomgeneric/statdump.c
     doomgeneric/st_lib.c
     doomgeneric/st_stuff.c
     doomgeneric/tables.c
     doomgeneric/v_video.c
     doomgeneric/w_checksum.c
     doomgeneric/w_file.c
     doomgeneric/w_file_stdc.c
     doomgeneric/wi_stuff.c
     doomgeneric/w_main.c
     doomgeneric/w_wad.c
     doomgeneric/z_zone.c
)

#
# Diagnostic apps
#

build_app(hardware_test PREINSTALL
    SOURCES
     thirdparty/microui.c
     test_badge.c
     test_drawing_helper.c
     test_keyboard.c
     run_tests.c
)

build_app(memtester PREINSTALL
    SOURCES
     memtester.c
     tests.c
     getopt.c
    COMPILE_DEFINITIONS
     "__badgevms__=(1)"
     "TEST_NARROW_WRITES=(1)"
)

build_app(bench_basic_a PREINSTALL
    SOURCES
     main.c
)

build_app(bench_basic_b PREINSTALL
    SOURCES
     main.c
)

#
# Example apps
#

build_app(test_appdb PREINSTALL
    SOURCES
     main.c
)

build_app(test_bmi270 PREINSTALL
    SOURCES
     main.c
)

build_app(test_bme690 PREINSTALL
    SOURCES
     main.c
)

build_app(test_curl PREINSTALL
    SOURCES
     main.c
)

build_app(test_framebuffer PREINSTALL
    SOURCES
     main.c
)

build_app(hello PREINSTALL
    SOURCES 
     main.c
)

build_app(test_process PREINSTALL
    SOURCES
     main.c
)

build_app(test_readdir PREINSTALL
    SOURCES
     main.c
)

build_app(test_socket PREINSTALL
    SOURCES
     main.c
)

# AKA Snake
build_app(sdl_test PREINSTALL
    SOURCES
     main.c
    LIBRARIES
     sdl3
)

build_app(sdl2_test PREINSTALL
    SOURCES
     main.c
    LIBRARIES
     sdl2
)

build_app(test_thread PREINSTALL
    SOURCES
     main.c
)

build_app(test_wifi PREINSTALL
    SOURCES
     main.c
)

get_property(SDK_APP_ELFS GLOBAL PROPERTY SDK_APP_ELFS)
add_custom_target(sdk_app_build_all ALL DEPENDS ${SDK_APP_ELFS})
