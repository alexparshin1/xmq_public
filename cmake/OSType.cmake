IF (UNIX)
    EXECUTE_PROCESS(COMMAND uname OUTPUT_VARIABLE SYSTEM_NAME)
    # "BSD" is matched as well as "Linux" so that OS_TYPE is not left empty on FreeBSD: the test
    # below expanded it unquoted, and an empty expansion turns the IF into a syntax error rather
    # than a false condition. It is quoted now too, so an unrecognised kernel is merely not Linux.
    STRING(REGEX MATCH "Linux|BSD" OS_TYPE "${SYSTEM_NAME}")
    IF ("${OS_TYPE}" STREQUAL "Linux")
        SET(LINUX TRUE)
    ELSE ()
        SET(BSD TRUE)
    ENDIF ()

    EXECUTE_PROCESS(COMMAND grep -E "^ID=" /etc/os-release OUTPUT_VARIABLE OS_NAME)
    STRING(REGEX MATCH "(debian|ubuntu|fedora|redhat|ol|linuxmint)" OS_FLAVOUR "${OS_NAME}")

    EXECUTE_PROCESS(COMMAND grep -E "^VERSION=" /etc/os-release OUTPUT_VARIABLE OS_VERS)
    STRING(REGEX MATCH "[0-9]+" OS_VERSION "${OS_VERS}")
ELSE ()
    SET(OS_TYPE "Windows")
ENDIF ()

IF (OS_FLAVOUR)
    MESSAGE("OS Flavour:         ${OS_TYPE} (${OS_FLAVOUR} ${OS_VERSION})")
ELSE ()
    MESSAGE("OS Type:            ${OS_TYPE}")
ENDIF ()

IF (OS_FLAVOUR STREQUAL "fedora")
    SET(SET_RPATH "1")
ENDIF ()

IF (OS_FLAVOUR STREQUAL "redhat")
    SET(SET_RPATH "1")
ENDIF ()
