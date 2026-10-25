# PROTOTYPE, throwaway (anvil#134). Built on its own against an Anvil SDK:
#   cmake -S prototypes/bench-quit -B <build> -G Ninja -DAnvilPlugin_DIR=<sdk>/cmake
cmake_minimum_required(VERSION 3.25)
project(bench_quit LANGUAGES CXX)

if(NOT COMMAND anvil_add_plugin)
    find_package(AnvilPlugin REQUIRED)
endif()

anvil_add_plugin(bench_quit SOURCES bench_quit.cpp)
