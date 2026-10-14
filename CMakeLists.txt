cmake_minimum_required(VERSION 3.19)

project(glowbuffer
    DESCRIPTION "Drag and drop and paste that work on Hyprland, sandboxed apps included"
    VERSION "0.1.0"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)        # gnu++23, matching Hyprland's GNU extensions
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# Hyprland is built with GCC; a plugin built with clang mismatches its ABI.
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR "glowbuffer must be built with g++ to match Hyprland's ABI. Configure with -DCMAKE_CXX_COMPILER=g++")
endif()

include(GNUInstallDirs)

find_package(PkgConfig REQUIRED)
pkg_check_modules(deps REQUIRED IMPORTED_TARGET
    hyprland
    libsystemd          # sd-bus, for the document portal
    wayland-server
    libinput
    xkbcommon
    pixman-1
    libdrm
)

add_library(glowbuffer MODULE
    src/main.cpp
    src/Bridge.cpp
    src/ClipboardPlan.cpp
    src/PasteImage.cpp
    src/Persist.cpp
    src/Sandbox.cpp
    src/UriList.cpp
)
set_target_properties(glowbuffer PROPERTIES PREFIX "")   # glowbuffer.so, not libglowbuffer.so

target_compile_options(glowbuffer PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
    -Wno-missing-field-initializers
)

target_compile_definitions(glowbuffer PRIVATE
    WLR_USE_UNSTABLE
    GLOWBUFFER_VERSION="${PROJECT_VERSION}"
)

# -z nodelete: the image is never unmapped, not even by `hyprctl plugin unload`.
# glowbuffer installs request handlers on Hyprland's wl_data_offer objects, and a clipboard
# offer lives until the next copy: unmap the code and the next paste jumps into nothing,
# taking the compositor down. The cost is that a same-path reload after an unload gets the
# old image back (glibc matches it by name); load a new build from a new path or restart
# Hyprland. glowdesk links the opposite way (-fno-gnu-unique) for hot reload; that trade
# does not hold for a plugin whose handlers outlive its unload.
target_link_options(glowbuffer PRIVATE -Wl,-z,nodelete)

target_link_libraries(glowbuffer PRIVATE PkgConfig::deps)

install(TARGETS glowbuffer LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/hyprland/plugins)

# Pure-logic tests, no compositor needed. tests/run.sh does the same without CMake.
option(GLOWBUFFER_TESTS "Build the unit tests" OFF)
if(GLOWBUFFER_TESTS)
    enable_testing()
    add_executable(unit_test tests/unit.cpp src/UriList.cpp src/Sandbox.cpp src/PasteImage.cpp src/ClipboardPlan.cpp)
    target_include_directories(unit_test PRIVATE src)
    target_compile_options(unit_test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME unit COMMAND unit_test)
endif()
