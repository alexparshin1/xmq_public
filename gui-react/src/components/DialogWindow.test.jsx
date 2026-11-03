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
    expect(screen.getByText("Remove")).toBeDisabled();

    // The add buttons open a dialog rather than adding an empty row: what is added has to be a
    // name, and that dialog is where the name is chosen.
    fireEvent.click(screen.getByText("Add Group"));
    fireEvent.change(screen.getByPlaceholderText("Group name"), {target: {value: "water"}});
    // The outer dialog has Ok too; the picker is the first of the two in the document.
    fireEvent.click(screen.getAllByText("Ok")[0]);
    fireEvent.click(screen.getByText("Add User"));
    fireEvent.change(screen.getByPlaceholderText("Username"), {target: {value: "alice"}});
    // The outer dialog has Ok too; the picker is the first of the two in the document.
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

test("A group typed with the group: prefix is stored under its bare name", () => {
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
    fireEvent.change(screen.getByPlaceholderText("Group name"),
        {target: {value: "group:Administrators"}});
    // The outer dialog has Ok too; the picker is the first of the two in the document.
    fireEvent.click(screen.getAllByText("Ok")[0]);
    expect(screen.getByText("group:Administrators")).toBeInTheDocument();
});

test("A name that does not exist is refused rather than stored", () => {
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
    fireEvent.change(screen.getByPlaceholderText("Group name"), {target: {value: "missing"}});
    // The outer dialog has Ok too; the picker is the first of the two in the document.
    fireEvent.click(screen.getAllByText("Ok")[0]);
    // The dialog stays open with the reason, and nothing was added to the rule.
    expect(screen.getByText("There is no group named 'missing'.")).toBeInTheDocument();
    expect(screen.getByPlaceholderText("Group name")).toBeInTheDocument();
    expect(screen.getByText("No users or groups yet.")).toBeInTheDocument();

    fireEvent.click(screen.getAllByText("Cancel")[0]);
    expect(screen.queryByPlaceholderText("Group name")).toBeNull();

    fireEvent.click(screen.getByText("Add User"));
    fireEvent.change(screen.getByPlaceholderText("Username"), {target: {value: "bob"}});
    // The outer dialog has Ok too; the picker is the first of the two in the document.
    fireEvent.click(screen.getAllByText("Ok")[0]);
    expect(screen.getByText("There is no account named 'bob'.")).toBeInTheDocument();

    fireEvent.change(screen.getByPlaceholderText("Username"), {target: {value: "alice"}});
    // The outer dialog has Ok too; the picker is the first of the two in the document.
    fireEvent.click(screen.getAllByText("Ok")[0]);
    expect(screen.getByText("user:alice")).toBeInTheDocument();
});
