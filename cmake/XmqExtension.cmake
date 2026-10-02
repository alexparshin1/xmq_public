# Creates an XMQ extension: a shared library that links nothing of the broker.
#
# Every extension goes through this, the open samples and the closed ones alike, so that "an
# extension sees only the ABI" is arranged by the build rather than remembered by its author. The
# invariant is that an extension carries no libxmq_* and no broker header - not that its ldd output
# names only libc, which stopped being true the moment an extension was allowed to reach a database.
#
# Two things it does that are easy to get wrong by hand:
#
#   - The include path is the directory holding extension/, and nothing else. Without that an
#     extension can include the broker's headers, and the C boundary becomes decorative.
#   - Link libraries are cleared. A target inherits whatever LINK_LIBRARIES() the enclosing
#     directory set, which is how the samples silently picked up a static gtest and failed to link
#     on the one distribution whose gtest is built without -fPIC.
#
# Usage, from an extension's own CMakeLists.txt:
#
#     XMQ_ADD_EXTENSION(xmq_acl SOURCES Acl.cpp Rules.cpp)
#     XMQ_ADD_EXTENSION(xmq_user_database SOURCES UserDatabase.cpp SPTK OPENSSL)
#
# SPTK is opt-in and gives the extension SPTK's headers and its database libraries - for an
# extension whose store is a database. It reaches SQL the way the broker does rather than linking
# libsqlite3 or libpq itself, which would put a branch per database inside every such extension,
# and rather than a query service in the ABI, which would put the broker on that path.
#
FUNCTION(XMQ_ADD_EXTENSION extensionName)
    CMAKE_PARSE_ARGUMENTS(EXTENSION "SPTK;OPENSSL" "" "SOURCES" ${ARGN})

    IF (NOT EXTENSION_SOURCES)
        MESSAGE(FATAL_ERROR "XMQ_ADD_EXTENSION(${extensionName}) needs SOURCES")
    ENDIF ()

    # Where extension/xmq_extension.h is found. Inside the broker's tree that is the tree itself;
    # built on its own against an installed broker it is include/xmq, which the caller sets.
    IF (NOT XMQ_EXTENSION_INCLUDE_DIR)
        SET(XMQ_EXTENSION_INCLUDE_DIR "${XMQ_SOURCE_ROOT}")
    ENDIF ()
    IF (NOT EXISTS "${XMQ_EXTENSION_INCLUDE_DIR}/extension/xmq_extension.h")
        MESSAGE(FATAL_ERROR
                "XMQ_EXTENSION_INCLUDE_DIR does not hold extension/xmq_extension.h: "
                "${XMQ_EXTENSION_INCLUDE_DIR}")
    ENDIF ()

    SET(extensionIncludes "${XMQ_EXTENSION_INCLUDE_DIR}")
    SET(extensionLibraries "")

    IF (EXTENSION_SPTK)
        IF (NOT SPTK_INCLUDE_DIR OR NOT SPTK_LIBRARY_CORE OR NOT SPTK_LIBRARY_SPDB)
            MESSAGE(FATAL_ERROR
                    "XMQ_ADD_EXTENSION(${extensionName} ... SPTK) needs SPTK with database support. "
                    "FIND_PACKAGE(SPTK) has to have run in a directory that reaches this one.")
        ENDIF ()

        # Refused rather than linked: a static SPTK inside an extension is a second copy of the
        # library in a process that already has one, with its own statics and its own idea of what
        # is fixed in it. FindSPTK restricts its own search to the shared suffix, so this catches a
        # cache variable somebody set by hand.
        FOREACH (sptkLibrary "${SPTK_LIBRARY_CORE}" "${SPTK_LIBRARY_SPDB}")
            IF (sptkLibrary MATCHES "_static|\\.a$")
                MESSAGE(FATAL_ERROR
                        "XMQ_ADD_EXTENSION(${extensionName} ... SPTK) will not link a static SPTK: "
                        "${sptkLibrary}")
            ENDIF ()
        ENDFOREACH ()

        # The whole include directory, which on an installed prefix also holds xmq/extension - the
        # ABI headers, which are the only headers XMQ installs. There are no broker headers to
        # expose here, so this does not widen what the extension can see beyond the database.
        LIST(APPEND extensionIncludes "${SPTK_INCLUDE_DIR}")

        # And what SPTK's own public headers include: pcre2.h from RegularExpression.h, zlib.h and
        # openssl/ssl.h from elsewhere. The broker reaches these through INCLUDE_DIRECTORIES() at
        # directory level - which is exactly what this function clears - so an extension given only
        # SPTK's directory fails the moment an SPTK header includes one of them.
        #
        # Invisible on a Linux desktop, where all three sit in /usr/include and are found without
        # being named. It surfaced on Windows, where they do not: "missing path to pcre2".
        FOREACH (sptkDependency PCRE2_INCLUDE_DIR ZLIB_INCLUDE_DIR OPENSSL_INCLUDE_DIR)
            IF (${sptkDependency})
                LIST(APPEND extensionIncludes "${${sptkDependency}}")
            ENDIF ()
        ENDFOREACH ()
        LIST(APPEND extensionLibraries "${SPTK_LIBRARY_SPDB}" "${SPTK_LIBRARY_CORE}")
    ENDIF ()

    # OpenSSL's crypto library, for an extension that hashes or verifies something itself.
    #
    # Opt-in and separate from SPTK, because needing SPTK's database layer says nothing about
    # needing a KDF. Named rather than left to come in behind SPTK: on Linux libcrypto arrives as a
    # transitive dependency of libsputil5 and the link succeeds without anyone asking for it, which
    # is exactly the kind of accident that holds until the platform changes. On Windows it does not
    # hold - an import library carries no such transitivity - and the same source that linked on a
    # desktop failed with three unresolved externals: PKCS5_PBKDF2_HMAC, EVP_sha256, CRYPTO_memcmp.
    IF (EXTENSION_OPENSSL)
        IF (NOT TARGET OpenSSL::Crypto)
            MESSAGE(FATAL_ERROR
                    "XMQ_ADD_EXTENSION(${extensionName} ... OPENSSL) needs OpenSSL. "
                    "FIND_PACKAGE(OpenSSL) has to have run in a directory that reaches this one.")
        ENDIF ()
        LIST(APPEND extensionLibraries OpenSSL::Crypto)
    ENDIF ()

    ADD_LIBRARY(${extensionName} SHARED ${EXTENSION_SOURCES})

    # PRIVATE and exclusive: set, not appended, so nothing the enclosing directory added survives.
    SET_TARGET_PROPERTIES(${extensionName} PROPERTIES
            INCLUDE_DIRECTORIES "${extensionIncludes}"
            LINK_LIBRARIES "${extensionLibraries}"
            INTERFACE_LINK_LIBRARIES ""
            CXX_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN ON
            POSITION_INDEPENDENT_CODE ON)
    # These modules are loaded with dlopen(), so coverage runtime symbols must be resolved
    # by each module rather than relying on the executable's dynamic symbol table.
    IF (BUILD_WITH_COVERAGE AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        TARGET_LINK_LIBRARIES(${extensionName} PRIVATE gcov)
    ENDIF ()
ENDFUNCTION()
