# anvil-model-reload: built through the AnvilPlugin package like any plugin
# repository; within the Anvil tree the package is already loaded.
cmake_minimum_required(VERSION 3.25)
project(anvil_model_reload LANGUAGES CXX)

if(NOT COMMAND anvil_add_plugin)
    find_package(AnvilPlugin REQUIRED)
endif()

anvil_add_plugin(anvil_model_reload SOURCES src/plugin.cpp src/files.cpp src/gfx_file.cpp src/reload.cpp
    src/game.cpp src/report.cpp src/texts.cpp)
target_compile_definitions(anvil_model_reload PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(anvil_model_reload PRIVATE shell32 ole32)
