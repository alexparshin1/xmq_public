FILE(GLOB XMQ_SERVER_FILES *.cpp *.h
        Bridge/*
        Cluster/*
        ClientSession/*
        Extensions/*
        Settings/*
        Subscription/*
        interface/*
)

INCLUDE_DIRECTORIES(. Bridge Cluster ClientSession Extensions Settings Subscription)

ADD_LIBRARY(xmq_server_core ${XMQ_SERVER_FILES})

TARGET_LINK_LIBRARIES(xmq_server_core xmq_base xmq_storage xmq_common xmq_service xmq_client)
