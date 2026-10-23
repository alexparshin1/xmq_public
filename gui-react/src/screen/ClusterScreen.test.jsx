import React from "react";
import {render, screen as view} from "@testing-library/react";
import ClusterScreen from "./ClusterScreen";
import {confirmationWindow} from "../components/MessageWindow";

jest.mock("../components/DataTable", () => () => null);
jest.mock("../components/MessageWindow", () => ({
    confirmationWindow: jest.fn(), errorWindow: jest.fn(), informationWindow: jest.fn()
}));

beforeEach(() => jest.clearAllMocks());

test("an empty Redis membership offers Start cluster for the selected local node", async () => {
    const screen = new ClusterScreen({});
    screen.state.cluster = {enabled: false, cluster_state: "empty"};
    screen.state.selected = {node_id: "local-id", name: "theater", state: "Standalone", is_this_node: true};
    screen.send = jest.fn();
    confirmationWindow.mockResolvedValue(true);
    render(screen.renderNodeActions());
    expect(view.getByRole("button", {name: /Start cluster/})).toBeEnabled();
    screen.joinNode();
    await Promise.resolve();
    expect(screen.send).toHaveBeenCalledWith("start", {node: {node_id: "local-id", name: "theater"}}, expect.any(String));
});

test.each(["offline", "unknown"])("%s membership does not offer a new cluster", state => {
    const screen = new ClusterScreen({});
    screen.state.cluster = {enabled: false, cluster_state: state, cluster_members: [{name: "failed-host"}]};
    screen.state.selected = {node_id: "remote-id", name: "theater", state: "Standalone"};
    render(<>{screen.renderNodeActions()}{screen.renderClusterGuidance()}</>);
    expect(view.queryByRole("button", {name: /Start cluster/})).not.toBeInTheDocument();
    expect(view.getByRole("button", {name: /Join cluster/})).toBeDisabled();
    expect(view.getByRole("status")).toHaveTextContent(state === "offline" ? "Force remove node" : "Restore storage connectivity");
});

test("a working cluster still explains how to handle an Offline member", () => {
    const screen = new ClusterScreen({});
    screen.state.cluster = {enabled: true, cluster_state: "active", cluster_members: [
        {name: "theater", state: "Clustered"}, {name: "failed-host", state: "Offline"}
    ]};
    render(screen.renderClusterGuidance());
    expect(view.getByRole("status")).toHaveTextContent("Offline members: failed-host");
    expect(view.getByRole("status")).toHaveTextContent("Force remove node");
});

test("join sends one confirmed administrator action without asking for another password", async () => {
    const screen = new ClusterScreen({});
    screen.state.selected = {node_id: "node-guid", name: "theater", state: "Standalone"};
    screen.send = jest.fn();
    confirmationWindow.mockResolvedValue(true);
    screen.joinNode();
    await Promise.resolve();
    expect(confirmationWindow).toHaveBeenCalledTimes(1);
    expect(confirmationWindow).toHaveBeenCalledWith(expect.stringContaining("restart"));
    expect(screen.send).toHaveBeenCalledWith("join", {
        node: {node_id: "node-guid", name: "theater"}
    }, expect.any(String));
});

test("cancelling Join does not change membership", async () => {
    const screen = new ClusterScreen({});
    screen.state.selected = {node_id: "node-guid", name: "theater", state: "Standalone"};
    screen.send = jest.fn();
    confirmationWindow.mockResolvedValue(false);
    screen.joinNode();
    await Promise.resolve();
    expect(screen.send).not.toHaveBeenCalled();
});

test.each([false, true])("remove requires confirmation and sends force only as selected: %s", async (force) => {
    const screen = new ClusterScreen({});
    screen.state.selected = {node_id: "node-guid", name: "theater"};
    screen.send = jest.fn();
    confirmationWindow.mockResolvedValue(true);
    screen.removeNode(null, force);
    await Promise.resolve();
    expect(confirmationWindow).toHaveBeenCalledWith(expect.stringContaining("will not restart"));
    expect(confirmationWindow).toHaveBeenCalledWith(expect.stringContaining('"theater"'));
    expect(screen.send).toHaveBeenCalledWith("remove", {
        node: {node_id: "node-guid", name: "theater"}, force
    }, expect.any(String));
});

