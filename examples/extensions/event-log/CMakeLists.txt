# An extension is a plain shared library that includes two headers from the broker and links
# nothing of it. That is the point of the C ABI: nothing here needs the broker's build, its
# compiler, or its standard library.

CMAKE_MINIMUM_REQUIRED(VERSION 3.20)
PROJECT(xmq_event_log CXX)

SET(CMAKE_CXX_STANDARD 20)
SET(CMAKE_CXX_STANDARD_REQUIRED ON)

ADD_LIBRARY(xmq_event_log SHARED EventLog.cpp)

# XMQ_EXTENSION_INCLUDE_DIR points at the directory holding extension/xmq_extension.h. Building
# inside the broker's tree it is the tree itself; building on its own, it is wherever the broker's
# development package put the headers.
IF (NOT XMQ_EXTENSION_INCLUDE_DIR)
    SET(XMQ_EXTENSION_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
ENDIF ()
TARGET_INCLUDE_DIRECTORIES(xmq_event_log PRIVATE "${XMQ_EXTENSION_INCLUDE_DIR}")

# No "lib" prefix games and no versioned soname: the configuration names the file outright.
SET_TARGET_PROPERTIES(xmq_event_log PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)

# Not lib/xmq: that is the directory the web interface is served from, so anything left there can be
# fetched over HTTP by anyone who can reach the control service.
# LIBRARY for the .so, RUNTIME for the .dll. In CMake's taxonomy a Windows DLL is a runtime
# artifact, not a library one, so "LIBRARY DESTINATION" alone never applied there: the DLL went to
# the default runtime directory, bin, while the configuration the installer writes names
# lib/xmq-extensions on every platform. The broker then could not load its own authenticator -
# "cannot load ... xmq_user_database.dll: The specified module could not be found (126)" - and,
# the extension being required, served the configuration interface and no MQTT at all.
INSTALL(TARGETS xmq_event_log
        LIBRARY DESTINATION lib/xmq-extensions
        RUNTIME DESTINATION lib/xmq-extensions)
