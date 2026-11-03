import React from "react";
import {fireEvent, render, screen} from "@testing-library/react";
import PermissionsScreen, {validTopicFilter} from "./PermissionsScreen";

jest.mock("../components/MessageWindow", () => ({
    errorWindow: jest.fn(),
    informationWindow: jest.fn(),
    confirmationWindow: jest.fn()
}));

beforeEach(() => {
    jest.clearAllMocks();
});

function instance() {
    const screen = new PermissionsScreen({});
    screen.setState = update => {
        const patch = typeof update === "function" ? update(screen.state) : update;
        if (patch) {
            screen.state = {...screen.state, ...patch};
        }
    };
    return screen;
}

test.each([
    ["allow", "Allowed"],
    ["deny", "Denied"],
    ["not_handled", "Not covered by any rule"]
])("Permission test access reports %s as %s", (decision, wording) => {
    const screen = new PermissionsScreen({});
    screen.state.testResult = {decision, explanation: "because"};
    const {container} = render(screen.renderDecision());
    expect(container.textContent).toContain(wording);
    expect(container.textContent).toContain("because");
});

test("An import plan with a conflict has to be resolved before it is applied", () => {
    const screen = new PermissionsScreen({});
    screen.state.importPlan = {
        new: [], unchanged: [], redundant: [], skipped: [],
        conflicts: [{
            imported: {group: "user:a", action: "publish", deny: true, filter: "sensors/#"},
            existing: {group: "user:a", action: "publish", deny: false, filter: "sensors/temp"},
            scope: "sensors/temp"
        }],
        counts: {new: 0, unchanged: 0, redundant: 0, conflicts: 1}
    };
    const {container} = render(screen.renderImportPlan());
    expect(container.textContent).toContain("conflict");
    expect(container.textContent).toContain("sensors/temp");
    expect(container.querySelector("select")).not.toBeNull();
    expect(container.textContent).toContain("Apply import");
});

test("A plan without conflicts does not ask for a resolution", () => {
    const screen = new PermissionsScreen({});
    screen.state.importPlan = {
        new: [{group: "user:a", action: "subscribe", deny: false, filter: "sensors/#"}],
        unchanged: [], redundant: [], conflicts: [], skipped: [],
        counts: {new: 1, unchanged: 0, redundant: 0, conflicts: 0}
    };
    const {container} = render(screen.renderImportPlan());
    expect(container.querySelector("select")).toBeNull();
    expect(container.textContent).toContain("1 new");
});

test("The policy editor offers the groups and accounts the server knows", async () => {
    const ControlAPI = require("../ControlAPI").default;
    jest.spyOn(ControlAPI, "asyncMakeAPICall").mockImplementation(method => {
        if (method === "UserGroupControl") {
            return Promise.resolve({result: {success: true}, list: [{name: "operators"}, {name: "water"}]});
        }
        if (method === "UserControl") {
            return Promise.resolve({result: {success: true}, list: [{username: "alice"}]});
        }
        return Promise.resolve({result: {success: true}, rules: []});
    });

    const screen = instance();
    await screen.readSubjects();
    expect(screen.state.subjectOptions.groups).toEqual(["operators", "water", "anonymous"]);
    expect(screen.state.subjectOptions.users).toEqual(["alice"]);
});

test("Rules are grouped into one ordered policy per subject", () => {
    const screen = instance();
    screen.state.rows = [
        {id: 1, group: "operators", pattern: false, order: 20, deny: true, action: "subscribe",
         filter: "sensors/secret/#"},
        {id: 2, group: "operators", pattern: false, order: 10, deny: false, action: "subscribe",
         filter: "sensors/#"},
        {id: 3, group: "user:alice", pattern: false, order: 30, deny: false, action: "publish",
         filter: "device/1"}
    ];

    const policies = screen.policies();
    expect(policies.map(policy => policy.subject)).toEqual(["operators", "user:alice"]);
    // The list is the policy: evaluation order, not the order the rows arrived in.
    expect(policies[0].rules.map(rule => rule.filter)).toEqual(["sensors/#", "sensors/secret/#"]);
});

test("A pattern policy is titled as applying to all clients", () => {
    const screen = instance();
    screen.state.rows = [
        {id: 1, group: "*", pattern: true, order: 10, deny: false, action: "publish",
         filter: "devices/%c/#"}
    ];
    expect(screen.policyTitle(screen.policies()[0])).toBe("All clients");
});

test("The editor refuses a bad filter, a duplicate and an empty policy", () => {
    const screen = instance();

    const editor = entries => ({
        subject: "operators", pattern: false, original: null, entries, error: ""
    });

    expect(screen.policyProblem(editor([]))).toContain("at least one rule");
    expect(screen.policyProblem(editor([
        {deny: false, action: "subscribe", filter: "//s/#"}
    ]))).toContain("not a valid topic filter");
    expect(screen.policyProblem(editor([
        {deny: false, action: "subscribe", filter: "s/#"},
        {deny: false, action: "subscribe", filter: "s/#"}
    ]))).toContain("repeats");
    expect(screen.policyProblem(editor([
        {deny: false, action: "subscribe", filter: "s/#"}
    ]))).toBeNull();
});