test("cancelling removal leaves the configuration untouched", async () => {
    const screen = new ClusterScreen({});
    screen.state.selected = {node_id: "node-guid", name: "theater"};
    screen.send = jest.fn();
    confirmationWindow.mockResolvedValue(false);
    screen.removeNode();
    await Promise.resolve();
    expect(screen.send).not.toHaveBeenCalled();
});

test.each([["leaveCluster", "disable"], ["retireConfiguration", "retire"]])(
    "%s is a separate confirmed action", async (method, action) => {
        const screen = new ClusterScreen({});
        screen.state.selected = {node_id: "node-guid", name: "theater"};
        screen.send = jest.fn();
        confirmationWindow.mockResolvedValue(true);
        screen[method]();
        await Promise.resolve();
        expect(confirmationWindow).toHaveBeenCalledWith(expect.stringContaining('"theater"'));
        expect(screen.send).toHaveBeenCalledWith(action, {node: {node_id: "node-guid", name: "theater"}}, expect.any(String));
    });

// The target belongs to the confirmation, even if another row is selected while it is open.
test.each([["joinNode", "join"], ["leaveCluster", "disable"], ["removeNode", "remove"],
           ["retireConfiguration", "retire"]])("%s confirms the named node and keeps that target", async (method, action) => {
    const screen = new ClusterScreen({});
    screen.state.selected = {node_id: "theater-id", name: "theater", state: "Standalone"};
    screen.send = jest.fn();
    let confirm;
    confirmationWindow.mockImplementation(() => new Promise(resolve => { confirm = resolve; }));
    screen[method]();
    expect(screen.send).not.toHaveBeenCalled();
    expect(confirmationWindow).toHaveBeenCalledWith(expect.stringContaining('"theater"'));
    screen.state.selected = {node_id: "other-id", name: "thinker11"};
    confirm(true);
    await Promise.resolve();
    expect(screen.send).toHaveBeenCalledWith(action, expect.objectContaining({
        node: {node_id: "theater-id", name: "theater"}
    }), expect.any(String));
});

test.each(["joinNode", "leaveCluster", "removeNode", "retireConfiguration"])(
    "cancelling %s sends no action", async method => {
        const screen = new ClusterScreen({});
        screen.state.selected = {node_id: "node-guid", name: "theater"};
        screen.send = jest.fn();
        confirmationWindow.mockResolvedValue(false);
        screen[method]();
        await Promise.resolve();
        expect(screen.send).not.toHaveBeenCalled();
    });

test("the action panel uses the selected node's state, rather than this server's mode", () => {
    const screen = new ClusterScreen({});
    screen.state.cluster = {enabled: true};
    screen.state.selected = {node_id: "remote", name: "theater", state: "Standalone"};
    const rendered = render(screen.renderNodeActions());
    expect(view.getByText("theater")).toBeInTheDocument();
    expect(view.getByRole("button", {name: /Join cluster/})).toBeEnabled();
    expect(view.getByRole("button", {name: /Leave cluster/})).toBeDisabled();
    expect(view.getByRole("button", {name: /Retire configuration/})).toBeEnabled();

    screen.state.cluster.enabled = false;
    screen.state.selected = {node_id: "remote", name: "thinker11", state: "Clustered"};
    rendered.rerender(screen.renderNodeActions());
    expect(view.getByText("thinker11")).toBeInTheDocument();
    expect(view.getByRole("button", {name: /Leave cluster/})).toBeEnabled();
    expect(view.getByRole("button", {name: /Retire configuration/})).toBeDisabled();
});

test("without a selection every node action is disabled", () => {
    const screen = new ClusterScreen({});
    screen.state.cluster = {enabled: true};
    render(screen.renderNodeActions());
    for (const name of [/Join cluster/, /Leave cluster/, /Retire configuration/, /^Remove node/, /Force remove/]) {
        expect(view.getByRole("button", {name})).toBeDisabled();
    }
    expect(view.getByRole("button", {name: /Add node/})).toBeEnabled();
});
