IF (WIN32)
    SET(LEVELDB_POSSIBLE_INCLUDE_PATHS
            $ENV{ProgramFiles}/leveldb/include
            $ENV{ProgramFiles\(x86\)}/leveldb/include)
    SET(LEVELDB_POSSIBLE_LIB_PATHS
            $ENV{ProgramFiles}/leveldb/lib
            $ENV{ProgramFiles\(x86\)}/leveldb/lib)
ELSE (WIN32)
    SET(LEVELDB_POSSIBLE_INCLUDE_PATHS
            $ENV{HOME}/local/include
            /usr/local/include
            /usr/include)
    SET(LEVELDB_POSSIBLE_LIB_PATHS
            $ENV{HOME}/local/lib
            /usr/local/lib
            /usr/lib /usr/lib/*)
ENDIF (WIN32)

FIND_PATH(LEVELDB_INCLUDE_DIR leveldb/db.h ${LEVELDB_POSSIBLE_INCLUDE_PATHS})
FIND_LIBRARY(LEVELDB_LIBRARY NAMES leveldb PATHS ${LEVELDB_POSSIBLE_LIB_PATHS})

IF (LEVELDB_INCLUDE_DIR AND LEVELDB_LIBRARY)
    SET(LEVELDB_FOUND TRUE)
ENDIF (LEVELDB_INCLUDE_DIR AND LEVELDB_LIBRARY)

IF (LEVELDB_FOUND)
    IF (NOT RocksDB_FIND_QUIETLY)
        MESSAGE(STATUS "Found LEVELDB: ${LEVELDB_LIBRARY}")
    ENDIF ()
ELSE (LEVELDB_FOUND)
    IF (LEVELDB_FIND_REQUIRED)
        MESSAGE(FATAL_ERROR "Could not find LEVELDB")
    ENDIF (LEVELDB_FIND_REQUIRED)
ENDIF (LEVELDB_FOUND)
