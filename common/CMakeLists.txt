FILE(GLOB XMQ_MQTT_FILES mqtt/*.cpp mqtt/*.h mqtt/properties/*.cpp mqtt/properties/*.h)
FILE(GLOB XMQ_COMMON_FILES *.cpp *.h)
ADD_LIBRARY(xmq_common ${XMQ_COMMON_FILES} ${XMQ_MQTT_FILES}
        Packet.cpp
        Packet.h)
TARGET_LINK_LIBRARIES(xmq_common xmq_base)
