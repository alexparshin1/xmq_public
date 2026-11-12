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

afterEach(() => {
    ControlAPI.token = "";
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
    expect(screen.getByRole("link", {name: "Permissions"})).toBeInTheDocument();
    expect(screen.getByRole("link", {name: "Imports"})).toHaveAttribute("href", "/import");

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

test("signed out, the menu offers signing in, the documentation, and nothing else", () => {
    ControlAPI.token = "";
    render(<MemoryRouter><MainMenu/></MemoryRouter>);
    expect(screen.getByRole("link", {name: "Login"})).toBeInTheDocument();
    expect(groupHeader("System")).toHaveAttribute("aria-expanded", "true");
    expect(screen.queryByRole("button", {name: "Configuration"})).not.toBeInTheDocument();
    expect(groupHeader("Documentation")).toHaveAttribute("aria-expanded", "false");
});
