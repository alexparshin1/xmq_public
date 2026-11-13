import React from "react";
import {fireEvent, render, screen, waitFor} from "@testing-library/react";
import ExtensionsScreen from "./ExtensionsScreen";
import ControlAPI from "../ControlAPI";

/*
 * Presets are the extension's own statement about which of its settings belong together, and the
 * screen's job is narrow: fill the fields from the chosen one, apply nothing, and never invent a
 * setting the form does not show. These tests hold the screen to that, because a preset that fills
 * a field wrongly is worse than no preset at all - it looks authoritative.
 */
const extension = {
    name: "ldap",
    version: "0.1",
    description: "LDAP bind authentication",
    library: "/tmp/libxmq_ldap.so",
    source: "/tmp/90-ldap.conf",
    running: true,
    required: false,
    settings: [
        {name: "host", label: "LDAP host", type: "string", value: "dc1.example.org", required: true,
            declared: true},
        {name: "directory_type", label: "Directory type", type: "choice", choices: "ldap,ad",
            value: "ldap", declared: true},
        {name: "encryption", label: "LDAP encryption", type: "choice", choices: "ldaps,starttls",
            value: "ldaps", declared: true},
        {name: "port", label: "LDAP port", type: "integer", value: "389", declared: true},
        {name: "nested_groups", label: "Nested groups", type: "choice", choices: "auto,yes,no",
            value: "auto", declared: true},
        {name: "user_filter", label: "User search filter", type: "string",
            value: "(uid={username})", declared: true}
    ],
    presets: [{
        name: "ad",
        label: "Active Directory",
        description: "Active Directory: login by sAMAccountName or user principal name.",
        choice_setting: "directory_type",
        choice_value: "ad",
        settings: [
            {name: "directory_type", value: "ad"},
            {name: "encryption", value: "ldaps"},
            {name: "port", value: "636"},
            {name: "nested_groups", value: "yes"},
            {name: "user_filter", value: ""},
            // A preset written against a newer extension: the screen must ignore what it cannot show.
            {name: "something_new", value: "ignored"}
        ]
    }]
};

let listed;

beforeEach(() => {
    listed = [extension];
    jest.spyOn(ControlAPI, "asyncMakeAPICall").mockImplementation((service, request) => {
        if (service === "ServerControl") {
            return Promise.resolve({running: true});
        }
        if (request && request.action === "list") {
            return Promise.resolve({list: listed});
        }
        return Promise.resolve({message: "saved"});
    });
    ControlAPI.apiError = () => null;
});

afterEach(() => {
    jest.restoreAllMocks();
});

const openExtension = async () => {
    render(<ExtensionsScreen/>);
    fireEvent.click(await screen.findByText("ldap"));
};

const controlOf = (label) => {
    // By its own row rather than the first text match: the preset summary names the same settings,
    // and a lookup by text would find the sentence instead of the field.
    const row = [...document.querySelectorAll(".extensionRow")]
        .find(one => (one.querySelector("label")?.textContent || "").startsWith(label));
    return row.querySelector("select, input");
};

const chosenPreset = () => {
    fireEvent.change(screen.getByLabelText("Set defaults"), {target: {value: "ad"}});
    fireEvent.click(screen.getByRole("button", {name: "Preset"}));
};

test("a sidecar form switches visible fields without saving or losing their values", async () => {
    listed = [{...extension, ui_definition: JSON.stringify({version: 1,
        sections: [{title: "Directory profile", fields: [
            {name: "directory_type"},
            {name: "host", visible_when: {setting: "directory_type", values: ["ad"]}},
            {name: "user_filter", widget: "multiline"}
        ]}],
        presets: [{name: "ad", label: "AD profile", choice_setting: "directory_type",
                   choice_value: "ad", values: {directory_type: "ad", nested_groups: "yes"}}]
    })}];
    await openExtension();
    expect(screen.getByText("Directory profile")).toBeInTheDocument();
    expect(document.querySelector("textarea").value).toBe("(uid={username})");
    expect([...document.querySelectorAll(".extensionRow label")]
        .some(label => label.textContent.startsWith("LDAP host"))).toBe(false);
    fireEvent.change(controlOf("Directory type"), {target: {value: "ad"}});
    expect(controlOf("LDAP host").value).toBe("dc1.example.org");
    expect(controlOf("Nested groups").value).toBe("yes");
    fireEvent.change(controlOf("Directory type"), {target: {value: "ldap"}});
    fireEvent.click(screen.getByRole("button", {name: "Apply"}));
    await waitFor(() => expect(ControlAPI.asyncMakeAPICall).toHaveBeenCalledWith("ExtensionControl",
        expect.objectContaining({action: "set", settings: expect.arrayContaining([
            {name: "host", value: "dc1.example.org"}
        ])})));
});

