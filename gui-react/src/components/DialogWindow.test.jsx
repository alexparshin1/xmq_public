import React from "react";
import {fireEvent, render, screen} from "@testing-library/react";
import DialogWindow from "./DialogWindow";

test("A combo box offers the known values and still accepts a typed one", () => {
    const {container} = render(<DialogWindow
        windowTitle="Add New Rule"
        fields={[{
            headerName: "Group", field: "group",
            context: {inputType: "combobox", options: ["operators", "water"]}
        }]}
        values={{}}
        onSubmit={() => {}}
        closeModal={() => {}}/>);

    // An input with a list, not a select: the values are suggestions, and `user:<name>` - which is
    // not among them - has to remain typeable.
    const input = screen.getByRole("combobox");
    expect(input.tagName).toBe("INPUT");
    expect(input).toHaveAttribute("list", "group-options");
    const options = Array.from(container.querySelectorAll("#group-options option"))
        .map(option => option.getAttribute("value"));
    expect(options).toEqual(["operators", "water"]);

    fireEvent.change(input, {target: {value: "user:alice"}});
    expect(input).toHaveValue("user:alice");
});

test("The subject list adds a group and a user and removes them by name", () => {
    render(<DialogWindow
        windowTitle="Add New Rule"
        fields={[{
            headerName: "Applies to", field: "group",
            context: {inputType: "subjects", options: {groups: ["operators", "water"], users: ["alice"]}}
        }]}
        values={{}}
        onSubmit={() => {}}
        closeModal={() => {}}/>);

    expect(screen.getByText("No users or groups yet.")).toBeInTheDocument();

    fireEvent.change(screen.getByPlaceholderText("Group name"), {target: {value: "water"}});
    fireEvent.click(screen.getByText("Add Group"));
    fireEvent.change(screen.getByPlaceholderText("Username"), {target: {value: "alice"}});
    fireEvent.click(screen.getByText("Add User"));

    expect(screen.getByText("water")).toBeInTheDocument();
    expect(screen.getByText("user:alice")).toBeInTheDocument();

    // Remove is per entry, so one subject can go without disturbing the others.
    const water = screen.getByText("water");
    fireEvent.click(water.parentElement.querySelector("button"));
    expect(screen.queryByText("water")).toBeNull();
    expect(screen.getByText("user:alice")).toBeInTheDocument();
});

test("A group typed with the group: prefix is stored under its bare name", () => {
    render(<DialogWindow
        windowTitle="Add New Rule"
        fields={[{
            headerName: "Applies to", field: "group",
            context: {inputType: "subjects", options: {groups: [], users: []}}
        }]}
        values={{}}
        onSubmit={() => {}}
        closeModal={() => {}}/>);

    fireEvent.change(screen.getByPlaceholderText("Group name"),
        {target: {value: "group:Administrators"}});
    fireEvent.click(screen.getByText("Add Group"));
    expect(screen.getByText("Administrators")).toBeInTheDocument();
});
