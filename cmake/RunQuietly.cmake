# Runs a command and says nothing unless it fails.
#
# The React build produces some seventy lines - npm's deprecation warnings for packages nobody here
# chose, and a file-size table - none of which anyone reads while it is working, and all of which
# buries the messages a packaging run does want to show. When it breaks, all of it is wanted at
# once, so it is captured rather than discarded.
#
# Invoked as a script, so that it works the same from any generator:
#
#   ${CMAKE_COMMAND} -DQUIET_DIR=<dir> -DQUIET_WHAT=<description> -DQUIET_COMMAND=<command line>
#                    -P cmake/RunQuietly.cmake
#
# QUIET_COMMAND is one command line for the platform's shell, which is how it was run before this
# script existed: the shell is what finds node on PATH, and a login shell is what reads the profile
# that puts it there.

IF (NOT DEFINED QUIET_COMMAND OR NOT DEFINED QUIET_DIR)
    MESSAGE(FATAL_ERROR "RunQuietly.cmake needs QUIET_COMMAND and QUIET_DIR")
ENDIF ()

# A semicolon in QUIET_COMMAND makes CMake deliver it as a list, and the shell would then run only
# the first element while the rest became its positional arguments - quietly, and with a zero exit
# status. Caught here rather than left to be discovered by a step that appears to succeed.
LIST(LENGTH QUIET_COMMAND partCount)
IF (partCount GREATER 1)
    MESSAGE(FATAL_ERROR
            "QUIET_COMMAND arrived as ${partCount} parts, which means it contains an unescaped "
            "semicolon: [${QUIET_COMMAND}]. Escape it as \\; or use a script.")
ENDIF ()

IF (WIN32)
    SET(shell cmd /C "${QUIET_COMMAND}")
ELSE ()
    # Login shell, so that ~/.profile has put node on PATH.
    SET(shell bash -l -c "${QUIET_COMMAND}")
ENDIF ()

EXECUTE_PROCESS(
        COMMAND ${shell}
        WORKING_DIRECTORY "${QUIET_DIR}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output          # one stream: interleaving is how the failure reads
)

IF (NOT result EQUAL 0)
    # Named before the output, because the output is long and the reader needs to know what it
    # belongs to before wading into it.
    MESSAGE(STATUS "Failed: ${QUIET_WHAT}")
    MESSAGE("${output}")
    MESSAGE(FATAL_ERROR "${QUIET_WHAT} failed with status ${result}")
ENDIF ()
