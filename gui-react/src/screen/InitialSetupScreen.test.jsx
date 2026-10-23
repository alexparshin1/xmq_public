import React from "react";
import {fireEvent, render, screen, waitFor} from "@testing-library/react";
import InitialSetupScreen from "./InitialSetupScreen";
import ControlAPI from "../ControlAPI";
import {errorWindow} from "../components/MessageWindow";

jest.mock("../components/HelpButton", () => () => null);
jest.mock("../components/MessageWindow", () => ({
    errorWindow: jest.fn(),
    confirmationWindow: jest.fn()
}));

const defaults = {
    node_name: "primary",
    node_host: "thinker10",
    mqtt_port: 1883,
    mqtt_ssl_port: 8883,
    web_service_port: 18883,
    redis_port: 6379
};

beforeEach(() => {
    ControlAPI.setupRequired = true;
    jest.spyOn(ControlAPI, "asyncMakeAPICall").mockImplementation((method, body) =>
        Promise.resolve(body.action === "get" ? {result: {success: true}, setup: defaults} : null));
});

afterEach(() => {
    jest.restoreAllMocks();
    jest.clearAllMocks();
    ControlAPI.setupRequired = false;
});

async function form() {
    const view = render(<InitialSetupScreen/>);
    await screen.findByDisplayValue("thinker10");
    for (const name of ["admin_password", "admin_password_confirmation", "cluster_password", "cluster_password_confirmation"]) {
        fireEvent.change(view.container.querySelector(`input[name="${name}"]`), {
            target: {value: "Sunflower7!"}
        });
    }
    return view;
}

test("local SQLite keeps the default database name and sends no shared URI", async () => {
    await form();
    expect(screen.getByLabelText("Database name")).toHaveValue("xmq");
    expect(screen.getByLabelText("Database hostname")).toBeDisabled();
    fireEvent.click(screen.getByText("Submit and restart"));
    await waitFor(() => expect(ControlAPI.asyncMakeAPICall).toHaveBeenCalledWith("InitialSetupControl", {
        action: "set", setup: expect.objectContaining({database_uri: "", cluster_password: "Sunflower7!"})
    }));
});

test.each(["", "Different-Secret1"])("refuses an empty or mismatched cluster password: %s", async (password) => {
    await form();
    fireEvent.change(screen.getByLabelText("Cluster password:"), {target: {value: password}});
    fireEvent.click(screen.getByText("Submit and restart"));
    expect(errorWindow).toHaveBeenCalledWith(expect.stringMatching(/cluster password/));
    expect(ControlAPI.asyncMakeAPICall).not.toHaveBeenCalledWith("InitialSetupControl",
        expect.objectContaining({action: "set"}));
});

test.each(["postgresql", "mysql"])("builds the %s URI with escaped credentials", async (type) => {
    const view = await form();
    fireEvent.change(screen.getByLabelText("Database server type"), {target: {value: type}});
    fireEvent.change(screen.getByLabelText("Database hostname"), {target: {value: "theater:5433"}});
    fireEvent.change(screen.getByLabelText("Database user:"), {target: {value: "xmq@team"}});
    fireEvent.change(screen.getByLabelText("Database password:"), {target: {value: "s:e/cr@t"}});
    fireEvent.change(view.container.querySelector('input[name="redis_host"]'), {target: {value: "theater"}});
    fireEvent.click(screen.getByText("Submit and restart"));
    await waitFor(() => expect(ControlAPI.asyncMakeAPICall).toHaveBeenCalledWith("InitialSetupControl", {
        action: "set", setup: expect.objectContaining({
            database_uri: `${type}://xmq%40team:s%3Ae%2Fcr%40t@theater:5433/xmq`
        })
    }));
});

test("a shared user database requires Redis before submitting", async () => {
    await form();
    fireEvent.change(screen.getByLabelText("Database server type"), {target: {value: "postgresql"}});
    fireEvent.change(screen.getByLabelText("Database hostname"), {target: {value: "theater"}});
    fireEvent.click(screen.getByText("Submit and restart"));
    expect(errorWindow).toHaveBeenCalledWith(expect.stringContaining("Provide the Redis host"));
    expect(ControlAPI.asyncMakeAPICall).not.toHaveBeenCalledWith("InitialSetupControl",
        expect.objectContaining({action: "set"}));
});

test("splits a configured template URI into the connection fields", async () => {
    ControlAPI.asyncMakeAPICall.mockResolvedValue({result: {success: true}, setup: {
        ...defaults, database_uri: "postgresql://xmq:s%40cret@theater:5433/accounts"
    }});
    await form();
    expect(screen.getByLabelText("Database server type")).toHaveValue("postgresql");
    expect(screen.getByLabelText("Database hostname")).toHaveValue("theater:5433");
    expect(screen.getByLabelText("Database name")).toHaveValue("accounts");
    expect(screen.getByLabelText("Database user:")).toHaveValue("xmq");
    expect(screen.getByLabelText("Database password:")).toHaveValue("s@cret");
});
