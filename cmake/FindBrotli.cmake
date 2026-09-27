IF (WIN32)
   SET (BROTLI_POSSIBLE_INCLUDE_PATHS
        $ENV{SystemDrive}/*/include
        $ENV{ProgramFiles}/*/include
        $ENV{ProgramFiles}/*/inc
        $ENV{ProgramW6432}/*/include
        $ENV{ProgramW6432}/*/inc)
   SET (BROTLI_POSSIBLE_LIB_PATHS
        $ENV{SystemDrive}/*/lib/x64
        $ENV{ProgramFiles}/*/lib/x64
        $ENV{SystemDrive}/*/lib
        $ENV{ProgramFiles}/*/lib
        $ENV{ProgramW6432}/*/lib/x64
        $ENV{SystemDrive}/*/lib
        $ENV{ProgramW6432}/*/lib)
ELSE (WIN32)
   SET (BROTLI_POSSIBLE_INCLUDE_PATHS
        $ENV{HOME}/local/include
        /usr/local/include
        /usr/include)
   SET (BROTLI_POSSIBLE_LIB_PATHS
        $ENV{HOME}/local/lib
        /usr/local/lib
        /usr/lib /usr/lib/*)
ENDIF ()

FIND_PATH(BROTLI_INCLUDE_DIR brotli/decode.h ${BROTLI_POSSIBLE_INCLUDE_PATHS})
FIND_LIBRARY(BROTLI_LIBRARY_DECODE NAMES brotlidec PATHS ${BROTLI_POSSIBLE_LIB_PATHS})
FIND_LIBRARY(BROTLI_LIBRARY_ENCODE NAMES brotlienc PATHS ${BROTLI_POSSIBLE_LIB_PATHS})

IF (BROTLI_INCLUDE_DIR AND BROTLI_LIBRARY_DECODE AND BROTLI_LIBRARY_ENCODE)
   SET(BROTLI_FOUND TRUE)
   SET(BROTLI_LIBRARIES ${BROTLI_LIBRARY_DECODE} ${BROTLI_LIBRARY_ENCODE})
ENDIF ()

IF (NOT BROTLI_FOUND)
   IF (BROTLI_FIND_REQUIRED)
      MESSAGE(FATAL_ERROR "Could not find BROTLI")
   ENDIF (BROTLI_FIND_REQUIRED)
ENDIF ()
