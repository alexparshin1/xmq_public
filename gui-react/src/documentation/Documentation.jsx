import React from "react";
import DocumentationIndex from "./DocumentationIndex";
import InstallDocumentation from "./InstallDocumentation";
import SetupDocumentation from "./SetupDocumentation";
import PersistenceDocumentation from "./PersistenceDocumentation";
import TestingDocumentation from "./TestingDocumentation";
import PermissionsDocumentation from "./PermissionsDocumentation";
import ExtensionsDocumentation from "./ExtensionsDocumentation";
import UserDatabaseDocumentation from "./UserDatabaseDocumentation";
import LdapDocumentation from "./LdapDocumentation";
import JwtDocumentation from "./JwtDocumentation";
import HttpAuthDocumentation from "./HttpAuthDocumentation";
import WebhookDocumentation from "./WebhookDocumentation";
import MosquittoImportDocumentation from "./MosquittoImportDocumentation";
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
    extensions: ExtensionsDocumentation,
    "user-database": UserDatabaseDocumentation,
    ldap: LdapDocumentation,
    jwt: JwtDocumentation,
    "http-auth": HttpAuthDocumentation,
    webhook: WebhookDocumentation,
    "mosquitto-import": MosquittoImportDocumentation,
    prometheus: PrometheusDocumentation,
    cluster: ClusterDocumentation,
    sizing: SizingDocumentation,
    "load-balancer": LoadBalancerDocumentation
};

export default function Documentation({page = "index"}) {
    const Screen = screens[page];
    return <Screen/>;
}
