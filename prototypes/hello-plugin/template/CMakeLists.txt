cmake_minimum_required(VERSION 3.25)
project(hello LANGUAGES CXX)

# A real template would fetch a released SDK:
#   include(FetchContent)
#   FetchContent_Declare(anvil_sdk URL https://github.com/happy-dwarf-team/anvil/releases/download/v2.0.0/anvil-sdk.zip)
#   FetchContent_MakeAvailable(anvil_sdk)
# The prototype points at the SDK folder next to it.
find_package(AnvilPlugin REQUIRED PATHS "${CMAKE_CURRENT_LIST_DIR}/../sdk/cmake" NO_DEFAULT_PATH)

anvil_add_plugin(hello SOURCES src/plugin.cpp)
