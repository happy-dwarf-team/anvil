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

# Core: everything except DllMain and the proxy exports, so tests can link it.
add_library(anvil_core STATIC
    src/core/expectations.cpp
    src/core/game_build.cpp
    src/core/log.cpp
    src/core/manifest.cpp
    src/core/patching.cpp
    src/core/resolver.cpp
    src/core/runtime.cpp
    src/core/semver.cpp
    src/core/seh_guard.cpp
    src/core/services.cpp)
target_include_directories(anvil_core PUBLIC include src)
target_link_libraries(anvil_core PUBLIC tomlplusplus::tomlplusplus safetyhook::safetyhook bcrypt)
target_compile_definitions(anvil_core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    ANVIL_VERSION="${PROJECT_VERSION}")
if(MSVC)
    target_compile_options(anvil_core PUBLIC /W4 /utf-8 /permissive- /EHsc)
endif()

# version.dll = proxy + loader
# File version shown in the DLL's Properties, from the same version.txt.
configure_file(src/proxy/version.rc.in ${CMAKE_CURRENT_BINARY_DIR}/anvil_version.rc @ONLY)
add_library(anvil SHARED src/anvil_main.cpp src/proxy/version_proxy.cpp src/proxy/version.def
    ${CMAKE_CURRENT_BINARY_DIR}/anvil_version.rc)
target_link_libraries(anvil PRIVATE anvil_core)
set_target_properties(anvil PROPERTIES OUTPUT_NAME version)

if(ANVIL_BUILD_EXAMPLES)
    add_library(hello_plugin SHARED examples/hello/hello.c)
    target_include_directories(hello_plugin PRIVATE include)
    set_target_properties(hello_plugin PROPERTIES OUTPUT_NAME hello)
endif()

if(ANVIL_BUILD_TESTS)
    enable_testing()
    add_executable(anvil_tests tests/test_main.cpp)
    target_link_libraries(anvil_tests PRIVATE anvil_core)
    add_dependencies(anvil_tests anvil)
    add_test(NAME anvil_tests COMMAND anvil_tests $<TARGET_FILE:anvil>)
endif()

# Release layout (cmake --install ... --component anvil): <prefix>/version.dll, <prefix>/anvil/...
install(TARGETS anvil RUNTIME DESTINATION . COMPONENT anvil)
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