test("a broken UI sidecar leaves the ordinary form available with a warning", async () => {
    listed = [{...extension, ui_definition: '{"version":99,"sections":[]}'}];
    await openExtension();
    expect(screen.getByRole("alert")).toHaveTextContent("Unsupported UI description version");
    expect(controlOf("LDAP host").value).toBe("dc1.example.org");
});

test("a disabled field retains edits and is submitted rather than silently cleared", async () => {
    listed = [{...extension, ui_definition: JSON.stringify({version: 1,
        sections: [{title: "Connection", fields: [
            {name: "directory_type"},
            {name: "host", enabled_when: {setting: "directory_type", values: ["ad"]}}
        ]}]
    })}];
    await openExtension();
    expect(controlOf("LDAP host")).toBeDisabled();
    fireEvent.change(controlOf("Directory type"), {target: {value: "ad"}});
    expect(controlOf("LDAP host")).not.toBeDisabled();
    fireEvent.change(controlOf("LDAP host"), {target: {value: "dc2.example.org"}});
    fireEvent.change(controlOf("Directory type"), {target: {value: "ldap"}});
    expect(controlOf("LDAP host")).toBeDisabled();
    expect(controlOf("LDAP host").value).toBe("dc2.example.org");
    fireEvent.click(screen.getByRole("button", {name: "Apply"}));
    await waitFor(() => expect(ControlAPI.asyncMakeAPICall).toHaveBeenCalledWith("ExtensionControl",
        expect.objectContaining({action: "set", settings: expect.arrayContaining([
            {name: "host", value: "dc2.example.org"}
        ])})));
});

test("a preset fills the form and leaves the fields it does not name alone", async () => {
    await openExtension();

    expect(screen.getByLabelText("Set defaults")).toBeInTheDocument();
    chosenPreset();

    expect(controlOf("Directory type").value).toBe("ad");
    expect(controlOf("LDAP port").value).toBe("636");
    expect(controlOf("LDAP encryption").value).toBe("ldaps");
    expect(controlOf("LDAP host").value).toBe("dc1.example.org");
    // Cleared rather than left at the uid filter, which would silently override the AD profile.
    expect(controlOf("User search filter").value).toBe("");

    // An operator has to be able to tell a fill that changed three fields from a button that did
    // nothing: both leave the form looking almost the same.
    expect(screen.getByText(/Filled: Directory type, LDAP port, Nested groups, User search filter/))
        .toBeInTheDocument();
    expect(screen.getByText(/Already at these values: LDAP encryption/)).toBeInTheDocument();
});

test("choosing a preset applies nothing: only Apply writes, and it writes what is shown", async () => {
    await openExtension();
    chosenPreset();

    expect(ControlAPI.asyncMakeAPICall.mock.calls
        .filter(([, request]) => request && request.action === "set")).toHaveLength(0);

    fireEvent.click(screen.getByRole("button", {name: "Apply"}));

    await waitFor(() => {
        const call = ControlAPI.asyncMakeAPICall.mock.calls
            .find(([, request]) => request && request.action === "set");
        expect(call).toBeTruthy();
        const settings = Object.fromEntries(call[1].settings.map(one => [one.name, one.value]));
        expect(settings.directory_type).toBe("ad");
        expect(settings.port).toBe("636");
        expect(settings.user_filter).toBe("");
        expect(settings.host).toBe("dc1.example.org");
        // The undeclared name in the preset never reaches the broker.
        expect(Object.keys(settings).sort())
            .toEqual(["directory_type", "encryption", "host", "nested_groups", "port",
                      "user_filter"]);
    });
});

test("choosing a directory type brings the profile that belongs with it", async () => {
    await openExtension();

    // No Preset button: selecting the type is itself the statement of which profile applies.
    fireEvent.change(controlOf("Directory type"), {target: {value: "ad"}});

    expect(controlOf("Directory type").value).toBe("ad");
    expect(controlOf("Nested groups").value).toBe("yes");
    expect(controlOf("LDAP host").value).toBe("dc1.example.org");
    expect(screen.getByText(/Filled: Directory type, LDAP port, Nested groups/))
        .toBeInTheDocument();
});

test("an extension without presets gets no starting-point control", async () => {
    listed = [{...extension, presets: []}];
    await openExtension();
    expect(screen.queryByLabelText("Set defaults")).not.toBeInTheDocument();
});
