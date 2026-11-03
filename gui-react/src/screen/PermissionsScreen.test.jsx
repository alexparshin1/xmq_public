import React from "react";
import {render} from "@testing-library/react";
import PermissionsScreen from "./PermissionsScreen";

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

test("The Group field offers the groups the server knows", async () => {
    const ControlAPI = require("../ControlAPI").default;
    jest.spyOn(ControlAPI, "asyncMakeAPICall").mockImplementation(method => {
        if (method === "UserGroupControl") {
            return Promise.resolve({
                result: {success: true},
                list: [{name: "operators"}, {name: "water"}]
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
    await screen.readGroups();

    const group = screen.state.headers.find(header => header.field === "group");
    expect(group.context.inputType).toBe("combobox");
    expect(group.context.options).toEqual(["operators", "water", "anonymous"]);
});
