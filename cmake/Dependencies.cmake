include(FetchContent)

set(CHIPS_REVISION ca7d7ddd3ba77b48685d24120cf413ea53786767)
set(CHIPS_SRC "" CACHE PATH "Existing checkout of floooh/chips")

if(NOT CHIPS_SRC AND EXISTS "${PROJECT_SOURCE_DIR}/third_party/chips/systems/cpc.h")
    set(CHIPS_SRC "${PROJECT_SOURCE_DIR}/third_party/chips")
endif()

if(NOT CHIPS_SRC)
    FetchContent_Declare(chips
        GIT_REPOSITORY https://github.com/floooh/chips.git
        GIT_TAG "${CHIPS_REVISION}"
    )
    FetchContent_MakeAvailable(chips)
    set(CHIPS_SRC "${chips_SOURCE_DIR}")
endif()

if(NOT EXISTS "${CHIPS_SRC}/systems/cpc.h" OR NOT EXISTS "${CHIPS_SRC}/chips/z80.h"
   OR NOT EXISTS "${CHIPS_SRC}/util/z80dasm.h")
    message(FATAL_ERROR "CHIPS_SRC must point to a floooh/chips source tree")
endif()
