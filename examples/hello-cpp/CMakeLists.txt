cmake_minimum_required(VERSION 3.25)
project(hello_cpp LANGUAGES CXX)

# A plugin repository gets the package from the SDK archive of a release:
#   include(FetchContent)
#   FetchContent_Declare(anvil_sdk URL <anvil-sdk-windows-x64.zip> URL_HASH SHA256=<hash>)
#   FetchContent_MakeAvailable(anvil_sdk)
# or from an unpacked SDK: cmake -DAnvilPlugin_DIR=<sdk>/cmake ...
# Within the Anvil tree, the package is already loaded.
if(NOT COMMAND anvil_add_plugin)
    find_package(AnvilPlugin REQUIRED)
endif()

anvil_add_plugin(hello_cpp SOURCES hello.cpp)
