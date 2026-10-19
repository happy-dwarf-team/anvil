cmake_minimum_required(VERSION 3.25)
# version.txt is the only place the version is written; release-please bumps it.
file(STRINGS version.txt ANVIL_VERSION_TEXT LIMIT_COUNT 1 REGEX "^[0-9]+\\.[0-9]+\\.[0-9]+$")
if(NOT ANVIL_VERSION_TEXT)
    message(FATAL_ERROR "version.txt must hold a MAJOR.MINOR.PATCH version")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS version.txt)
project(anvil VERSION ${ANVIL_VERSION_TEXT} LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Anvil targets 64-bit Windows only")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: version.dll must not depend on a redistributable being installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(ANVIL_BUILD_TESTS "Build unit tests" ON)
option(ANVIL_BUILD_EXAMPLES "Build the example plugin" ON)

include(FetchContent)
FetchContent_Declare(tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG v3.4.0
    GIT_SHALLOW TRUE)
set(SAFETYHOOK_FETCH_ZYDIS ON CACHE BOOL "" FORCE)
FetchContent_Declare(safetyhook
    GIT_REPOSITORY https://github.com/cursey/safetyhook.git
    GIT_TAG v0.7.0
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(tomlplusplus safetyhook)

# anvil_core: the rules shared by version.dll and anvil.exe (manifests, SemVer,
# the resolver, the game build identity, symbols.toml and its verification
# against a PE image, instruction boundaries (Zydis, from SafetyHook's fetch),
# the log). Nothing here may depend on running inside the game process, so the
# CLI applies the same rules as the loader.
add_library(anvil_core STATIC
    src/core/game_build.cpp
    src/core/instructions.cpp
    src/core/log.cpp
    src/core/manifest.cpp
    src/core/pe_image.cpp
    src/core/resolver.cpp
    src/core/semver.cpp
    src/core/symbols.cpp)
target_include_directories(anvil_core PUBLIC include src)
target_link_libraries(anvil_core PUBLIC tomlplusplus::tomlplusplus Zydis bcrypt)
target_compile_definitions(anvil_core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    ANVIL_VERSION="${PROJECT_VERSION}")
if(MSVC)
    target_compile_options(anvil_core PUBLIC /W4 /utf-8 /permissive- /EHsc)
endif()

# anvil_loader: the parts that act on the running game (startup, the patch
# registry, symbols.toml checks against the running image, services, SEH guards).
# Linked into version.dll and the tests, never into anvil.exe.
add_library(anvil_loader STATIC
    src/core/patching.cpp
    src/core/runtime.cpp
    src/core/seh_guard.cpp
    src/core/services.cpp
    src/core/symbol_check.cpp)
target_link_libraries(anvil_loader PUBLIC anvil_core safetyhook::safetyhook)

# version.dll = proxy + loader
# File version shown in the DLL's Properties, from the same version.txt.
configure_file(src/proxy/version.rc.in ${CMAKE_CURRENT_BINARY_DIR}/anvil_version.rc @ONLY)
add_library(version_dll SHARED src/anvil_main.cpp src/proxy/version_proxy.cpp src/proxy/version.def
    ${CMAKE_CURRENT_BINARY_DIR}/anvil_version.rc)
target_link_libraries(version_dll PRIVATE anvil_loader)
set_target_properties(version_dll PROPERTIES OUTPUT_NAME version)

# anvil.exe = the command-line tool for plugin authors (design §16). Its
# commands live in anvil_tool so the tests can run them in-process.
add_library(anvil_tool STATIC src/cli/check.cpp src/cli/gen.cpp)
target_link_libraries(anvil_tool PUBLIC anvil_core)
add_executable(anvil src/cli/anvil_cli.cpp)
target_link_libraries(anvil PRIVATE anvil_tool)

if(ANVIL_BUILD_EXAMPLES)
    add_library(hello_plugin SHARED examples/hello/hello.c)
    target_include_directories(hello_plugin PRIVATE include)
    set_target_properties(hello_plugin PROPERTIES OUTPUT_NAME hello)
endif()

if(ANVIL_BUILD_TESTS)
    enable_testing()
    add_executable(anvil_tests tests/test_main.cpp tests/manifest_tests.cpp tests/resolver_tests.cpp)
    target_link_libraries(anvil_tests PRIVATE anvil_loader)
    target_compile_definitions(anvil_tests PRIVATE ANVIL_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_dependencies(anvil_tests version_dll)
    add_test(NAME anvil_tests COMMAND anvil_tests $<TARGET_FILE:version_dll>)
    add_executable(anvil_symbols_tests tests/test_symbols.cpp)
    target_link_libraries(anvil_symbols_tests PRIVATE anvil_loader)
    add_test(NAME anvil_symbols_tests COMMAND anvil_symbols_tests)

    add_executable(anvil_cli_tests tests/test_cli.cpp)
    target_link_libraries(anvil_cli_tests PRIVATE anvil_tool)
    add_dependencies(anvil_cli_tests anvil)
    add_test(NAME anvil_cli_tests COMMAND anvil_cli_tests $<TARGET_FILE:anvil>)

    # anvil gen on the design §5.2 example: the headers must compile, warning
    # free, as C11 and C++23 and declare what the fixture says.
    set(ANVIL_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen_compile)
    add_custom_command(
        OUTPUT ${ANVIL_GEN_DIR}/symbols.gen.h ${ANVIL_GEN_DIR}/symbols.gen.hpp
        COMMAND anvil gen ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/gen/symbols.toml ${ANVIL_GEN_DIR}
        DEPENDS anvil ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/gen/symbols.toml
        COMMENT "anvil gen tests/fixtures/gen/symbols.toml")
    add_executable(anvil_gen_compile tests/gen_compile.c tests/gen_compile.cpp
        ${ANVIL_GEN_DIR}/symbols.gen.h ${ANVIL_GEN_DIR}/symbols.gen.hpp)
    target_include_directories(anvil_gen_compile PRIVATE include ${ANVIL_GEN_DIR})
    set_target_properties(anvil_gen_compile PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
    if(MSVC)
        target_compile_options(anvil_gen_compile PRIVATE /W4 /WX /utf-8)
    endif()
    add_test(NAME anvil_gen_compile COMMAND anvil_gen_compile)

    add_test(NAME anvil_cli_version COMMAND anvil --version)
    set_tests_properties(anvil_cli_version PROPERTIES PASS_REGULAR_EXPRESSION "^anvil ${PROJECT_VERSION}[\r\n]*$")
    add_test(NAME anvil_cli_help COMMAND anvil --help)
    set_tests_properties(anvil_cli_help PROPERTIES PASS_REGULAR_EXPRESSION "Usage: anvil <command>")
    # gen and check without their arguments, like unknown commands, exit non-zero (2).
    add_test(NAME anvil_cli_gen COMMAND anvil gen)
    add_test(NAME anvil_cli_check COMMAND anvil check)
    add_test(NAME anvil_cli_unknown COMMAND anvil frobnicate)
    set_tests_properties(anvil_cli_gen anvil_cli_check anvil_cli_unknown PROPERTIES WILL_FAIL TRUE)
endif()

# Release layout (cmake --install ... --component anvil): <prefix>/version.dll, <prefix>/anvil/...
# anvil.exe is not part of the package yet; it ships with the SDK archive (design §16).
install(TARGETS version_dll RUNTIME DESTINATION . COMPONENT anvil)
install(FILES dist/anvil.toml DESTINATION anvil COMPONENT anvil)
install(FILES dist/plugins-README.txt DESTINATION anvil/plugins RENAME README.txt COMPONENT anvil)
install(FILES LICENSE README.md DESTINATION anvil/docs COMPONENT anvil)
# Third-party code compiled into version.dll: ship its license texts.
# Zydis is declared inside SafetyHook's scope; fetch its properties here.
FetchContent_GetProperties(zydis)
if(NOT zydis_SOURCE_DIR)
    message(FATAL_ERROR "Zydis sources not found (needed for its license text)")
endif()
install(FILES ${tomlplusplus_SOURCE_DIR}/LICENSE DESTINATION anvil/docs/third-party RENAME tomlplusplus-LICENSE.txt COMPONENT anvil)
install(FILES ${safetyhook_SOURCE_DIR}/LICENSE DESTINATION anvil/docs/third-party RENAME safetyhook-LICENSE.txt COMPONENT anvil)
install(FILES ${zydis_SOURCE_DIR}/LICENSE DESTINATION anvil/docs/third-party RENAME zydis-LICENSE.txt COMPONENT anvil)
install(FILES ${zydis_SOURCE_DIR}/dependencies/zycore/LICENSE DESTINATION anvil/docs/third-party RENAME zycore-LICENSE.txt COMPONENT anvil)
