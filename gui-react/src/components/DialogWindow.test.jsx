import React from "react";
import {fireEvent, render, screen} from "@testing-library/react";
import DialogWindow from "./DialogWindow";

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
    expect(screen.getByText("Remove")).toBeDisabled();

    // The add buttons open a dialog whose combo box contains only names that exist: a name that
    // does not cannot be entered, which is the point of not using a free text field.
    fireEvent.click(screen.getByText("Add Group"));
    fireEvent.change(screen.getByRole("combobox"), {target: {value: "water"}});
    // The outer dialog has Ok too; the picker is the first of the two in the document.
    fireEvent.click(screen.getAllByText("Ok")[0]);
    fireEvent.click(screen.getByText("Add User"));
    fireEvent.change(screen.getByRole("combobox"), {target: {value: "user:alice"}});
    fireEvent.click(screen.getAllByText("Ok")[0]);

    // A group is shown with a prefix so it cannot be mistaken for a username.
    expect(screen.getByText("group:water")).toBeInTheDocument();
    expect(screen.getByText("user:alice")).toBeInTheDocument();

    // Remove acts on the entry selected in the list, so one subject can go without disturbing the
    // others.
    fireEvent.click(screen.getByText("group:water"));
    fireEvent.click(screen.getByText("Remove"));
    expect(screen.queryByText("group:water")).toBeNull();
    expect(screen.getByText("user:alice")).toBeInTheDocument();
});

test("The combo box stores the bare group name and shows it with its prefix", () => {
    render(<DialogWindow
        windowTitle="Add New Rule"
        fields={[{
            headerName: "Applies to", field: "group",
            context: {inputType: "subjects", options: {groups: ["Administrators"], users: []}}
        }]}
        values={{}}
        onSubmit={() => {}}
        closeModal={() => {}}/>);

    fireEvent.click(screen.getByText("Add Group"));
    fireEvent.change(screen.getByRole("combobox"), {target: {value: "Administrators"}});
    fireEvent.click(screen.getAllByText("Ok")[0]);
    expect(screen.getByText("group:Administrators")).toBeInTheDocument();
});

test("A name that does not exist is not offered at all", () => {
    render(<DialogWindow
        windowTitle="Add New Rule"
        fields={[{
            headerName: "Applies to", field: "group",
            context: {inputType: "subjects", options: {groups: ["operators"], users: ["alice"]}}
        }]}
        values={{}}
        onSubmit={() => {}}
        closeModal={() => {}}/>);

    fireEvent.click(screen.getByText("Add Group"));
    const groups = screen.getByRole("combobox");
    // Only the placeholder and the names that exist: "missing" has nothing to select.
    expect(Array.from(groups.options).map(option => option.value)).toEqual(["", "operators"]);
    expect(screen.queryByText("group:missing")).toBeNull();
    // Ok cannot be pressed before a name has been chosen.
    expect(screen.getAllByText("Ok")[0]).toBeDisabled();

    fireEvent.change(groups, {target: {value: "operators"}});
    fireEvent.click(screen.getAllByText("Ok")[0]);
    expect(screen.getByText("group:operators")).toBeInTheDocument();

    fireEvent.click(screen.getByText("Add User"));
    const users = screen.getByRole("combobox");
    expect(Array.from(users.options).map(option => option.value)).toEqual(["", "user:alice"]);
    fireEvent.change(users, {target: {value: "user:alice"}});
    fireEvent.click(screen.getAllByText("Ok")[0]);
    expect(screen.getByText("user:alice")).toBeInTheDocument();
});
