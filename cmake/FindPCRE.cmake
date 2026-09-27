IF (WIN32)
   SET (PCRE_POSSIBLE_INCLUDE_PATHS
        $ENV{SystemDrive}/*/include
        $ENV{ProgramFiles}/*/include
        $ENV{ProgramFiles}/*/inc
        $ENV{ProgramW6432}/*/include
        $ENV{ProgramW6432}/*/inc)
   SET (PCRE_POSSIBLE_LIB_PATHS
        $ENV{SystemDrive}/*/lib/x64
        $ENV{ProgramFiles}/*/lib/x64
        $ENV{SystemDrive}/*/lib
        $ENV{ProgramFiles}/*/lib
        $ENV{ProgramW6432}/*/lib/x64
        $ENV{SystemDrive}/*/lib
        $ENV{ProgramW6432}/*/lib)
ELSE (WIN32)
   SET (PCRE_POSSIBLE_INCLUDE_PATHS
        $ENV{HOME}/local/include
        /usr/local/include
        /usr/include)
   SET (PCRE_POSSIBLE_LIB_PATHS
        $ENV{HOME}/local/lib
        /usr/local/lib
        /usr/lib /usr/lib/*)
ENDIF (WIN32)

FIND_PATH(PCRE_INCLUDE_DIR pcre.h ${PCRE_POSSIBLE_INCLUDE_PATHS})
FIND_LIBRARY(PCRE_LIBRARY NAMES pcre pcre3 PATHS ${PCRE_POSSIBLE_LIB_PATHS})

IF (PCRE_INCLUDE_DIR AND PCRE_LIBRARY)
   SET(PCRE_FOUND TRUE)
ENDIF (PCRE_INCLUDE_DIR AND PCRE_LIBRARY)

IF (PCRE_FOUND)
   IF (NOT PCRE_FIND_QUIETLY)
      MESSAGE(STATUS "Found PCRE: ${PCRE_LIBRARY}")
   ENDIF (NOT PCRE_FIND_QUIETLY)
ELSE (PCRE_FOUND)
   IF (PCRE_FIND_REQUIRED)
      MESSAGE(FATAL_ERROR "Could not find PCRE")
   ENDIF (PCRE_FIND_REQUIRED)
ENDIF (PCRE_FOUND)
