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
