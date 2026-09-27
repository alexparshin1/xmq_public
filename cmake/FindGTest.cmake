IF (WIN32)
   SET (GTest_POSSIBLE_INCLUDE_PATHS
        $ENV{ProgramFiles}/*/include/gtest
        $ENV{ProgramFiles\(x86\)}/*/include/gtest)
   SET (GTest_POSSIBLE_LIB_PATHS
        $ENV{ProgramFiles}/*/lib/x64
        $ENV{ProgramFiles}/*/lib
        $ENV{ProgramW6432}/*/lib/x64
        $ENV{ProgramW6432}/*/lib)
ELSE (WIN32)
   # Same order, and same assumption, as FindSPTK.cmake: the prefix this build installs into, then
   # a home installation - where SPTK puts the googletest it vendors - then the system's.
   SET (GTest_POSSIBLE_INCLUDE_PATHS
        ${CMAKE_INSTALL_PREFIX}/include
        ${CMAKE_INSTALL_PREFIX}/include/gtest
        $ENV{HOME}/.local/include
        $ENV{HOME}/.local/include/gtest
        /usr/local/include
        /usr/local/include/gtest
        /usr/include
        /usr/include/gtest)
   SET (GTest_POSSIBLE_LIB_PATHS
        ${CMAKE_INSTALL_PREFIX}/lib
        $ENV{HOME}/.local/lib
        /usr/local/lib
        /usr/lib /usr/lib/*)
ENDIF (WIN32)

# HINTS, not PATHS. PATHS is consulted after CMAKE_SYSTEM_PREFIX_PATH, so a gtest sitting in a
# system prefix wins over the one in the prefix this build installs into - even when the two are
# different builds of different versions. That is not hypothetical: on FreeBSD the pkg gtest under
# /usr/local is built against the base Clang's libc++, so linking it into a GCC/libstdc++ build of
# XMQ leaves every [abi:cxx11] symbol undefined, while the headers still came from the other copy.
FIND_PATH(GTest_INCLUDE_DIR gtest.h HINTS ${GTest_POSSIBLE_INCLUDE_PATHS})
FIND_LIBRARY(GTest_LIBRARY NAMES gtest gtestd HINTS ${GTest_POSSIBLE_LIB_PATHS})
FIND_LIBRARY(GTest_MAIN_LIBRARY NAMES gtest_main gtest_maind HINTS ${GTest_POSSIBLE_LIB_PATHS})

IF (GTest_INCLUDE_DIR AND GTest_LIBRARY)
   SET(GTest_FOUND TRUE)
ENDIF (GTest_INCLUDE_DIR AND GTest_LIBRARY)

IF (GTest_FOUND)
   IF (NOT GTest_FIND_QUIETLY)
      MESSAGE(STATUS "Found GTEST: ${GTest_LIBRARY}")
   ENDIF (NOT GTest_FIND_QUIETLY)
ELSE (GTest_FOUND)
   IF (GTest_FIND_REQUIRED)
      MESSAGE(FATAL_ERROR "Could not find GTest")
   ENDIF (GTest_FIND_REQUIRED)
ENDIF (GTest_FOUND)
