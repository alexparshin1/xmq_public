import React from "react";
import {fireEvent, render, screen, within} from "@testing-library/react";
import {MemoryRouter} from "react-router-dom";
import App from "./App";
import ControlAPI from "./ControlAPI";
import {documentationPages, documentationPagesIn} from "./documentation/pages";

beforeEach(() => {
    ControlAPI.token = "";
});

test.each(documentationPages)("Documentation $label is available without signing in", page => {
    render(<MemoryRouter initialEntries={[page.path]}><App/></MemoryRouter>);
    expect(screen.getByRole("heading", {level: 1, name: page.title})).toBeInTheDocument();
    const group = page.menu || "Documentation";
    // The extension documentation moved into its own group when the configuration pages took
    // the Extensions name; page.menu still names the set of pages, not the menu heading.
    const heading = group === "Extensions" ? "Extension Docs" : group;
    const menu = screen.getByRole("button", {name: heading}).parentElement;
    for (const entry of documentationPagesIn(group)) {
        expect(within(menu).getByRole("link", {name: entry.label})).toHaveAttribute("href", entry.path);
    }
});

test("the Permissions page opens with its rules and tabs", async () => {
    ControlAPI.token = "a session";
    jest.spyOn(ControlAPI, "asyncMakeAPICall").mockImplementation((service, request) => {
        if (service === "ServerControl") {
            return Promise.resolve({running: true});
        }
        if (service === "ExtensionControl" && request.action === "list") {
            return Promise.resolve({result: {success: true}, list: []});
        }
        if (service === "UserGroupControl" || service === "UserControl") {
            return Promise.resolve({result: {success: true}, list: []});
        }
        if (service === "PermissionsControl" && request.action === "list") {
            return Promise.resolve({result: {success: true}, rules: [], default_deny: false});
        }
        return Promise.resolve({result: {success: true}});
    });

    render(<MemoryRouter initialEntries={["/permissions"]}><App/></MemoryRouter>);
    expect(await screen.findByRole("tab", {name: "Rules"})).toBeInTheDocument();
    expect(screen.getByText(/Each row is one user/)).toBeInTheDocument();
    jest.restoreAllMocks();
});

test("the documentation menu opens the shared Cluster page", () => {
    render(<MemoryRouter initialEntries={["/xmq_documentation"]}><App/></MemoryRouter>);
    fireEvent.click(screen.getByRole("link", {name: "Cluster", exact: true}));
    expect(screen.getByRole("heading", {level: 1, name: "Setting up a Cluster"})).toBeInTheDocument();
    expect(screen.getByText(/Before step 1, create the shared User Database/)).toBeInTheDocument();
    expect(screen.queryByText(/Give every node the same cluster password/)).not.toBeInTheDocument();
});

test.each([
    ["/manual", "XMQ MQTT Server Documentation"],
    ["/manual#install", "Download and Install"],
    ["/manual#cluster-setup", "Setting up a Cluster"]
])("legacy manual link %s reaches its documentation page", (url, heading) => {
    render(<MemoryRouter initialEntries={[url]}><App/></MemoryRouter>);
    expect(screen.getByRole("heading", {level: 1, name: heading})).toBeInTheDocument();
});
