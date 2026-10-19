# PROTOTYPE of the CMake package an Anvil SDK release would ship (throwaway).
#
#   find_package(AnvilPlugin)            # or FetchContent of the SDK archive
#   anvil_add_plugin(<target> SOURCES ...)
#
# reads plugin.toml and symbols.toml next to the calling CMakeLists.txt, runs
# `anvil gen`, builds the DLL with the static CRT, and lays out
#   <build>/plugin/<id>/{plugin.toml, symbols.toml, <entry>.dll}
# Targets added: <target> (the DLL), <target>-check (offline `anvil check`) and,
# when EU4_DIR is set, <target>-deploy (copy into <EU4_DIR>/anvil/plugins/<id>).

get_filename_component(ANVIL_SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)

function(anvil_add_plugin target)
    cmake_parse_arguments(ARG "" "" "SOURCES" ${ARGN})
    set(src_dir "${CMAKE_CURRENT_SOURCE_DIR}")
    file(READ "${src_dir}/plugin.toml" manifest)
    string(REGEX MATCH "\nid = \"([a-z0-9-]+)\"" _ "\n${manifest}")
    set(id "${CMAKE_MATCH_1}")
    string(REGEX MATCH "\nentry = \"([^\"]+)\\.dll\"" _ "\n${manifest}")
    set(entry "${CMAKE_MATCH_1}")

    set(gen_dir "${CMAKE_CURRENT_BINARY_DIR}/anvil-gen")
    add_custom_command(
        OUTPUT "${gen_dir}/symbols.gen.h" "${gen_dir}/symbols.gen.hpp"
        COMMAND Python3::Interpreter "${ANVIL_SDK_DIR}/tools/anvil.py" gen "${src_dir}/symbols.toml" "${gen_dir}"
        DEPENDS "${src_dir}/symbols.toml" "${ANVIL_SDK_DIR}/tools/anvil.py"
        COMMENT "anvil gen: symbols.toml -> symbols.gen.h / symbols.gen.hpp")

    set(out_dir "${CMAKE_BINARY_DIR}/plugin/${id}")
    add_library(${target} SHARED ${ARG_SOURCES} "${gen_dir}/symbols.gen.h" "${gen_dir}/symbols.gen.hpp")
    target_include_directories(${target} PRIVATE "${ANVIL_SDK_DIR}/include" "${gen_dir}")
    target_compile_features(${target} PRIVATE cxx_std_20)
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME "${entry}"
        MSVC_RUNTIME_LIBRARY "MultiThreaded"
        RUNTIME_OUTPUT_DIRECTORY "${out_dir}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${out_dir}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${out_dir}")
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${src_dir}/plugin.toml" "${src_dir}/symbols.toml" "${out_dir}")

    add_custom_target(${target}-check
        COMMAND Python3::Interpreter "${ANVIL_SDK_DIR}/tools/anvil.py" check "${src_dir}" $<$<BOOL:${EU4_EXE}>:--exe> $<$<BOOL:${EU4_EXE}>:${EU4_EXE}>
        COMMENT "anvil check ${id}" VERBATIM)
    if(DEFINED EU4_DIR)
        add_custom_target(${target}-deploy
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${out_dir}" "${EU4_DIR}/anvil/plugins/${id}"
            DEPENDS ${target}
            COMMENT "deploy ${id} to ${EU4_DIR}/anvil/plugins/${id}")
    endif()
endfunction()
