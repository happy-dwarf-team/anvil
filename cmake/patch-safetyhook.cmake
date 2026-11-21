# Anvil's change to SafetyHook v0.7.0, run by FetchContent as its PATCH_COMMAND
# in SafetyHook's source folder. It can run again on a patched tree.
#
# anvil#545 C5: SafetyHook's trap handler continued every access violation on a
# page it had trapped, and it never forgets a trap. A real read or write fault
# there (a write to a hooked code page that is read-only again) was continued
# forever, so the thread spun at the faulting instruction instead of crashing.
# A trap only makes pages non-executable, so a trapped thread faults on
# execution: the handler now continues only execute faults, and any other
# access violation goes on to the next handler (the game's, the crash report).
set(file "${CMAKE_CURRENT_SOURCE_DIR}/src/os.windows.cpp")
file(READ "${file}" content)
if(content MATCHES "anvil#545 C5")
    return()
endif()
# git may check the file out with CRLF line ends; the patched file has LF.
string(REPLACE "\r\n" "\n" content "${content}")

set(original [==[        if (exception_code != EXCEPTION_ACCESS_VIOLATION) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
]==])
set(patched [==[        if (exception_code != EXCEPTION_ACCESS_VIOLATION) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        // Anvil (anvil#545 C5): a trap makes pages non-executable, so only an
        // execute fault is a trapped thread. A read or write fault goes on to
        // the next handler instead of being continued forever.
        if (exp->ExceptionRecord->NumberParameters < 2 ||
            exp->ExceptionRecord->ExceptionInformation[0] != EXCEPTION_EXECUTE_FAULT) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
]==])

string(FIND "${content}" "${original}" at)
if(at EQUAL -1)
    message(FATAL_ERROR "patch-safetyhook.cmake: the trap handler in ${file} is not the one of SafetyHook v0.7.0; "
                        "check the patch against the new version")
endif()
string(REPLACE "${original}" "${patched}" content "${content}")
file(WRITE "${file}" "${content}")
