ADD_LIBRARY(xmq_utils
        CommandProperties.cpp CommandProperties.h
        RunDefinition.cpp RunDefinition.h
        Utility.cpp Utility.h
        UtilityClient.cpp UtilityClient.h
        UtilityCommandLine.cpp UtilityCommandLine.h
        ExecuteParallel.cpp ExecuteParallel.h
        Reporter.cpp Reporter.h
        Scenario.cpp Scenario.h
        ScenarioCommandLine.cpp ScenarioCommandLine.h)

ADD_EXECUTABLE(xmq_pub
        Publisher.cpp Publisher.h
        PublisherCommandLine.cpp PublisherCommandLine.h)

ADD_EXECUTABLE(xmq_sub
        Subscriber.cpp Subscriber.h
        SubscriberCommandLine.cpp SubscriberCommandLine.h)

ADD_EXECUTABLE(xmq_con
        Connections.cpp Connections.h
        ConnectionsCommandLine.cpp ConnectionsCommandLine.h)

ADD_EXECUTABLE(xmq_scn ScenarioMain.cpp)

# The cluster test orchestrator: the same load scenarios, run while the cluster is changed under them.
ADD_EXECUTABLE(xmq_scn_cluster
        ClusterTestMain.cpp
        ClusterTestRunner.cpp ClusterTestRunner.h
        ClusterTestDefinition.cpp ClusterTestDefinition.h
        ClusterTestCommandLine.cpp ClusterTestCommandLine.h)

TARGET_LINK_LIBRARIES(xmq_pub xmq_server_core xmq_base xmq_storage xmq_common xmq_utils xmq_client)
TARGET_LINK_LIBRARIES(xmq_sub xmq_server_core xmq_base xmq_storage xmq_common xmq_utils xmq_client)
TARGET_LINK_LIBRARIES(xmq_con xmq_server_core xmq_base xmq_storage xmq_common xmq_utils xmq_client)
TARGET_LINK_LIBRARIES(xmq_scn xmq_server_core xmq_base xmq_storage xmq_common xmq_utils xmq_client xmq_service xmq_scenario)
TARGET_LINK_LIBRARIES(xmq_scn_cluster xmq_server_core xmq_base xmq_storage xmq_common xmq_utils xmq_client xmq_service xmq_scenario)