test("Saving a policy sends the whole ordered list in one call", async () => {
    const ControlAPI = require("../ControlAPI").default;
    const call = jest.spyOn(ControlAPI, "asyncMakeAPICall").mockImplementation(
        () => Promise.resolve({result: {success: true}, rules: []}));

    const screen = instance();
    screen.readRules = () => {};
    screen.state.policyEditor = {
        subject: "operators", pattern: false, original: null, error: "",
        entries: [
            {deny: true, action: "subscribe", filter: "sensors/secret/#"},
            {deny: false, action: "subscribe", filter: "sensors/#"}
        ]
    };
    await screen.savePolicy();

    expect(call).toHaveBeenCalledWith("PermissionsControl", {
        action: "replace-policy",
        subject: "operators",
        subject_pattern: false,
        policy_rules: [
            {group: "operators", action: "subscribe", deny: true, filter: "sensors/secret/#"},
            {group: "operators", action: "subscribe", deny: false, filter: "sensors/#"}
        ]
    });
    expect(screen.state.policyEditor).toBeNull();
});

test("Rules and Test Access are tabs, and the test panel is not below the table", () => {
    const ControlAPI = require("../ControlAPI").default;
    jest.spyOn(ControlAPI, "asyncMakeAPICall").mockImplementation(
        () => Promise.resolve({result: {success: true}, rules: [], list: []}));

    const {container} = render(<PermissionsScreen/>);
    expect(screen.getByRole("tab", {name: "Rules"})).toHaveAttribute("aria-selected", "true");
    expect(container.textContent).toContain("Each row is one user");
    expect(container.textContent).not.toContain("Answers the question a rule is meant to answer");

    fireEvent.click(screen.getByRole("tab", {name: "Test Access"}));
    expect(screen.getByRole("tab", {name: "Test Access"})).toHaveAttribute("aria-selected", "true");
    expect(container.textContent).toContain("Answers the question a rule is meant to answer");
    expect(container.textContent).not.toContain("Each row is one user");
});

test.each([
    "s", "s/#", "s/+/temp", "#", "+", "$SYS/#", "a/b/c"
])("A topic filter the server accepts is accepted here: %s", filter => {
    expect(validTopicFilter(filter)).toBe(true);
});

test.each([
    "", "//s/#", "s//temp", "s/", "/s", "s/##", "s/+x", "s/#/x", "s+/x"
])("A topic filter the server refuses is refused here: %s", filter => {
    expect(validTopicFilter(filter)).toBe(false);
});

test("Test access sends the client id, which a %c rule needs", async () => {
    const ControlAPI = require("../ControlAPI").default;
    const call = jest.spyOn(ControlAPI, "asyncMakeAPICall").mockImplementation(
        () => Promise.resolve({result: {success: true}, decision: "allow", explanation: "allowed"}));

    const screen = instance();
    screen.state.testUsername = "";
    screen.state.testClientId = "device-7";
    screen.state.testTopic = "devices/device-7/temp";
    screen.state.testAction = "subscribe";
    await screen.runTest();

    expect(call).toHaveBeenCalledWith("PermissionsControl", {
        action: "test", username: "", client_id: "device-7",
        topic: "devices/device-7/temp", acl_action: "subscribe"
    });
});

test("The default-deny policy is read with the rules and can be changed", async () => {
    const ControlAPI = require("../ControlAPI").default;
    const call = jest.spyOn(ControlAPI, "asyncMakeAPICall").mockImplementation((method, request) => {
        if (request.action === "list") {
            return Promise.resolve({result: {success: true}, rules: [], default_deny: true});
        }
        return Promise.resolve({result: {success: true}, default_deny: false});
    });

    const screen = instance();
    await screen.readRules();
    expect(screen.state.defaultDeny).toBe(true);

    await screen.setDefaultDeny(false);
    expect(screen.state.defaultDeny).toBe(false);
    expect(call).toHaveBeenCalledWith("PermissionsControl",
        {action: "set-policy", default_deny: false});
});

test("Removing a policy sends an empty list for its subject", async () => {
    const ControlAPI = require("../ControlAPI").default;
    const call = jest.spyOn(ControlAPI, "asyncMakeAPICall").mockImplementation(
        () => Promise.resolve({result: {success: true}, rules: []}));
    const MessageWindow = require("../components/MessageWindow");
    MessageWindow.confirmationWindow.mockResolvedValue(true);

    const screen = instance();
    screen.readRules = () => {};
    screen.removePolicy({subject: "operators", pattern: false, rules: []});

    // The confirmation is a promise; let its handler run.
    await Promise.resolve();
    await Promise.resolve();
    expect(call).toHaveBeenCalledWith("PermissionsControl", {
        action: "replace-policy", subject: "operators", subject_pattern: false, policy_rules: []
    });
});
