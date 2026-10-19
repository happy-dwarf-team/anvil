# Consumes the SDK archive the way a plugin repository does (FetchContent of
# its URL) and builds an example with it. Run by tests/sdk/verify-sdk.ps1.
#   cmake -S tests/sdk -B <dir> -DANVIL_SDK_URL=<archive> -DANVIL_SDK_SHA256=<hash> -DANVIL_EXAMPLES=<examples folder>
cmake_minimum_required(VERSION 3.25)
project(sdk_fetch LANGUAGES C CXX)

include(FetchContent)
FetchContent_Declare(anvil_sdk URL "${ANVIL_SDK_URL}" URL_HASH "SHA256=${ANVIL_SDK_SHA256}")
FetchContent_MakeAvailable(anvil_sdk)

add_subdirectory("${ANVIL_EXAMPLES}/hello-cpp" hello-cpp)
