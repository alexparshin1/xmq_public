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

test.each([
    [true, "Cluster"],
    [false, "Standalone"]
])("Dashboard shows the mode as text, not as a control (%s)", (enabled, mode) => {
    const dashboard = new DashboardScreen({});
    dashboard.state.cluster = {enabled};
    const {container} = render(dashboard.renderCluster());
    // Join and Leave on the Cluster Configuration page are the one way the mode changes; a second
    // way here would be a second answer to the same question.
    expect(container.querySelector("select")).toBeNull();
    expect(container.querySelector("input")).toBeNull();
    expect(container.textContent).toContain(mode);
});
