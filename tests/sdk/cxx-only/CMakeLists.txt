# A plugin project with C++ only that takes the SDK from Anvil's sources
# (FetchContent of the repository): Anvil's own project enables C as well, the
# plugin's does not (#81). Configured, not built, by ctest anvil_sdk_cxx_only:
#   cmake -S tests/sdk/cxx-only -B <dir> -DANVIL_ROOT=<anvil source tree>
cmake_minimum_required(VERSION 3.25)
project(cxx_only LANGUAGES CXX)

add_subdirectory(anvil anvil)
anvil_add_plugin(hello_cpp SOURCES "${ANVIL_ROOT}/examples/hello-cpp/hello.cpp"
    PLUGIN_DIR "${ANVIL_ROOT}/examples/hello-cpp")
