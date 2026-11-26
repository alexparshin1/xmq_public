import React from "react";
import {fireEvent, render, screen} from "@testing-library/react";
import {MemoryRouter, useLocation} from "react-router-dom";
import MainMenu from "./MainMenu";
import ControlAPI from "../ControlAPI";

function LocationProbe() {
    const location = useLocation();
    return <div data-testid="path">{location.pathname}</div>;
}

function showMenu(path = "/") {
    ControlAPI.token = "a session";
    return render(<MemoryRouter initialEntries={[path]}>
        <MainMenu/>
        <LocationProbe/>
    </MemoryRouter>);
}

const path = () => screen.getByTestId("path").textContent;
const groupHeader = (title) => screen.getByRole("button", {name: title});

beforeEach(() => {
    // The menu asks the broker for its extension groups on connect. Tests that are about the menu's
    // own behaviour answer that call with "nothing reported", so the familiar sections remain.
    jest.spyOn(ControlAPI, "asyncMakeAPICall").mockImplementation(() => Promise.resolve(null));
});

afterEach(() => {
    ControlAPI.token = "";
    jest.restoreAllMocks();
});

test("only the group holding the current page is open", () => {
    showMenu("/dashboard");
    expect(screen.getByRole("link", {name: "Dashboard"})).toBeInTheDocument();
    expect(screen.getByRole("link", {name: "Sessions"})).toBeInTheDocument();
    expect(groupHeader("Operation")).toHaveAttribute("aria-expanded", "true");
    // The configuration pages are a long list; having them on screen while the dashboard is open
    // is exactly the bulk this menu avoids.
    expect(groupHeader("Configuration")).toHaveAttribute("aria-expanded", "false");
    expect(screen.queryByRole("link", {name: "Settings"})).not.toBeInTheDocument();
});

test("opening a group closes the one that was open", () => {
    showMenu("/dashboard");
    fireEvent.click(groupHeader("Configuration"));
    expect(groupHeader("Configuration")).toHaveAttribute("aria-expanded", "true");
    expect(groupHeader("Operation")).toHaveAttribute("aria-expanded", "false");
    expect(screen.queryByRole("link", {name: "Dashboard"})).not.toBeInTheDocument();
    expect(screen.getByRole("link", {name: "Initial Setup"})).toBeInTheDocument();
});

test("a group reopens on the page last used in it", () => {
    showMenu("/settings");
    fireEvent.click(groupHeader("User Management"));
    fireEvent.click(screen.getByRole("link", {name: "Users"}));
    expect(path()).toBe("/users");
    fireEvent.click(groupHeader("Operation"));
    expect(path()).toBe("/dashboard");
    fireEvent.click(groupHeader("User Management"));
    expect(path()).toBe("/users");
});

test("user management and cluster pages are grouped under their own headers", () => {
    showMenu("/users");
    expect(groupHeader("User Management")).toHaveAttribute("aria-expanded", "true");
    expect(screen.getByRole("link", {name: "Users"})).toBeInTheDocument();
    expect(screen.getByRole("link", {name: "Groups"})).toBeInTheDocument();
    expect(screen.getByRole("link", {name: "Imports"})).toHaveAttribute("href", "/import");
    expect(screen.getByRole("link", {name: "Permissions"})).toHaveAttribute("href", "/permissions");

    fireEvent.click(groupHeader("Cluster"));
    expect(screen.getByRole("link", {name: "Bridges"})).toBeInTheDocument();
    expect(screen.getByRole("link", {name: "Cluster"})).toBeInTheDocument();
    expect(screen.queryByRole("link", {name: "Users"})).not.toBeInTheDocument();
});

test("a URL opened directly expands the group that holds the page", () => {
    showMenu("/persistence");
    expect(groupHeader("Configuration")).toHaveAttribute("aria-expanded", "true");
    const item = screen.getByRole("link", {name: "Persistence"}).parentElement;
    expect(item).toHaveClass("AccordionItemSelected");
});

test("extension groups reported by the broker become menu sections", async () => {
    ControlAPI.asyncMakeAPICall.mockImplementation((service, request) => {
        if (service === "ExtensionControl" && request.action === "list") {
            return Promise.resolve({result: {success: true}, list: [
                {name: "acl", group: "Access Control", page: "permissions"},
                {name: "webhook", group: "Exports", page: "data-integrations"},
                {name: "topic-guard", group: "Permissions", page: "permissions"}
            ]});
        }
        return Promise.resolve(null);
    });

    showMenu("/settings");
    fireEvent.click(groupHeader("Extensions"));
    expect(await screen.findByRole("link", {name: "Exports"}))
        .toHaveAttribute("href", "/configurations/exports");
    expect(screen.getByRole("link", {name: "Access Control"}))
        .toHaveAttribute("href", "/configurations/access-control");
    // Permissions belongs to User Management, so even a group with that name does not put a
    // second link here.
    expect(screen.queryByRole("link", {name: "Permissions", exact: true})).not.toBeInTheDocument();
});

test("Permissions stays in the menu when no extension reports its group", async () => {
    ControlAPI.asyncMakeAPICall.mockImplementation((service, request) => {
        if (service === "ExtensionControl" && request.action === "list") {
            return Promise.resolve({result: {success: true}, list: [
                {name: "webhook", group: "Data Integrations", page: "data-integrations"},
                {name: "ldap", group: "Authentication", page: "authentication"}
            ]});
        }
        return Promise.resolve(null);
    });

    showMenu("/settings");
    fireEvent.click(groupHeader("User Management"));
    expect(await screen.findByRole("link", {name: "Permissions"}))
        .toHaveAttribute("href", "/permissions");
});

test("signed out, the menu offers signing in, the documentation, and nothing else", () => {
    ControlAPI.token = "";
    render(<MemoryRouter><MainMenu/></MemoryRouter>);
    expect(screen.getByRole("link", {name: "Login"})).toBeInTheDocument();
    expect(groupHeader("System")).toHaveAttribute("aria-expanded", "true");
    expect(screen.queryByRole("button", {name: "Configuration"})).not.toBeInTheDocument();
    expect(groupHeader("Documentation")).toHaveAttribute("aria-expanded", "false");
});
