# Stands for Anvil's source tree as a subproject: its project enables C and
# C++, then loads the AnvilPlugin package as the tree's CMakeLists.txt does.
project(anvil_stand_in LANGUAGES C CXX)

set(ANVIL_SDK_INCLUDE_DIR "${ANVIL_ROOT}/include")
set(ANVIL_SDK_TOOL "${CMAKE_COMMAND}")  # not run: the test only configures
include("${ANVIL_ROOT}/sdk/cmake/AnvilPluginConfig.cmake")
