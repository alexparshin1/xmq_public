import React from "react";
import {fireEvent, render, screen, waitFor} from "@testing-library/react";
import ConfigurationsScreen from "./ConfigurationsScreen";

/**
 * A required extension cannot be switched off, and the user-database's empty URI field is a copy
 * of a setting the broker already owns. Neither belongs in an editable configuration dialog.
 */
test("the State column agrees with the Enabled checkbox and the extension-wide switch", () => {
    const page = new ConfigurationsScreen({page: "authentication"});
    page.state = {
        extensions: [
            {name: "http-auth", page: "authentication", running: false,
                extension_enabled: true, configuration_enabled: true},
            {name: "ldap", page: "authentication", running: true,
                extension_enabled: true, configuration_enabled: true},
            {name: "jwt", page: "authentication", running: false,
                extension_enabled: true, configuration_enabled: false},
            {name: "user-database", page: "authentication", running: false,
                extension_enabled: false, configuration_enabled: true}
        ],
        busy: false
    };

    expect(page.rows().map(row => row.state))
        .toEqual(["enabled", "active", "disabled", "extension disabled"]);
});

test("an authorizer has one broker-wide configuration and no Add button", () => {
    const page = new ConfigurationsScreen({page: "permissions"});
    page.state = {
        extensions: [{name: "topic-guard", page: "permissions", authorizer: true}],
        busy: false
    };

    expect(page.addTypes()).toEqual([]);
    render(<div>{page.renderActions()}</div>);
    expect(screen.getByText("One broker-wide configuration")).toBeInTheDocument();
    expect(screen.queryByRole("button", {name: "Add"})).not.toBeInTheDocument();
    expect(screen.queryByRole("button", {name: "Delete"})).not.toBeInTheDocument();
});

test("a family declaration lets the form add a concrete group key", () => {
    const page = new ConfigurationsScreen({page: "permissions"});
    page.setState = update => {
        const patch = typeof update === "function" ? update(page.state) : update;
        if (patch) page.state = {...page.state, ...patch};
    };
    Object.assign(page.state, {
        extensions: [{
            name: "topic-guard", page: "permissions", configuration_enabled: true,
            settings: [
                {name: "groups", label: "Group names", type: "string", value: ""},
                {name: "group.*", label: "Members of this group", type: "string"},
                {name: "default", label: "Topics no rule covers", type: "choice", value: "abstain"}
            ]
        }],
        selected: "topic-guard", showEdit: true, edited: {}, newKeyDrafts: {}, busy: false, editEnabled: true
    });

    const view = render(<div>{page.renderEditDialog()}</div>);
    fireEvent.change(view.container.querySelector('input[placeholder^="new group."]'),
                     {target: {value: "operators"}});
    expect(page.state.newKeyDrafts["group.*"]).toBe("operators");
    view.rerender(<div>{page.renderEditDialog()}</div>);
    fireEvent.click(screen.getByRole("button", {name: "Add"}));
    view.rerender(<div>{page.renderEditDialog()}</div>);

    expect(page.state.edited["group.operators"]).toBe("");
    expect(screen.getByText(/group\.operators/)).toBeInTheDocument();
    expect(screen.queryByText("group.*")).not.toBeInTheDocument();
});

test("Ok with untouched fields writes only the flag, not blank settings", async () => {
    const page = new ConfigurationsScreen({});
    page.state = {
        extensions: [{name: "ldap", settings: [{name: "host", value: "dc.example.org"}]}],
        selected: "ldap",
        edited: {},
        busy: false
    };
    page.apply = jest.fn();
    page.saveConfigEnabled = jest.fn().mockResolvedValue({result: {success: true}, message: "done"});
    page.closeEdit = jest.fn();
    page.refresh = jest.fn();

    page.applyEdit();
    await waitFor(() => expect(page.refresh).toHaveBeenCalled());

    expect(page.apply).not.toHaveBeenCalled();
    expect(page.saveConfigEnabled).toHaveBeenCalledTimes(1);
});

test("the Enabled switch is locked for a required extension, and an unused URI is not shown", () => {
    const page = new ConfigurationsScreen({});
    page.state = {
        extensions: [{
            name: "user-database",
            required: true,
            configuration_enabled: true,
            settings: [
                {name: "database_uri", label: "Database URI", type: "string", value: ""},
                {name: "username", label: "Database user", type: "string", value: ""},
                {name: "password", label: "Database password", type: "secret", value: ""},
                {name: "unknown_user", label: "Unknown user", type: "choice", value: "deny"}
            ]
        }],
        selected: "user-database",
        showEdit: true,
        editEnabled: true,
        edited: {},
        busy: false,
        preset: "",
        presetResult: null
    };

    render(<div>{page.renderEditDialog()}</div>);
    expect(screen.getByRole("checkbox", {name: /Enabled/})).toBeDisabled();
    expect(screen.queryByLabelText("Database URI")).not.toBeInTheDocument();
    expect(screen.getByText(/account database URI comes from Initial Setup/)).toBeInTheDocument();
});

