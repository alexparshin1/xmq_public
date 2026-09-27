# The one extension the broker ships with, and the reason it can authenticate at all.
#
# Built through XMQ_ADD_EXTENSION like every other extension, the samples and the closed ones alike.
# That is what keeps it honest: the function clears the include path and the link libraries, so this
# sees extension/xmq_extension.h and nothing else of the broker. Being inside the tree buys it no
# access that an extension in somebody else's repository does not have - and if a later change here
# needs a broker header, that is the signal that something belongs in the ABI, not in an include
# path.
#
# SPTK, because its store is a database and it reaches SQL the way the broker does rather than
# linking libsqlite3 or libpq itself.
XMQ_ADD_EXTENSION(xmq_user_database SOURCES ${CMAKE_CURRENT_LIST_DIR}/UserDatabase.cpp SPTK OPENSSL)

# The SPTK it links is the broker's own copy, in lib/xmq-sptk beside this library's directory - see
# the install rules in the top CMakeLists.txt. Inside a running broker the libraries are already
# loaded by soname and this changes nothing; it is what makes the library resolve on its own, for
# ldd and for anything else that loads it.
IF (UNIX AND NOT APPLE)
    SET(xmqUserDatabaseRpath "$ORIGIN/../xmq-sptk")
    IF (GCC_LIBSTDCXX_DIR)
        SET(xmqUserDatabaseRpath "${xmqUserDatabaseRpath}:${GCC_LIBSTDCXX_DIR}")
    ENDIF ()
    SET_TARGET_PROPERTIES(xmq_user_database PROPERTIES INSTALL_RPATH "${xmqUserDatabaseRpath}")
ENDIF ()

# Not lib/xmq: that is the directory the web interface is served from, so anything left there can be
# fetched over HTTP by anyone who can reach the control service.
# LIBRARY for the .so, RUNTIME for the .dll. In CMake's taxonomy a Windows DLL is a runtime
# artifact, not a library one, so "LIBRARY DESTINATION" alone never applied there: the DLL went to
# the default runtime directory, bin, while the configuration the installer writes names
# lib/xmq-extensions on every platform. The broker then could not load its own authenticator -
# "cannot load ... xmq_user_database.dll: The specified module could not be found (126)" - and,
# the extension being required, served the configuration interface and no MQTT at all.
INSTALL(TARGETS xmq_user_database
        LIBRARY DESTINATION lib/xmq-extensions
        RUNTIME DESTINATION lib/xmq-extensions)

# The template its configuration is created from - by the broker, on a first start, not by the
# installer. The work is in a script of its own because it has to run at install time: see the
# comments there.
SET(XMQ_USER_DATABASE_LIBRARY_NAME
        "${CMAKE_SHARED_LIBRARY_PREFIX}xmq_user_database${CMAKE_SHARED_LIBRARY_SUFFIX}")
SET(XMQ_USER_DATABASE_TEMPLATE "${CMAKE_CURRENT_LIST_DIR}/50-user-database.conf.in")

CONFIGURE_FILE("${CMAKE_CURRENT_LIST_DIR}/install-config.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/install-config.cmake" @ONLY)
INSTALL(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/install-config.cmake")
