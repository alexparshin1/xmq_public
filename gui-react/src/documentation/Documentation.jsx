import React from "react";
import DocumentationIndex from "./DocumentationIndex";
import InstallDocumentation from "./InstallDocumentation";
import SetupDocumentation from "./SetupDocumentation";
import TestingDocumentation from "./TestingDocumentation";
import ClusterDocumentation from "./ClusterDocumentation";
import PermissionsDocumentation from "./PermissionsDocumentation";
import DataIntegrationDocumentation from "./DataIntegrationDocumentation";
import LoadBalancerDocumentation from "./LoadBalancerDocumentation";
import SizingDocumentation from "./SizingDocumentation";

const screens = {
    index: DocumentationIndex,
    install: InstallDocumentation,
    setup: SetupDocumentation,
    testing: TestingDocumentation,
    cluster: ClusterDocumentation,
    permissions: PermissionsDocumentation,
    "data-integration": DataIntegrationDocumentation,
    sizing: SizingDocumentation,
    "load-balancer": LoadBalancerDocumentation
};

export default function Documentation({page = "index"}) {
    const Screen = screens[page];
    return <Screen/>;
}
