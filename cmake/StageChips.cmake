# Copy the CPC system header with two hooks. All chip headers stay upstream.
set(cpc_header "${CHIPS_SRC}/systems/cpc.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${cpc_header}")
file(READ "${cpc_header}" cpc_source)

function(cpc_replace_anchor anchor replacement)
    string(FIND "${cpc_source}" "${anchor}" start)
    string(FIND "${cpc_source}" "${anchor}" end REVERSE)
    if(start EQUAL -1 OR NOT start EQUAL end)
        message(FATAL_ERROR "Unexpected chips CPC source while installing hook for '${anchor}'")
    endif()
    string(REPLACE "${anchor}" "${replacement}" cpc_source "${cpc_source}")
    set(cpc_source "${cpc_source}" PARENT_SCOPE)
endfunction()

cpc_replace_anchor(
    "} else if ((cpu_pins & (Z80_M1|Z80_IORQ)) == Z80_IORQ) {"
    "} else if ((cpu_pins & (Z80_M1|Z80_IORQ)) == Z80_IORQ && !cpc_expansion_io(sys, &cpu_pins)) {")
cpc_replace_anchor(
    "rom1_ptr = sys->rom_basic;"
    "rom1_ptr = cpc_expansion_upper_rom(sys, rom_select, sys->rom_basic);")

set(cpc_generated "${CMAKE_CURRENT_BINARY_DIR}/cpc-generated")
file(MAKE_DIRECTORY "${cpc_generated}")
file(WRITE "${cpc_generated}/cpc.h" "${cpc_source}")
