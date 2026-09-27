# Into the build tree, not back into the sources. What it holds depends on the installation prefix,
# so writing it into the source tree meant an ordinary developer build left paths from that
# developer's home in a tracked file, waiting to be committed by whoever did not look. Each build
# tree now has its own, which is also what lets two of them be configured for different prefixes.
CONFIGURE_FILE("xmq-config.h.cmake" "${CMAKE_CURRENT_BINARY_DIR}/xmq-config.h" @ONLY)

FILE(GLOB XMQ_BASE_FILES *.cpp *.h)
ADD_LIBRARY(xmq_base ${XMQ_BASE_FILES})
