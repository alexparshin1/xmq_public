import React from "react";
import {render} from "@testing-library/react";
import DashboardScreen from "./DashboardScreen";

test.each([
    ["postgresql://xmq:secret@theater/xmq", "postgresql://xmq@theater/xmq"],
    ["mysql://xmq:sec%40ret@theater:3306/xmq", "mysql://xmq@theater:3306/xmq"],
    ["postgresql://xmq:test#123@theater/xmq", "postgresql://xmq@theater/xmq"],
    ["postgresql://xmq@theater/xmq", "postgresql://xmq@theater/xmq"]
])("Dashboard hides the accounts database password: %s", (uri, displayed) => {
    const dashboard = new DashboardScreen({});
    dashboard.state.cluster = {enabled: true, database_uri: uri};
    const {container} = render(dashboard.renderCluster());
    expect(container.querySelector(".inputScreenHint")).toHaveTextContent(displayed);
    if (uri !== displayed) expect(container.textContent).not.toContain(uri);
    expect(dashboard.state.cluster.database_uri).toBe(uri);
});
