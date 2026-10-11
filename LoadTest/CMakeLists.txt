# What the load generator needs beside it, and it is not the same everywhere.
#
# The shell scripts drive xmq_scn and tune a host for a load test; they are shell, so they go to
# every Unix and to no Windows. The setup/ tree is Linux and only Linux - sysctl.d, limits.d,
# modules-load.d, an nftables rule and a systemd unit have no meaning on FreeBSD, which had been
# carrying all five, and less than none on Windows, whose package shipped them together with seven
# .sh files.
FILE(GLOB XMQ_SCN_FILES *.sh)
# Local to our own machines and ignored by git (see .gitignore): the glob would still find them on a
# disk that has them, and the 0.9.18 package shipped both that way.
LIST(FILTER XMQ_SCN_FILES EXCLUDE REGEX "/(aws_campaign|init_environment_thinker10)\\.sh$")
FILE(GLOB XMQ_SCN_SYSCTL_FILES setup/sysctl.d/*.conf)
FILE(GLOB XMQ_SCN_LIMITS_FILES setup/limits.d/*.conf)
FILE(GLOB XMQ_SCN_MODULES_FILES setup/modules-load.d/*.conf)
FILE(GLOB XMQ_SCN_NFTABLES_FILES setup/nftables.d/*.nft)
FILE(GLOB XMQ_SCN_SYSTEMD_FILES setup/systemd/*.service)

IF (NOT WIN32)
    INSTALL(PROGRAMS ${XMQ_SCN_FILES} DESTINATION share/xmq)
ENDIF ()

IF (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    INSTALL(FILES ${XMQ_SCN_SYSCTL_FILES} DESTINATION share/xmq/setup/sysctl.d)
    INSTALL(FILES ${XMQ_SCN_LIMITS_FILES} DESTINATION share/xmq/setup/limits.d)
    INSTALL(FILES ${XMQ_SCN_MODULES_FILES} DESTINATION share/xmq/setup/modules-load.d)
    INSTALL(FILES ${XMQ_SCN_NFTABLES_FILES} DESTINATION share/xmq/setup/nftables.d)
    INSTALL(FILES ${XMQ_SCN_SYSTEMD_FILES} DESTINATION share/xmq/setup/systemd)
    INSTALL(FILES setup/README.md DESTINATION share/xmq/setup)
ENDIF ()

# The cluster tests that xmq_scn_cluster runs, and the test cluster they run on. test_cluster.sh is a
# Unix tool - bash, ssh, openssl, pgrep - and goes to every Unix; the
# tests and their load scenario are what xmq_scn_cluster reads anywhere, Windows included, and serve
# as examples: their nodes are our machines, and a test is moved to others by editing its nodes.
FILE(GLOB XMQ_CLUSTER_TESTS Cluster/*.json)
IF (NOT WIN32)
    INSTALL(PROGRAMS Cluster/test_cluster.sh DESTINATION share/xmq/cluster)
ENDIF ()
INSTALL(FILES ${XMQ_CLUSTER_TESTS} Cluster/README.md DESTINATION share/xmq/cluster)
