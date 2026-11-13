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
        {name: "user_filter", label: "User search filter", type: "string",
            value: "(uid={username})", declared: true}
    ],
    presets: [{
        name: "ad",
        label: "Active Directory",
        description: "Active Directory: login by sAMAccountName or user principal name.",
        settings: [
            {name: "directory_type", value: "ad"},
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
    const row = screen.getByText(label, {exact: false}).closest(".extensionRow");
    return row.querySelector("select, input");
};

const chosenPreset = () => {
    fireEvent.change(screen.getByLabelText("Set defaults"), {target: {value: "ad"}});
    fireEvent.click(screen.getByRole("button", {name: "Preset"}));
};

test("a preset fills the form and leaves the fields it does not name alone", async () => {
    await openExtension();

    expect(screen.getByLabelText("Set defaults")).toBeInTheDocument();
    chosenPreset();

    expect(controlOf("Directory type").value).toBe("ad");
    expect(controlOf("LDAP host").value).toBe("dc1.example.org");
    // Cleared rather than left at the uid filter, which would silently override the AD profile.
    expect(controlOf("User search filter").value).toBe("");
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
        expect(settings.user_filter).toBe("");
        expect(settings.host).toBe("dc1.example.org");
        // The undeclared name in the preset never reaches the broker.
        expect(Object.keys(settings).sort()).toEqual(["directory_type", "host", "user_filter"]);
    });
});

test("an extension without presets gets no starting-point control", async () => {
    listed = [{...extension, presets: []}];
    await openExtension();
    expect(screen.queryByLabelText("Set defaults")).not.toBeInTheDocument();
});
