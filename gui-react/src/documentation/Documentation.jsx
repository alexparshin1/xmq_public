import React from "react";
import DocumentationIndex from "./DocumentationIndex";
import InstallDocumentation from "./InstallDocumentation";
import SetupDocumentation from "./SetupDocumentation";
import PersistenceDocumentation from "./PersistenceDocumentation";
import TestingDocumentation from "./TestingDocumentation";
import PermissionsDocumentation from "./PermissionsDocumentation";
import MosquittoImportDocumentation from "./MosquittoImportDocumentation";
import DataIntegrationDocumentation from "./DataIntegrationDocumentation";
import PrometheusDocumentation from "./PrometheusDocumentation";
import ClusterDocumentation from "./ClusterDocumentation";
import LoadBalancerDocumentation from "./LoadBalancerDocumentation";
import SizingDocumentation from "./SizingDocumentation";

const screens = {
    index: DocumentationIndex,
    install: InstallDocumentation,
    setup: SetupDocumentation,
    persistence: PersistenceDocumentation,
    testing: TestingDocumentation,
    permissions: PermissionsDocumentation,
    "mosquitto-import": MosquittoImportDocumentation,
    "data-integration": DataIntegrationDocumentation,
    prometheus: PrometheusDocumentation,
    cluster: ClusterDocumentation,
    sizing: SizingDocumentation,
    "load-balancer": LoadBalancerDocumentation
};

export default function Documentation({page = "index"}) {
    const Screen = screens[page];
    return <Screen/>;
}
