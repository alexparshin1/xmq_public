import React from "react";
import DocumentationIndex from "./DocumentationIndex";
import InstallDocumentation from "./InstallDocumentation";
import SetupDocumentation from "./SetupDocumentation";
import TestingDocumentation from "./TestingDocumentation";
import ClusterDocumentation from "./ClusterDocumentation";

const screens = {
    index: DocumentationIndex,
    install: InstallDocumentation,
    setup: SetupDocumentation,
    testing: TestingDocumentation,
    cluster: ClusterDocumentation
};

export default function Documentation({page = "index"}) {
    const Screen = screens[page];
    return <Screen/>;
}
