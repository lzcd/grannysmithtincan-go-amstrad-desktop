cmake_minimum_required(VERSION 3.20)
project(fujinet-go-amstrad VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(FRONTEND "gnome" CACHE STRING "Desktop frontend (gnome or none)")
set_property(CACHE FRONTEND PROPERTY STRINGS gnome none)
if(NOT FRONTEND MATCHES "^(gnome|none)$")
    message(FATAL_ERROR "FRONTEND must be gnome or none")
endif()

include(CTest)
include(cmake/Dependencies.cmake)
add_subdirectory(core)
if(FRONTEND STREQUAL "gnome")
    add_subdirectory(frontends/gnome)
endif()
