import React from "react";
import {render} from "@testing-library/react";
import PermissionsScreen, {validTopicFilter} from "./PermissionsScreen";

jest.mock("../components/MessageWindow", () => ({
    errorWindow: jest.fn(),
    informationWindow: jest.fn()
}));

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
    // The conflict is shown together with the two rules it is between, and the resolution is asked
    // for rather than guessed.
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

test("The subject editor offers the groups and accounts the server knows", async () => {
    const ControlAPI = require("../ControlAPI").default;
    jest.spyOn(ControlAPI, "asyncMakeAPICall").mockImplementation(method => {
        if (method === "UserGroupControl") {
            return Promise.resolve({
                result: {success: true},
                list: [{name: "operators"}, {name: "water"}]
            });
        }
        if (method === "UserControl") {
            return Promise.resolve({
                result: {success: true},
                list: [{username: "alice"}, {username: "bob"}]
            });
        }
        return Promise.resolve({result: {success: true}, rules: []});
    });

    const screen = new PermissionsScreen({});
    // The state is what the table is handed, so it is what the dialog will read; rendering the
    // grid adds nothing to this question.
    screen.setState = update => {
        const patch = typeof update === "function" ? update(screen.state) : update;
        if (patch) {
            screen.state = {...screen.state, ...patch};
        }
    };
    await screen.readSubjects();

    const group = screen.state.headers.find(header => header.field === "group");
    expect(group.context.inputType).toBe("subjects");
    expect(group.context.options.groups).toEqual(["operators", "water", "anonymous"]);
    expect(group.context.options.users).toEqual(["alice", "bob"]);
});

test("A rule for several subjects is stored as one rule for each", async () => {
    const ControlAPI = require("../ControlAPI").default;
    const calls = [];
    jest.spyOn(ControlAPI, "asyncMakeAPICall").mockImplementation((method, request) => {
        calls.push({method, request});
        return Promise.resolve({result: {success: true}, rules: []});
    });

    const screen = new PermissionsScreen({});
    screen.setState = () => {};
    screen.readRules = () => {};

    await screen.saveRule(["operators", "user:bob"], {
        action: "subscribe", deny: false, filter: "plant/#", source: "manual", source_ref: ""
    }, null, null);

    const additions = calls.filter(call => call.request.action === "add");
    expect(additions.map(call => call.request.rule.group))
        .toEqual(["operators", "user:bob"]);
});

test("Editing drops the old rule only when its subject is not kept", async () => {
    const ControlAPI = require("../ControlAPI").default;
    const calls = [];
    jest.spyOn(ControlAPI, "asyncMakeAPICall").mockImplementation((method, request) => {
        calls.push({method, request});
        if (request.action === "add") {
            return Promise.resolve({
                result: {success: true},
                rules: [{id: 42, group: request.rule.group, action: request.rule.action,
                         deny: request.rule.deny, filter: request.rule.filter}]
            });
        }
        return Promise.resolve({result: {success: true}, rules: []});
    });

    const screen = new PermissionsScreen({});
    screen.setState = () => {};
    screen.readRules = () => {};
    screen.state.rows = [{id: 7, group: "operators"}];

    // The original subject was removed and another added: the old row has to go.
    await screen.saveRule(["water"], {
        action: "subscribe", deny: false, filter: "plant/#", source: "manual", source_ref: ""
    }, {group: "operators"}, 7);

    expect(calls.some(call => call.request.action === "remove" && call.request.rule_id === 7))
        .toBe(true);
});

test.each([
    "s", "s/#", "s/+/temp", "#", "+", "$SYS/#", "a/b/c"
])("A topic filter the server accepts is accepted here: %s", filter => {
    expect(validTopicFilter(filter)).toBe(true);
});

test.each([
    "", "//s/#", "s//temp", "s/", "/s", "s/##", "s/+x", "s/#/x", "s+/x"
])("A topic filter the server refuses is refused here: %s", filter => {
    // The check runs before the dialog closes, so a typo is reported while it can still be fixed.
    expect(validTopicFilter(filter)).toBe(false);
});

test("Adding a rule with an empty topic level is refused before the server is asked", async () => {
    const ControlAPI = require("../ControlAPI").default;
    const call = jest.spyOn(ControlAPI, "asyncMakeAPICall").mockImplementation(
        () => Promise.resolve({result: {success: true}, rules: []}));
    const MessageWindow = require("../components/MessageWindow");

    const screen = new PermissionsScreen({});
    screen.state.rows = [];
    screen.onRowDataChange("add", {
        group: ["operators"], action: "subscribe", access: "allow", filter: "//s/#"
    });

    expect(call).not.toHaveBeenCalled();
    expect(MessageWindow.errorWindow).toHaveBeenCalled();
});
