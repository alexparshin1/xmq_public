FILE(GLOB XMQ_CLIENT_FILES *.cpp *.h)
ADD_LIBRARY(xmq_client ${XMQ_CLIENT_FILES})
TARGET_LINK_LIBRARIES(xmq_client xmq_base xmq_common)
