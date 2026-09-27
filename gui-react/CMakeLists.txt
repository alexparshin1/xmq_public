# The React GUI is built in the binary directory, from a copy of the sources.
# The copy is made by the custom command below, so that editing a source file
# is enough to rebuild the GUI: copying at configure time only would leave the
# build using whatever the sources looked like when CMake last ran.
# CONFIGURE_DEPENDS so that adding a source file is noticed by a plain build: without it
# the glob is evaluated once, at configure time, and a new screen is invisible until CMake
# is run again.
FILE(GLOB_RECURSE REACT_SOURCE_FILES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/src/*
     ${CMAKE_CURRENT_SOURCE_DIR}/public/*)
LIST(APPEND REACT_SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/package.json
                               ${CMAKE_CURRENT_SOURCE_DIR}/package-lock.json
                               ${CMAKE_CURRENT_SOURCE_DIR}/.env)

# What the dependency install needs, and nothing else: these two files decide what npm installs,
# and they are the only inputs the install step has.
SET(REACT_STAGE_MANIFEST
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/package.json ${CMAKE_CURRENT_BINARY_DIR}/package.json
    # The lock file goes with it, so npm installs the versions this project was tested against.
    # Without it the build directory had package.json alone, every install resolved afresh, and the
    # interface was built from whatever npm chose that day - which is how a build that worked
    # yesterday fails today with an error in a dependency nobody here has ever edited. It did:
    # a fresh resolve pulled an eslint-config-react-app that wanted a jest plugin it did not get.
    #
    # Copying it also fixed this rule running on every build. react-scripts writes the bundle's
    # content hash into index.html, so a dependency set that was resolved afresh each time produced
    # a different index.html each time, and a rule whose output keeps changing is a rule that keeps
    # running. Pinned input, constant output: a build with nothing changed is now instant.
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/package-lock.json ${CMAKE_CURRENT_BINARY_DIR}/package-lock.json)

# What the interface itself is built from.
SET(REACT_STAGE_SOURCES
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR}/src
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/public ${CMAKE_CURRENT_BINARY_DIR}/public
    # react-scripts reads .env from the directory it runs in, which is this one. See the file
    # itself for what is set there and why it is a file rather than an environment variable.
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/.env ${CMAKE_CURRENT_BINARY_DIR}/.env)

# One rule for both platforms: RunQuietly.cmake picks the shell, so there is nothing left here
# that differs between them.
#
# Quietly, because npm has some seventy lines to say - deprecation warnings for packages nobody
# here chose, and a file-size table - and none of it is read while it is working. On a packaging
# run it buries the messages that are actually being looked for. The output is captured rather
# than discarded: when the step fails, all of it is printed.
SET(REACT_RUN ${CMAKE_COMMAND} -DQUIET_DIR=${CMAKE_CURRENT_BINARY_DIR})
SET(REACT_RUNNER -P ${CMAKE_SOURCE_DIR}/cmake/RunQuietly.cmake)
SET(REACT_NODE_MODULES_STAMP ${CMAKE_CURRENT_BINARY_DIR}/node_modules/.package-lock.json)
SET(REACT_BUILD_INDEX ${CMAKE_CURRENT_BINARY_DIR}/build/index.html)

# The staged copies below are cleared of the previous build's hashed bundles first, for the reason
# the install rule at the end of this file explains. It hangs on this rule, the one that rebuilds
# the interface, rather than on the POST_BUILD that stages it: the staging runs on every build and
# skips files that already match, and removing static/ from under it would put the full 6MB rewrite
# back. This rule runs only when npm has actually produced a new interface.
#
# remove_directory rather than "rm -rf", which needs CMake 3.17 and this project asks only for 3.15.
IF (WIN32)
    FOREACH (reactConsumer xmq_server xmq_unit_tests)
        LIST(APPEND REACT_UNSTAGE
             COMMAND ${CMAKE_COMMAND} -E remove_directory
                     $<TARGET_FILE_DIR:${reactConsumer}>/../react/static)
    ENDFOREACH ()
ENDIF ()

# Installing the dependencies is its own rule, depending on the two files that decide what gets
# installed and on nothing else.
#
# It used to be the first half of the rule below, which depends on every source file - so npm ci ran
# whenever any screen was edited. npm ci deletes node_modules and reinstalls it from scratch by
# design: 374MB of downloading and unpacking to pay for a one-line change to a screen, and it needs
# the network to do it. A registry that stops answering mid-request hangs the build with no timeout,
# which is how this was noticed - and the build said "Building the React interface" while it hung,
# because that was the COMMENT of the whole two-step rule.
#
# The stamp is what npm ci itself writes at the end of a successful install, so this cannot think it
# has run when it has not.
ADD_CUSTOM_COMMAND(
        OUTPUT ${REACT_NODE_MODULES_STAMP}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/package.json
                ${CMAKE_CURRENT_SOURCE_DIR}/package-lock.json
        ${REACT_STAGE_MANIFEST}
        # The flags are about the network, which is the one thing this step cannot do without and
        # the one thing a build should not be at the mercy of:
        #
        #   --prefer-offline  take what the npm cache already has and ask the registry only for what
        #                     is missing. A rebuild on a machine that has built before then needs no
        #                     network at all - which matters on a phone connection away from wifi.
        #   --no-audit        the audit request happens after everything is installed, is nothing to
        #                     do with building, and is the likeliest thing to have been hanging when
        #                     this was found: node_modules was complete and npm was still waiting on
        #                     one socket that had gone quiet.
        #   --no-fund         the same, for the funding notice.
        #   --fetch-timeout   so a registry that stops answering fails the build in a minute instead
        #                     of hanging it until somebody notices.
        COMMAND ${REACT_RUN} "-DQUIET_WHAT=Installing the React environment"
                "-DQUIET_COMMAND=npm ci --prefer-offline --no-audit --no-fund --fetch-timeout=60000"
                ${REACT_RUNNER}
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Installing the React environment")

ADD_CUSTOM_COMMAND(
        OUTPUT ${REACT_BUILD_INDEX}
        DEPENDS ${REACT_SOURCE_FILES} ${REACT_NODE_MODULES_STAMP}
        ${REACT_STAGE_SOURCES}
        COMMAND ${REACT_RUN} "-DQUIET_WHAT=Building the React interface" "-DQUIET_COMMAND=npm run build"
                ${REACT_RUNNER}
        # After the build, not before: the staged copy is only worth discarding once there is a new
        # interface to replace it with.
        ${REACT_UNSTAGE}
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Building the React interface"
)

ADD_CUSTOM_TARGET(ReactGUI ALL DEPENDS ${REACT_BUILD_INDEX})

# On Windows the GUI is resolved relative to the running executable - <directory>/react beside
# <directory>/bin/xmq_server.exe (see DirectoryNames::webfaceDirectory). That layout exists only
# in an installation; a binary started straight out of the build tree, the unit tests above all,
# would look for a react directory that the build never produces. So the built GUI is staged next
# to every executable that serves it, at the same ../react the installed layout gives them.
#
# POST_BUILD on the target rather than extra commands on the custom command above, so that a
# staged copy someone deleted comes back on the next build instead of waiting for npm to have
# reason to rebuild the GUI.
IF (WIN32)
    # copy_directory rewrites all 6MB every build; the newer form skips files that already match.
    IF (CMAKE_VERSION VERSION_GREATER_EQUAL 3.26)
        SET(REACT_STAGE_COMMAND copy_directory_if_different)
    ELSE ()
        SET(REACT_STAGE_COMMAND copy_directory)
    ENDIF ()

    FOREACH (reactConsumer xmq_server xmq_unit_tests)
        ADD_CUSTOM_COMMAND(TARGET ReactGUI POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E ${REACT_STAGE_COMMAND}
                        ${CMAKE_CURRENT_BINARY_DIR}/build
                        $<TARGET_FILE_DIR:${reactConsumer}>/../react)
    ENDFOREACH ()
ENDIF ()

# Clear the previously installed interface out before laying the new one down. INSTALL(DIRECTORY)
# copies over whatever is already there and never prunes, and react-scripts names its bundles by
# content hash, so without this every change to the interface left the old main.<hash>.js and its
# 4MB source map behind for good, with nothing that would ever reclaim them. Worse than the disk:
# a browser still holding the previous index.html keeps finding the bundle it names and goes on
# quietly running the old interface, so a failed upgrade looks like a working one.
#
# Only static/ is removed, not the whole destination. On Unix the destination is lib/xmq, which the
# example extensions install their libraries into as well (examples/extensions/*/CMakeLists.txt),
# and those must survive. Everything react-scripts gives a hashed name to lives under static/; the
# files beside it - index.html, asset-manifest.json, favicon.ico, manifest.json, robots.txt - keep
# their names from one build to the next and are simply overwritten.
#
# Evaluated at install time, not here: CMAKE_INSTALL_PREFIX can still be overridden by
# "cmake --install --prefix", and DESTDIR only exists then.
INSTALL(CODE "
    SET(reactDestination \"${XMQ_REACT_INSTALL_DESTINATION}\")
    IF (NOT IS_ABSOLUTE \"\${reactDestination}\")
        SET(reactDestination \"\${CMAKE_INSTALL_PREFIX}/\${reactDestination}\")
    ENDIF ()
    SET(reactStaticDirectory \"\$ENV{DESTDIR}\${reactDestination}/static\")
    IF (IS_DIRECTORY \"\${reactStaticDirectory}\")
        MESSAGE(STATUS \"Removing the previously installed interface: \${reactStaticDirectory}\")
        FILE(REMOVE_RECURSE \"\${reactStaticDirectory}\")
    ENDIF ()
")

# The destination relative to the prefix, not REACT_DIRECTORY itself - see XMQ_INSTALL_DESTINATION
# in the top-level CMakeLists for why an absolute one cannot be packaged.
INSTALL(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/build/ DESTINATION ${XMQ_REACT_INSTALL_DESTINATION})
