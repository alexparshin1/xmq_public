IF (WIN32)

    SET(EPOLL_POSSIBLE_INCLUDE_PATHS
            $ENV{SystemDrive}/wepoll/include
            $ENV{SystemDrive}/*/wepoll/include
            $ENV{ProgramFiles}/wepoll/include
            $ENV{ProgramW6432}/wepoll/include)

    SET(EPOLL_POSSIBLE_LIB_PATHS
            $ENV{SystemDrive}/wepoll/lib
            $ENV{SystemDrive}/*/wepoll/lib
            $ENV{ProgramFiles}/wepoll/lib
            $ENV{ProgramW6432}/wepoll/lib)

    SET(EPOLL_NAME wepoll)

    FIND_PATH(EPOLL_INCLUDE_DIR wepoll.h ${EPOLL_POSSIBLE_INCLUDE_PATHS})

    FIND_LIBRARY(EPOLL_LIBRARY NAMES wepoll.lib PATHS ${EPOLL_POSSIBLE_LIB_PATHS})

ELSE ()

    SET(EPOLL_POSSIBLE_INCLUDE_PATHS /usr/include)
    SET(EPOLL_POSSIBLE_LIB_PATHS /usr/lib /usr/local/lib)
    FIND_PATH(EPOLL_INCLUDE_DIR sys/epoll.h ${EPOLL_POSSIBLE_INCLUDE_PATHS})

    IF (BSD)
        FIND_LIBRARY(EPOLL_LIBRARY NAMES epoll PATHS ${EPOLL_POSSIBLE_LIB_PATHS})
    ELSE ()
        SET(EPOLL_LIBRARY "")
    ENDIF ()

    SET(EPOLL_NAME epoll)

ENDIF ()

IF (EPOLL_INCLUDE_DIR)
    SET(EPOLL_FOUND TRUE)
ENDIF ()

IF (EPOLL_FOUND)
    IF (NOT EPoll_FIND_QUIETLY)
        MESSAGE(STATUS "Found EPoll:  ${EPOLL_LIBRARY}")
    ENDIF ()
ELSE ()
    IF (EPoll_FIND_REQUIRED)
        MESSAGE(FATAL_ERROR "Could not find ${EPOLL_NAME} library")
    ENDIF ()
ENDIF ()
