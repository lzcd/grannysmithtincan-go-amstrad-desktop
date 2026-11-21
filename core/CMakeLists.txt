include("${PROJECT_SOURCE_DIR}/cmake/StageChips.cmake")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(rom_builder "${PROJECT_SOURCE_DIR}/tools/cpc/build_fuji_rom.py")
set(rom_protocol "${CMAKE_CURRENT_SOURCE_DIR}/include/cpcfuji.h"
                 "${CMAKE_CURRENT_SOURCE_DIR}/cpc/fuji_card.h")
add_custom_command(
    OUTPUT "${cpc_generated}/fuji_rom.h"
    COMMAND Python3::Interpreter "${rom_builder}" "${cpc_generated}/fuji_rom.h" ${rom_protocol}
    DEPENDS "${rom_builder}" ${rom_protocol}
    VERBATIM)

find_package(Threads REQUIRED)
add_library(cpc_core STATIC cpc/session.c cpc/fuji_card.c cpc/fuji_link.c
    "${cpc_generated}/fuji_rom.h")
target_include_directories(cpc_core PUBLIC include)
target_include_directories(cpc_core SYSTEM PRIVATE "${cpc_generated}" "${CHIPS_SRC}")
target_link_libraries(cpc_core PRIVATE Threads::Threads)

add_executable(cpc-headless tests/headless.c)
target_include_directories(cpc-headless PRIVATE cpc)
target_link_libraries(cpc-headless PRIVATE cpc_core)

if(BUILD_TESTING)
    add_executable(cpc-fuji-card-test tests/fuji_card_test.c)
    target_include_directories(cpc-fuji-card-test PRIVATE cpc)
    target_link_libraries(cpc-fuji-card-test PRIVATE cpc_core)
    add_test(NAME cpc-fuji-card COMMAND cpc-fuji-card-test)
endif()

set(CPC464_ROM "" CACHE FILEPATH "Local CPC 464 combined or OS ROM for the boot smoke test")
set(CPC464_BASIC_ROM "" CACHE FILEPATH "BASIC ROM if CPC464_ROM is a separate OS ROM")
set(CPC_FUJINET_RUNTIME_DIST "" CACHE PATH "FujiNet-PC RS232 build/dist for the live TNFS test")
set(CPC_TNFSD "" CACHE FILEPATH "FujiNet tnfsd binary for the live TNFS test")
if(BUILD_TESTING AND CPC464_ROM)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/boot-smoke")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fuji-smoke")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/boip-smoke")
    set(boot_roms "${CPC464_ROM}")
    if(CPC464_BASIC_ROM)
        list(APPEND boot_roms "${CPC464_BASIC_ROM}")
    endif()
    add_test(NAME cpc464-boot COMMAND cpc-headless
        "${CMAKE_CURRENT_BINARY_DIR}/boot-smoke" ${boot_roms})
    add_test(NAME cpc464-fuji COMMAND cpc-headless --fuji
        "${CMAKE_CURRENT_BINARY_DIR}/fuji-smoke" ${boot_roms})
    add_test(NAME cpc464-boip
        COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/boip_fixture.py"
        "$<TARGET_FILE:cpc-headless>" "${CMAKE_CURRENT_BINARY_DIR}/boip-smoke" ${boot_roms})
    add_test(NAME cpc464-boip-program
        COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/boip_fixture.py" --program
        "$<TARGET_FILE:cpc-headless>" "${CMAKE_CURRENT_BINARY_DIR}/boip-smoke" ${boot_roms})
    set_tests_properties(cpc464-boip cpc464-boip-program PROPERTIES TIMEOUT 40)
    if(CPC_FUJINET_RUNTIME_DIST AND CPC_TNFSD)
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tnfs-smoke")
        set(tnfs_smoke_args
            "$<TARGET_FILE:cpc-headless>" "${PROJECT_SOURCE_DIR}/tools/fujinet/run-rs232.py"
            "${CPC_FUJINET_RUNTIME_DIST}" "${CPC_TNFSD}"
            "${CMAKE_CURRENT_BINARY_DIR}/tnfs-smoke" ${boot_roms})
        add_test(NAME cpc464-tnfs
            COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/tnfs_smoke.py"
            ${tnfs_smoke_args})
        add_test(NAME cpc464-tnfs-program
            COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/tnfs_smoke.py"
            --program ${tnfs_smoke_args})
        set_tests_properties(cpc464-tnfs cpc464-tnfs-program PROPERTIES
            TIMEOUT 60 RESOURCE_LOCK cpc_tnfs_port)
    endif()
endif()
