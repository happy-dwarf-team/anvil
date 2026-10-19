# Root of the Anvil SDK archive. FetchContent_MakeAvailable(anvil_sdk) runs this
# file, which defines anvil_add_plugin(); find_package(AnvilPlugin) reads
# cmake/AnvilPluginConfig.cmake directly instead.
cmake_minimum_required(VERSION 3.25)
include("${CMAKE_CURRENT_LIST_DIR}/cmake/AnvilPluginConfig.cmake")
