# Give every staged file root:wheel before the package is built from it.
#
# CPack has no setting for ownership: the pkg(8) archive records whoever owns the files in the
# staging directory, and pkg preserves that on install. Built by hand, that is the person who ran
# cpack - so the broker, its configuration and its rc.d script arrived owned by them. On a machine
# where that account does not exist the files land on whichever account happens to hold the same
# numeric id, which is worse than untidy.
#
# Run as a CPACK_PRE_BUILD_SCRIPT, after the install into the staging directory and before the
# archive is made. It needs cpack to be running as root; when it is not, chown fails and the script
# says so rather than producing a package that looks right and is not.

IF (NOT DEFINED CPACK_TEMPORARY_INSTALL_DIRECTORY OR NOT IS_DIRECTORY "${CPACK_TEMPORARY_INSTALL_DIRECTORY}")
    MESSAGE(FATAL_ERROR "Ownership script: no staging directory to work on")
ENDIF ()

EXECUTE_PROCESS(COMMAND chown -R root:wheel "${CPACK_TEMPORARY_INSTALL_DIRECTORY}"
                RESULT_VARIABLE chownResult
                ERROR_VARIABLE chownError)

IF (NOT chownResult EQUAL 0)
    MESSAGE(FATAL_ERROR
            "Could not set root:wheel on the staged files: ${chownError}"
            "Build the package as root - 'sudo cpack', or build_freebsd_package.sh, which does it.")
ENDIF ()
