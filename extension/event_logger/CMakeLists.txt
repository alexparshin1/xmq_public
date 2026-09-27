# event_logger - an XMQ extension.
#
# A plain shared library. It includes the broker's ABI headers and links nothing of the broker, so
# nothing here needs the broker's build, its compiler or its standard library.

CMAKE_MINIMUM_REQUIRED(VERSION 3.20)
PROJECT(xmq_event_logger CXX)

SET(CMAKE_CXX_STANDARD 20)
SET(CMAKE_CXX_STANDARD_REQUIRED ON)

# Where the broker is installed, taken from where its headers were found. Without it CMake installs
# into /usr/local, which is not where this broker lives - so the library lands somewhere the broker
# will never be told to load it from. Generated projects carry this now; this one predates it.
IF (CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    SET(CMAKE_INSTALL_PREFIX "$ENV{HOME}/.local" CACHE PATH "Where the broker is installed" FORCE)
ENDIF ()

ADD_LIBRARY(xmq_event_logger SHARED EventLogger.cpp)

# The directory holding extension/xmq_extension.h. Filled in when this project was generated; pass
# -DXMQ_EXTENSION_INCLUDE_DIR=... to build against a different broker's headers.
IF (NOT XMQ_EXTENSION_INCLUDE_DIR)
    SET(XMQ_EXTENSION_INCLUDE_DIR "/home/alexeyp/workspace/xmq")
ENDIF ()
IF (NOT EXISTS "${XMQ_EXTENSION_INCLUDE_DIR}/extension/xmq_extension.h")
    MESSAGE(FATAL_ERROR "No extension/xmq_extension.h under ${XMQ_EXTENSION_INCLUDE_DIR}")
ENDIF ()
TARGET_INCLUDE_DIRECTORIES(xmq_event_logger PRIVATE "${XMQ_EXTENSION_INCLUDE_DIR}")

# Hidden visibility: the one exported symbol is the entry point, and XMQ_EXTENSION_EXPORT in the
# header takes care of that one. Without this the library exports its whole implementation.
SET_TARGET_PROPERTIES(xmq_event_logger PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)

# Not lib/xmq: that is the directory the web interface is served from, so anything left there can be
# fetched over HTTP by anyone who can reach the control service.
INSTALL(TARGETS xmq_event_logger LIBRARY DESTINATION lib/xmq-extensions)
