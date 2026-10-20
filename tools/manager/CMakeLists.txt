# AnvilManager.exe, the player's installer and management tool (design §15,
# issue #59). anvil_manager_core holds what it does without a window: finding
# the game, the install, repair and uninstall plans for the package it
# carries, plugin zips (checked with anvil check's own code, anvil_tool) and
# the release check. The tests link it directly.

# miniz, pinned to a release tag and compiled here from its sources (its own
# CMakeLists.txt is not used): reads and, for the tests, writes zip archives.
include(FetchContent)
FetchContent_Declare(miniz
    GIT_REPOSITORY https://github.com/richgel999/miniz.git
    GIT_TAG 3.0.2
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR no-cmake)
FetchContent_MakeAvailable(miniz)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/miniz/miniz_export.h "#pragma once\n#define MINIZ_EXPORT\n")
add_library(anvil_miniz STATIC
    ${miniz_SOURCE_DIR}/miniz.c
    ${miniz_SOURCE_DIR}/miniz_tdef.c
    ${miniz_SOURCE_DIR}/miniz_tinfl.c
    ${miniz_SOURCE_DIR}/miniz_zip.c)
target_include_directories(anvil_miniz PUBLIC ${miniz_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/miniz)
# Archives are read from memory: no stdio, no time functions.
target_compile_definitions(anvil_miniz PUBLIC MINIZ_NO_STDIO MINIZ_NO_TIME MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
set_target_properties(anvil_miniz PROPERTIES COMPILE_WARNING_AS_ERROR OFF)
set(ANVIL_MINIZ_LICENSE ${miniz_SOURCE_DIR}/LICENSE CACHE INTERNAL "")

add_library(anvil_manager_core STATIC
    src/files.cpp
    src/game.cpp
    src/install.cpp
    src/plugin_zip.cpp
    src/release_check.cpp
    src/zip.cpp)
target_include_directories(anvil_manager_core PUBLIC src)
target_link_libraries(anvil_manager_core PUBLIC anvil_tool PRIVATE anvil_miniz advapi32)
if(MSVC)
    target_compile_options(anvil_manager_core PRIVATE /W4 /WX)
endif()
