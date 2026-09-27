IF (WIN32)
   SET (SPTK_POSSIBLE_INCLUDE_PATHS
        $ENV{SystemDrive}/SPTK/include
        $ENV{ProgramFiles}/SPTK/include
        $ENV{ProgramW6432}/SPTK/include
        "$ENV{SystemDrive}/Program Files (x86)/SPTK/include"
        "$ENV{SystemDrive}/Program Files/*/include")
   SET (SPTK_POSSIBLE_LIB_PATHS
        $ENV{SystemDrive}/SPTK/lib
        $ENV{ProgramFiles}/SPTK/lib
        $ENV{ProgramW6432}/SPTK/lib
        "$ENV{SystemDrive}/Program Files (x86)/SPTK/lib"
        "$ENV{SystemDrive}/Program Files/*/LIB")
   SET (SPTK_POSSIBLE_SHARE_PATHS
        $ENV{SystemDrive}/SPTK/share
        $ENV{ProgramFiles}/SPTK/share
        $ENV{ProgramW6432}/SPTK/share
        "$ENV{SystemDrive}/Program Files (x86)/SPTK/share"
        "$ENV{SystemDrive}/Program Files/*/share")
ELSE (WIN32)
   # The prefix this build installs into, then a home installation, then the system's. ~/.local is
   # worth searching even when this build installs elsewhere, because that is exactly what a package
   # build is: configured for /usr/local, against an SPTK installed under the builder's home.
   #
   # The order assumes one toolchain per machine, which is the policy. On a machine carrying two,
   # this happily finds an SPTK built against the other standard library and the link fails with
   # nothing but undefined std:: symbols to explain it - pass -DSPTK_LIBRARY_CORE and friends to
   # settle it by hand there.
   SET (SPTK_POSSIBLE_INCLUDE_PATHS
        ${CMAKE_INSTALL_PREFIX}/include
        $ENV{HOME}/.local/include
        /usr/local/include
        /usr/include)
   SET (SPTK_POSSIBLE_LIB_PATHS
        ${CMAKE_INSTALL_PREFIX}/lib
        $ENV{HOME}/.local/lib
        /usr/local/lib
        /usr/lib
        /usr/lib64)
ENDIF (WIN32)

# HINTS, not PATHS, throughout: PATHS is only consulted after CMAKE_SYSTEM_PREFIX_PATH, so a copy
# in a system prefix would win over the prefix this build installs into. See the note in
# FindGTest.cmake for what that costs when the two copies are not ABI-compatible.
FIND_PATH(SPTK_INCLUDE_DIR sptk5/sptk.h HINTS ${SPTK_POSSIBLE_INCLUDE_PATHS})
FIND_PATH(GTEST_INCLUDE_DIR gtest/gtest.h HINTS ${SPTK_POSSIBLE_INCLUDE_PATHS})

# The shared libraries, not the _static ones. SPTK reaches its database drivers with dlopen, and
# each driver links libspdb5 and libsputil5 for itself: linking SPTK statically here would put a
# second copy of it in the process the moment the first driver loads, under the same soname and
# with nothing to notice that the two copies came from different builds. They do come from
# different builds - that was live on the build host on 2026-08-22, the installed shared libraries
# being nine days older than the static ones and missing a security fix the static ones had.
#
# The libraries are shipped inside XMQ's own package (see the install rules in CMakeLists.txt), so
# this does not ask a user to install SPTK separately.
#
# A find_library result is a cache entry, and find_library leaves an entry that is already set
# exactly as it is - the NAMES below are not even looked at. A tree configured back when this file
# searched for sputil5_static therefore keeps handing out the static archives forever, however this
# file is edited. Drop such an entry so the search below can run again.
FOREACH (sptkLibraryVariable SPTK_LIBRARY_CORE SPTK_LIBRARY_WSDL SPTK_LIBRARY_SPDB)
   IF (DEFINED ${sptkLibraryVariable} AND ${sptkLibraryVariable} MATCHES "_static|\.a$")
      MESSAGE(STATUS "Discarding static SPTK library ${${sptkLibraryVariable}} cached in ${sptkLibraryVariable}")
      UNSET(${sptkLibraryVariable} CACHE)
   ENDIF ()
ENDFOREACH ()

# On Unix, restrict the search to the shared-library suffix: SPTK names its static archives
# libsputil5_static.a today, but nothing guarantees that, and a plain libsputil5.a sitting beside
# the .so would otherwise be picked up silently. On Windows the suffix cannot separate the two - a
# DLL is linked through an import library carrying the same .lib suffix as a static archive - so
# there the _static naming and the guard above are what keep them apart.
IF (NOT WIN32)
   SET (sptkSavedLibrarySuffixes ${CMAKE_FIND_LIBRARY_SUFFIXES})
   SET (CMAKE_FIND_LIBRARY_SUFFIXES ${CMAKE_SHARED_LIBRARY_SUFFIX})
ENDIF (NOT WIN32)

FIND_LIBRARY(SPTK_LIBRARY_CORE NAMES sputil5 HINTS ${SPTK_POSSIBLE_LIB_PATHS})
FIND_LIBRARY(SPTK_LIBRARY_WSDL NAMES spwsdl5 HINTS ${SPTK_POSSIBLE_LIB_PATHS})
FIND_LIBRARY(SPTK_LIBRARY_SPDB NAMES spdb5   HINTS ${SPTK_POSSIBLE_LIB_PATHS})

IF (NOT WIN32)
   SET (CMAKE_FIND_LIBRARY_SUFFIXES ${sptkSavedLibrarySuffixes})
   UNSET (sptkSavedLibrarySuffixes)
ENDIF (NOT WIN32)

