import React from "react";
import {fireEvent, render, screen, within} from "@testing-library/react";
import {MemoryRouter} from "react-router-dom";
import App from "./App";
import ControlAPI from "./ControlAPI";
import {documentationPages} from "./documentation/pages";

beforeEach(() => {
    ControlAPI.token = "";
});

test.each(documentationPages)("Documentation $label is available without signing in", page => {
    render(<MemoryRouter initialEntries={[page.path]}><App/></MemoryRouter>);
    expect(screen.getByRole("heading", {level: 1, name: page.title})).toBeInTheDocument();
    const menu = screen.getByText("Documentation").parentElement;
    for (const entry of documentationPages) {
        expect(within(menu).getByRole("link", {name: entry.label})).toHaveAttribute("href", entry.path);
    }
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
