import React from "react";
import {fireEvent, render, screen} from "@testing-library/react";
import MosquittoImportScreen from "./MosquittoImportScreen";

jest.mock("../components/MessageWindow", () => ({
    errorWindow: jest.fn(),
    informationWindow: jest.fn()
}));

beforeEach(() => {
    jest.clearAllMocks();
});

const passwordPlan = {
    new: [{username: "alice", status: "new", reason: ""}],
    existing: [{username: "bob", status: "existing", reason: ""}],
    unsupported: [{username: "old", status: "unsupported", reason: "SHA512-crypt"}],
    invalid: [],
    counts: {new: 1, existing: 1, unsupported: 1, invalid: 0}
};

const aclPlan = {
    new: [{group: "user:alice", action: "publish", deny: false, filter: "sensors/#"}],
    unchanged: [], redundant: [], conflicts: [], skipped: [],
    counts: {new: 1, unchanged: 0, redundant: 0, conflicts: 0}
};

function instance() {
    const screen = new MosquittoImportScreen({});
    screen.setState = update => {
        const patch = typeof update === "function" ? update(screen.state) : update;
        if (patch) {
            screen.state = {...screen.state, ...patch};
        }
    };
    return screen;
}

test("Preview reads both files through the server and keeps the plans apart", async () => {
    const ControlAPI = require("../ControlAPI").default;
    const calls = [];
    jest.spyOn(ControlAPI, "asyncMakeAPICall").mockImplementation((method, request) => {
        calls.push({method, request});
        if (method === "UserControl") {
            return Promise.resolve({result: {success: true}, plan: JSON.stringify(passwordPlan)});
        }
        return Promise.resolve({result: {success: true}, plan: JSON.stringify(aclPlan),
                                default_deny: false});
    });

    const screen = instance();
    screen.state.passwordContent = "alice:secret\n";
    screen.state.aclContent = "user alice\ntopic write sensors/#\n";
    expect(await screen.preview()).toEqual(expect.objectContaining({passwordPlan, aclPlan}));

    expect(calls.map(call => call.method)).toEqual(["UserControl", "PermissionsControl"]);
    expect(calls[1].request).toEqual({action: "import-plan", format: "acl",
                                      content: "user alice\ntopic write sensors/#\n"});
    expect(screen.state.passwordPlan.counts.new).toBe(1);
    expect(screen.state.aclPlan.counts.new).toBe(1);
    expect(screen.state.aclDefaultDeny).toBe(false);
});

test("An empty input is not sent at all", async () => {
    const ControlAPI = require("../ControlAPI").default;
    const call = jest.spyOn(ControlAPI, "asyncMakeAPICall");
    const screen = instance();
    screen.state.aclContent = "user alice\ntopic write sensors/#\n";
    jest.spyOn(ControlAPI, "asyncMakeAPICall").mockImplementation(
        () => Promise.resolve({result: {success: true}, plan: JSON.stringify(aclPlan), default_deny: false}));

    await screen.preview();
    expect(call.mock.calls.some(([method]) => method === "UserControl")).toBe(false);
});

test("Both inputs empty is refused before any call", async () => {
    const ControlAPI = require("../ControlAPI").default;
    const call = jest.spyOn(ControlAPI, "asyncMakeAPICall");
    const screen = instance();
    expect(await screen.preview()).toBe(false);
    expect(call).not.toHaveBeenCalled();
    expect(require("../components/MessageWindow").errorWindow).toHaveBeenCalled();
});

test("An ACL with conflicts cannot be applied from here", async () => {
    const ControlAPI = require("../ControlAPI").default;
    const call = jest.spyOn(ControlAPI, "asyncMakeAPICall");
    const screen = instance();
    // The plan is produced by the server; what matters here is that conflicts stop the apply.
    screen.preview = async () => ({
        passwordPlan: null,
        aclPlan: {...aclPlan, counts: {new: 0, unchanged: 0, redundant: 0, conflicts: 1}}
    });

    await screen.importAll();
    expect(call).not.toHaveBeenCalled();
    expect(require("../components/MessageWindow").errorWindow).toHaveBeenCalled();
});

test("The ACL preview says when the import turns default deny on", () => {
    const screen = instance();
    screen.state.aclPlan = aclPlan;
    screen.state.aclDefaultDeny = false;
    const {container} = render(screen.renderAclPlan());
    expect(container.textContent).toContain("turns on deny-by-default");
});
