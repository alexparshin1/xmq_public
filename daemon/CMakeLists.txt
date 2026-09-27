IF (BSD)
    # rc.d, relative to the prefix - /usr/local/etc/rc.d on FreeBSD. Relative and not absolute
    # because an absolute destination cannot be packaged; see XMQ_INSTALL_DESTINATION in the
    # top-level CMakeLists. PROGRAMS, not FILES: rc(8) will not run a script that is not executable.
    INSTALL(PROGRAMS xmq_server.rc DESTINATION etc/rc.d RENAME xmq_server)
ELSE ()
    EXECUTE_PROCESS(COMMAND id -u OUTPUT_VARIABLE user)
    IF (user EQUAL 0)
        INSTALL(FILES xmq_server.service xmq_test_suite.service DESTINATION /etc/systemd/system)
        EXECUTE_PROCESS(COMMAND sudo systemctl daemon-reload)
    ENDIF ()
ENDIF ()
