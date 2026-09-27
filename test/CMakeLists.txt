# XMQ unit tests

# CONFIGURE_DEPENDS so that a new test file is noticed by a plain build. Without it the glob is
# evaluated once, at configure time, and a test added afterwards is silently left out of the
# binary: the build succeeds, the suite passes, and the new test never ran.
FILE(GLOB XMQ_TEST_FILES CONFIGURE_DEPENDS
        *.cpp *.h
        BridgeTests/*
        ClientTest/*
        ControlServiceTests/*
        ClusterTests/*
        DataReaderTests/*
        ExtensionTests/*
        PersistenceTests/*
        ScenarioTests/*
        ServerPerformanceTests/*
        ServerTests/Common/*
        ServerTests/Control/*
        ServerTests/ExternalClient/*
        ServerTests/MqttClient/*
        ServerTests/Settings/*
        ServerTests/Subscriptions/*
        SettingsTests/*
        SysTopicsTests/*
        ServerTests/*
        StorageTests/*
        Message/*
        ServiceTests/*
        Queues/*
        UtilityTests/*
)

# SlowAuthorizer.cpp is an extension, built into a shared library of its own below - the glob above
# sweeps the whole of ExtensionTests/ and picks it up as though it were a test. Compiled into this
# binary as well it brings XMQ_DEFINE_EXTENSION with it, which is how its object came to collide
# with the rest of the suite over inline std::string members. Every other extension source is under
# examples/ and so out of the glob's reach; this one lives here because it must not be copied.
LIST(REMOVE_ITEM XMQ_TEST_FILES "${CMAKE_CURRENT_LIST_DIR}/ExtensionTests/SlowAuthorizer.cpp")

INCLUDE_DIRECTORIES(.)

# Tests belonging to extensions kept outside this tree, when one is being built with it. They are
# compiled into this binary rather than one of their own: what they test is an extension talking to
# the broker, so they need the broker, and a second test binary that also links it would double the
# build for nothing. Absent - which is the usual case, and every build of this repository on its
# own - the glob is empty and nothing changes.
SET(XMQ_EXTENSION_TEST_FILES "")
IF (XMQ_EXTENSIONS_SOURCE_DIR AND IS_DIRECTORY "${XMQ_EXTENSIONS_SOURCE_DIR}")
    FILE(GLOB XMQ_EXTENSION_TEST_FILES CONFIGURE_DEPENDS "${XMQ_EXTENSIONS_SOURCE_DIR}/*/test/*.cpp")
ENDIF ()

ADD_EXECUTABLE(xmq_unit_tests ${XMQ_TEST_FILES} ${XMQ_EXTENSION_TEST_FILES} ServerTests/Publish_LargeMessages.cpp)

TARGET_LINK_LIBRARIES(xmq_unit_tests xmq_base xmq_storage xmq_common xmq_service xmq_server_core xmq_client xmq_utils xmq_scenario ${GTest_LIBRARY} ${GTest_MAIN_LIBRARY})

# The sample extension is built as part of the test suite, from the same sources and against the
# same published headers an outside author would use. That is what makes the conformance tests
# worth having: a change to the ABI that would break an already-built extension breaks the build
# here, in this repository, rather than in somebody's deployment.
# Both samples are built, and each declares one capability. That is what lets the conformance
# tests assert the separation rather than describe it: the observer-only library must never be
# asked to authenticate, and the authenticator-only one must never be handed an event.
# Through XMQ_ADD_EXTENSION, the same function a closed extension in its own repository goes
# through - so what the conformance tests load is built exactly the way a customer's extension is,
# and a mistake in that machinery fails here rather than there.
XMQ_ADD_EXTENSION(xmq_test_extension SOURCES ${CMAKE_CURRENT_LIST_DIR}/../examples/extensions/event-log/EventLog.cpp)
XMQ_ADD_EXTENSION(xmq_test_authenticator SOURCES ${CMAKE_CURRENT_LIST_DIR}/../examples/extensions/allow-list/AllowList.cpp)
XMQ_ADD_EXTENSION(xmq_test_authorizer SOURCES ${CMAKE_CURRENT_LIST_DIR}/../examples/extensions/topic-guard/TopicGuard.cpp)

# The shipped authenticator, built a second time as a test fixture so the tests drive the same
# source the broker ships. SPTK gives it the database layer and OPENSSL the KDF it verifies with -
# both opt-in, see cmake/XmqExtension.cmake for why.
XMQ_ADD_EXTENSION(xmq_test_user_database
        SOURCES ${CMAKE_CURRENT_LIST_DIR}/../extension/user-database/UserDatabase.cpp SPTK OPENSSL)

# Not a sample, and deliberately not under examples/: it blocks inside authorize(), which the ABI
# forbids, so that an invalidation can be made to land while the broker is inside the extension.
XMQ_ADD_EXTENSION(xmq_test_slow_authorizer SOURCES ${CMAKE_CURRENT_LIST_DIR}/ExtensionTests/SlowAuthorizer.cpp)

ADD_DEPENDENCIES(xmq_unit_tests xmq_test_extension xmq_test_authenticator xmq_test_authorizer
        xmq_test_slow_authorizer xmq_test_user_database)
TARGET_COMPILE_DEFINITIONS(xmq_unit_tests PRIVATE
        XMQ_TEST_EXTENSION_LIBRARY="$<TARGET_FILE:xmq_test_extension>"
        XMQ_TEST_AUTHENTICATOR_LIBRARY="$<TARGET_FILE:xmq_test_authenticator>"
        XMQ_TEST_AUTHORIZER_LIBRARY="$<TARGET_FILE:xmq_test_authorizer>"
        XMQ_TEST_SLOW_AUTHORIZER_LIBRARY="$<TARGET_FILE:xmq_test_slow_authorizer>"
        XMQ_TEST_USER_DATABASE_LIBRARY="$<TARGET_FILE:xmq_test_user_database>")

# The external-client tests drive xmq_pub and xmq_sub as separate processes. They are told where
# this build put them rather than looking them up on PATH: on PATH they resolve to whatever copy is
# installed on the machine - a different version, or on a machine where XMQ has never been
# installed, nothing at all, which is how they came to fail with status 127 on FreeBSD. Testing the
# binaries this build produced is also the only thing that makes the result mean anything.
ADD_DEPENDENCIES(xmq_unit_tests xmq_pub xmq_sub)
TARGET_COMPILE_DEFINITIONS(xmq_unit_tests PRIVATE
        XMQ_PUB_EXECUTABLE="$<TARGET_FILE:xmq_pub>"
        XMQ_SUB_EXECUTABLE="$<TARGET_FILE:xmq_sub>")

# The web interface the tests ask for pages: this build's React output, not an installed copy. See
# DirectoryNames::setWebfaceDirectory, which the suite calls with this at start-up.
TARGET_COMPILE_DEFINITIONS(xmq_unit_tests PRIVATE
        XMQ_REACT_BUILD_DIRECTORY="${CMAKE_BINARY_DIR}/gui-react/build")

IF (XMQ_EXTENSION_TEST_FILES)
    # An extension's tests load the extension, so it has to exist by the time they run, and they
    # have to be told where it landed.
    ADD_DEPENDENCIES(xmq_unit_tests xmq_acl)
    TARGET_COMPILE_DEFINITIONS(xmq_unit_tests PRIVATE XMQ_ACL_LIBRARY="$<TARGET_FILE:xmq_acl>")
ENDIF ()

# The scenario tests read their .json scenarios from the source tree, told to them here rather than
# resolved against the current directory. They used to be copied into the build tree and looked up
# as a relative path, which meant the suite only passed when started from whichever directory the
# copy had landed in - one directory on Windows, another everywhere else. Nothing writes to these
# files, so there is no reason to copy them at all; not copying also stops a renamed scenario from
# lingering as a stale duplicate, which copy_directory never removed.
TARGET_COMPILE_DEFINITIONS(xmq_unit_tests PRIVATE
        XMQ_TEST_SCENARIO_DIRECTORY="${CMAKE_CURRENT_LIST_DIR}/ScenarioTests")
