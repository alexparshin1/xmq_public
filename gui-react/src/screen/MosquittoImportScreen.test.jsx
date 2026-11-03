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
    const call = jest.spyOn(ControlAPI, "asyncMakeAPICall").mockImplementation(
        () => Promise.resolve({
            result: {success: true},
            password_plan: JSON.stringify(passwordPlan),
            acl_plan: JSON.stringify(aclPlan),
            default_deny: false
        }));

    const screen = instance();
    screen.state.passwordContent = "alice:secret\n";
    screen.state.aclContent = "user alice\ntopic write sensors/#\n";
    expect(await screen.preview()).toEqual(expect.objectContaining({passwordPlan, aclPlan}));

    expect(call).toHaveBeenCalledTimes(1);
    expect(call).toHaveBeenCalledWith("MosquittoImportControl", {
        action: "plan",
        password_content: "alice:secret\n",
        acl_format: "acl",
        acl_content: "user alice\ntopic write sensors/#\n"
    });
    expect(screen.state.passwordPlan.counts.new).toBe(1);
    expect(screen.state.aclPlan.counts.new).toBe(1);
    expect(screen.state.aclDefaultDeny).toBe(false);
});

test("An empty input is still sent, and the server treats it as absent", async () => {
    const ControlAPI = require("../ControlAPI").default;
    const call = jest.spyOn(ControlAPI, "asyncMakeAPICall").mockImplementation(
        () => Promise.resolve({result: {success: true}, password_plan: JSON.stringify(passwordPlan),
                               default_deny: false}));
    const screen = instance();
    screen.state.passwordContent = "alice:secret\n";

    // One call for the whole import means the empty half is simply an empty string; the plan says
    // which half had anything in it.
    await screen.preview();
    expect(call).toHaveBeenCalledWith("MosquittoImportControl",
        expect.objectContaining({action: "plan", password_content: "alice:secret\n", acl_content: ""}));
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

test("A .json ACL file is read as dynamic-security", async () => {
    const ControlAPI = require("../ControlAPI").default;
    const call = jest.spyOn(ControlAPI, "asyncMakeAPICall").mockImplementation(
        () => Promise.resolve({result: {success: true}, password_plan: JSON.stringify(passwordPlan),
                               acl_plan: JSON.stringify(aclPlan), default_deny: false}));

    const screen = instance();
    screen.aclLoaded("dynamic-security.json", "{\"clients\":[]}");
    expect(screen.state.aclFormat).toBe("dynsec");
    await screen.preview();
    expect(call).toHaveBeenCalledWith("MosquittoImportControl",
        expect.objectContaining({action: "plan", acl_format: "dynsec", acl_content: "{\"clients\":[]}"}));
});

test("Import applies both halves in one call", async () => {
    const ControlAPI = require("../ControlAPI").default;
    const call = jest.spyOn(ControlAPI, "asyncMakeAPICall").mockImplementation((method, request) => {
        if (request.action === "plan") {
            return Promise.resolve({result: {success: true},
                                    password_plan: JSON.stringify(passwordPlan),
                                    acl_plan: JSON.stringify(aclPlan), default_deny: false});
        }
        return Promise.resolve({result: {success: true}, new_accounts: 2, new_rules: 3,
                                default_deny: true});
    });

    const screen = instance();
    screen.state.passwordContent = "alice:secret\n";
    screen.state.aclContent = "user alice\ntopic write sensors/#\n";
    await screen.importAll();

    expect(call).toHaveBeenCalledTimes(2);
    expect(call).toHaveBeenNthCalledWith(2, "MosquittoImportControl", {
        action: "apply",
        password_content: "alice:secret\n",
        acl_format: "acl",
        acl_content: "user alice\ntopic write sensors/#\n",
        resolution: "reject"
    });
    expect(screen.state.result.new_accounts).toBe(2);
});
