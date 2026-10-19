cmake_minimum_required(VERSION 3.25)
project(hello_c LANGUAGES C CXX)
find_package(AnvilPlugin REQUIRED PATHS "${CMAKE_CURRENT_LIST_DIR}/../sdk/cmake" NO_DEFAULT_PATH)
anvil_add_plugin(hello_c SOURCES src/plugin.c)