# GTest is linked statically and is deliberately outside the restriction above.
FIND_LIBRARY(GTEST_LIBRARY NAMES gtest HINTS ${SPTK_POSSIBLE_LIB_PATHS})

SET(SPTK_LIBRARIES ${SPTK_LIBRARY_WSDL} ${SPTK_LIBRARY_SPDB} ${SPTK_LIBRARY_CORE})

IF (WIN32)
    FIND_PATH(SPTK_SHARE_DIR sptk5/resources/event_provider.rc ${SPTK_POSSIBLE_SHARE_PATHS})
ENDIF (WIN32)

# Every one of the three, not just a non-empty SPTK_LIBRARIES: the list is built unconditionally
# above, so a missing library leaves the literal SPTK_LIBRARY_WSDL-NOTFOUND in it and the list is
# still non-empty. That reported success and handed the -NOTFOUND string to the linker.
IF (SPTK_INCLUDE_DIR AND SPTK_LIBRARY_CORE AND SPTK_LIBRARY_SPDB AND SPTK_LIBRARY_WSDL)
   SET(SPTK_FOUND TRUE)
ENDIF (SPTK_INCLUDE_DIR AND SPTK_LIBRARY_CORE AND SPTK_LIBRARY_SPDB AND SPTK_LIBRARY_WSDL)

# The version of the SPTK found, read from the header SPTK generates on every platform. It matters
# because SPTK's soname carries its whole version: a build against any other release than the one
# asked for produces binaries that name that other release's libraries and load nothing else.
# FIND_PACKAGE(SPTK 5.6.11 EXACT) refuses anything else here, at configure time, instead of at the
# first start of an installed broker. SPTK_NOT_FOUND_REASON says why, for a caller that searched
# QUIET and reports the failure itself.
UNSET(SPTK_VERSION)
UNSET(SPTK_NOT_FOUND_REASON)
IF (SPTK_INCLUDE_DIR AND EXISTS "${SPTK_INCLUDE_DIR}/sptk5/sptk-config.h")
   FILE(STRINGS "${SPTK_INCLUDE_DIR}/sptk5/sptk-config.h" sptkVersionLine
        REGEX "^constexpr const char\\* VERSION = \"[0-9.]+\";")
   IF (sptkVersionLine)
      STRING(REGEX REPLACE "^.*\"([0-9.]+)\".*$" "\\1" SPTK_VERSION "${sptkVersionLine}")
   ENDIF ()
   UNSET(sptkVersionLine)
ENDIF ()
IF (SPTK_FOUND AND SPTK_FIND_VERSION)
   IF (NOT SPTK_VERSION)
      SET(SPTK_FOUND FALSE)
      SET(SPTK_NOT_FOUND_REASON "no version in ${SPTK_INCLUDE_DIR}/sptk5/sptk-config.h, and ${SPTK_FIND_VERSION} is required")
   ELSEIF (SPTK_FIND_VERSION_EXACT AND NOT SPTK_VERSION VERSION_EQUAL SPTK_FIND_VERSION)
      SET(SPTK_FOUND FALSE)
      SET(SPTK_NOT_FOUND_REASON "found ${SPTK_VERSION} in ${SPTK_INCLUDE_DIR}, and exactly ${SPTK_FIND_VERSION} is required")
   ELSEIF (SPTK_VERSION VERSION_LESS SPTK_FIND_VERSION)
      SET(SPTK_FOUND FALSE)
      SET(SPTK_NOT_FOUND_REASON "found ${SPTK_VERSION} in ${SPTK_INCLUDE_DIR}, and at least ${SPTK_FIND_VERSION} is required")
   ENDIF ()
ENDIF ()

IF (SPTK_INCLUDE_DIR AND SPTK_LIBRARY_SPDB)
   SET(SPTK_DB_FOUND TRUE)
ENDIF (SPTK_INCLUDE_DIR AND SPTK_LIBRARY_SPDB)

IF (SPTK_INCLUDE_DIR AND SPTK_LIBRARY_WSDL)
   SET(SPTK_WSDL_FOUND TRUE)
ENDIF (SPTK_INCLUDE_DIR AND SPTK_LIBRARY_WSDL)

IF (SPTK_FOUND)
   IF (NOT SPTK_FIND_QUIETLY)
      MESSAGE(STATUS "Found SPTK: ${SPTK_LIBRARIES}")
   ENDIF (NOT SPTK_FIND_QUIETLY)
ELSE (SPTK_FOUND)
   IF (SPTK_FIND_REQUIRED)
      MESSAGE(FATAL_ERROR "Could not find SPTK")
   ENDIF (SPTK_FIND_REQUIRED)
ENDIF (SPTK_FOUND)

IF (SPTK_DB_FOUND)
   IF (NOT SPTK_FIND_QUIETLY)
      MESSAGE(STATUS "Found SPTK DB support: ${SPTK_LIBRARY_SPDB}")
   ENDIF (NOT SPTK_FIND_QUIETLY)
ELSE (SPTK_DB_FOUND)
   IF (SPTK_FIND_REQUIRED)
      MESSAGE(FATAL_ERROR "Could not find SPTK DB support")
   ENDIF (SPTK_FIND_REQUIRED)
ENDIF (SPTK_DB_FOUND)

IF (SPTK_WSDL_FOUND)
   IF (NOT SPTK_FIND_QUIETLY)
      MESSAGE(STATUS "Found SPTK WSDL support: ${SPTK_LIBRARY_WSDL}")
   ENDIF (NOT SPTK_FIND_QUIETLY)
ELSE (SPTK_WSDL_FOUND)
   IF (SPTK_FIND_REQUIRED)
      MESSAGE(FATAL_ERROR "Could not find SPTK WSDL support")
   ENDIF (SPTK_FIND_REQUIRED)
ENDIF (SPTK_WSDL_FOUND)
