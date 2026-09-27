# Determines whether or not the compiler supports C++20
macro(check_for_cxx20_compiler _VAR)
    #message(STATUS "Checking for C++20 compiler")
    set(${_VAR})
    if ((MSVC AND (MSVC12 OR MSVC13 OR MSVC14 OR MSVC15 OR MSVC16)) OR
    (CMAKE_COMPILER_IS_GNUCXX AND NOT ${CMAKE_CXX_COMPILER_VERSION} VERSION_LESS 5.0) OR
    (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND NOT ${CMAKE_CXX_COMPILER_VERSION} VERSION_LESS 5.0))
        set(${_VAR} 1)
        #message(STATUS "Checking for C++20 compiler - available")
    else ()
        #message(STATUS "Checking for C++20 compiler - unavailable")
    endif ()
endmacro()

# Sets the appropriate flag to enable C++20 support
macro(enable_cxx20)
    if (CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++20")
    endif ()
endmacro()
