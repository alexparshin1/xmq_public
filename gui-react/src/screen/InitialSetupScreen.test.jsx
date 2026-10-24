import React from "react";
import {act, fireEvent, render, screen, waitFor} from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import InitialSetupScreen from "./InitialSetupScreen";
import ControlAPI from "../ControlAPI";
import {errorWindow} from "../components/MessageWindow";

jest.mock("../components/HelpButton", () => () => null);
jest.mock("../components/MessageWindow", () => ({
    errorWindow: jest.fn(),
    confirmationWindow: jest.fn()
}));

const defaults = {
    node_name: "thinker10",
    node_host: "thinker10",
    mqtt_port: 1883,
    mqtt_ssl_port: 8883,
    web_service_port: 18883,
    redis_port: 6379,
    redis_database: 0
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
    await waitFor(() => expect(screen.getByLabelText("Node name:")).toHaveValue("thinker10"));
    for (const name of ["admin_password", "admin_password_confirmation", "cluster_password", "cluster_password_confirmation"]) {
        fireEvent.change(view.container.querySelector(`input[name="${name}"]`), {
            target: {value: "Sunflower7!"}
        });
    }
    return view;
}

test.each(["admin_password", "admin_password_confirmation", "cluster_password", "cluster_password_confirmation"])(
    "%s can reveal and hide its value without submitting the form", async name => {
        const view = await form();
        const input = view.container.querySelector(`input[name="${name}"]`);
        const toggle = input.closest(".passwordInput").querySelector("button");
        expect(input).toHaveAttribute("type", "password");
        fireEvent.click(toggle);
        expect(input).toHaveAttribute("type", "text");
        expect(input).toHaveValue("Sunflower7!");
        fireEvent.click(toggle);
        expect(input).toHaveAttribute("type", "password");
        expect(ControlAPI.asyncMakeAPICall).not.toHaveBeenCalledWith("InitialSetupControl",
            expect.objectContaining({action: "set"}));
    });

test("Node name uses the hostname supplied by the server", async () => {
    await form();
    expect(screen.getByLabelText("Node name:")).toHaveValue("thinker10");
});

test("Enter in an input activates Submit and restart once", async () => {
    await form();
    act(() => userEvent.type(screen.getByLabelText("Repeat cluster password:"), "{enter}"));
    await waitFor(() => expect(ControlAPI.asyncMakeAPICall).toHaveBeenCalledWith("InitialSetupControl",
        expect.objectContaining({action: "set"})));
    expect(ControlAPI.asyncMakeAPICall.mock.calls.filter(([, body]) => body.action === "set")).toHaveLength(1);
});

test("Enter runs validation and repeated submit is ignored while busy", async () => {
    await form();
    const repeat = screen.getByLabelText("Repeat password:");
    fireEvent.change(repeat, {target: {value: "Mismatch7!"}});
    act(() => userEvent.type(repeat, "{enter}"));
    expect(errorWindow).toHaveBeenCalledWith(expect.stringContaining("administrator passwords do not match"));
    expect(ControlAPI.asyncMakeAPICall.mock.calls.filter(([, body]) => body.action === "set")).toHaveLength(0);
    fireEvent.change(repeat, {target: {value: "Sunflower7!"}});
    ControlAPI.asyncMakeAPICall.mockImplementation(() => new Promise(() => {}));
    act(() => userEvent.type(repeat, "{enter}"));
    expect(screen.getByRole("button", {name: /Checking connections/})).toBeDisabled();
    fireEvent.submit(repeat.closest("form"));
    expect(ControlAPI.asyncMakeAPICall.mock.calls.filter(([, body]) => body.action === "set")).toHaveLength(1);
});

test.each(["redis_host", "redis_port"])("leaving %s checks the current host and port on the server", async field => {
    const view = await form();
    ControlAPI.asyncMakeAPICall.mockResolvedValue({result: {success: true}});
    fireEvent.change(screen.getByLabelText("Redis host:"), {target: {value: "theater"}});
    fireEvent.change(screen.getByLabelText("port:"), {target: {value: "6380"}});
    fireEvent.blur(view.container.querySelector(`input[name="${field}"]`));
    await waitFor(() => expect(screen.getByRole("status")).toHaveTextContent("Redis at theater:6380 is reachable."));
    expect(ControlAPI.asyncMakeAPICall).toHaveBeenCalledWith("InitialSetupControl", {
        action: "check-redis", redis_host: "theater", redis_port: 6380, redis_database: 0, redis_username: "", redis_password: ""
    }, true);
});

test("a failed Redis check is shown beside the fields and old replies are ignored", async () => {
    await form();
    let resolve;
    ControlAPI.asyncMakeAPICall.mockImplementation(() => new Promise(done => { resolve = done; }));
    const host = screen.getByLabelText("Redis host:");
    fireEvent.change(host, {target: {value: "old-host"}});
    fireEvent.blur(host);
    fireEvent.change(host, {target: {value: "new-host"}});
    await act(async () => resolve({result: {success: true}}));
    expect(screen.queryByRole("status")).not.toBeInTheDocument();
    ControlAPI.asyncMakeAPICall.mockResolvedValue({result: {success: false, description: "Redis cannot be reached"}});
    fireEvent.blur(host);
    await waitFor(() => expect(screen.getByRole("alert")).toHaveTextContent("Redis cannot be reached"));
});

test("empty host skips the Redis check and an invalid port is rejected locally", async () => {
    await form();
    ControlAPI.asyncMakeAPICall.mockClear();
    fireEvent.blur(screen.getByLabelText("Redis host:"));
    expect(ControlAPI.asyncMakeAPICall).not.toHaveBeenCalled();
    fireEvent.change(screen.getByLabelText("Redis host:"), {target: {value: "theater"}});
    fireEvent.change(screen.getByLabelText("port:"), {target: {value: "70000"}});
    fireEvent.blur(screen.getByLabelText("port:"));
    expect(screen.getByRole("alert")).toHaveTextContent("between 1 and 65535");
    expect(ControlAPI.asyncMakeAPICall).not.toHaveBeenCalled();
});

test("Redis credentials are used for connection checks and included in setup", async () => {
    await form();
    ControlAPI.asyncMakeAPICall.mockResolvedValue({result: {success: true}});
    fireEvent.change(screen.getByLabelText("Redis host:"), {target: {value: "theater"}});
    fireEvent.change(screen.getByLabelText("Redis username:"), {target: {value: "cache-user"}});
    fireEvent.change(screen.getByLabelText("Redis password:"), {target: {value: "Redis-Secret7!"}});
    fireEvent.blur(screen.getByLabelText("Redis password:"));
    await waitFor(() => expect(ControlAPI.asyncMakeAPICall).toHaveBeenCalledWith("InitialSetupControl", {
        action: "check-redis", redis_host: "theater", redis_port: 6379, redis_database: 0, redis_username: "cache-user", redis_password: "Redis-Secret7!"
    }, true));
    ControlAPI.asyncMakeAPICall.mockResolvedValue(null);
    fireEvent.click(screen.getByRole("button", {name: "Submit and restart"}));
    await waitFor(() => expect(ControlAPI.asyncMakeAPICall).toHaveBeenCalledWith("InitialSetupControl", {
        action: "set", setup: expect.objectContaining({redis_username: "cache-user", redis_password: "Redis-Secret7!"})
    }));
});

test.each(["admin", "cluster"])("%s confirmation checks on blur and clears when corrected", async account => {
    const view = await form();
    const repeat = view.container.querySelector(`input[name="${account}_password_confirmation"]`);
    const password = view.container.querySelector(`input[name="${account}_password"]`);
    fireEvent.change(repeat, {target: {value: "Different7!"}});
    expect(screen.queryByRole("alert")).not.toBeInTheDocument();
    fireEvent.blur(repeat);
    expect(screen.getByRole("alert")).toHaveTextContent(/passwords do not match/);
    expect(repeat).toHaveAttribute("aria-invalid", "true");
    fireEvent.change(repeat, {target: {value: "Sunflower7!"}});
    expect(screen.queryByRole("alert")).not.toBeInTheDocument();
    expect(repeat).toHaveAttribute("aria-invalid", "false");
    fireEvent.change(password, {target: {value: "Changed7!"}});
    expect(screen.getByRole("alert")).toHaveTextContent(/passwords do not match/);
    fireEvent.change(repeat, {target: {value: "Changed7!"}});
    fireEvent.blur(repeat);
    expect(screen.queryByRole("alert")).not.toBeInTheDocument();
});

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

test.each(["postgresql", "mysql"])("%s connection failure keeps the setup form editable", async type => {
    await form();
    fireEvent.change(screen.getByLabelText("Database server type"), {target: {value: type}});
    fireEvent.change(screen.getByLabelText("Database hostname"), {target: {value: "db-host:5433"}});
    fireEvent.change(screen.getByLabelText("Database password:"), {target: {value: "Db-Secret7!"}});
    fireEvent.change(screen.getByLabelText("Redis host:"), {target: {value: "theater"}});
    let complete;
    ControlAPI.asyncMakeAPICall.mockImplementation(() => new Promise(resolve => { complete = resolve; }));
    fireEvent.click(screen.getByText("Submit and restart"));
    expect(screen.getByRole("button", {name: /Checking connections/})).toBeDisabled();
    await act(async () => complete({result: {success: false, description: "Database connection refused"}}));
    expect(errorWindow).toHaveBeenCalledWith("The configuration was not replaced: Database connection refused");
    expect(screen.getByRole("button", {name: "Submit and restart"})).toBeEnabled();
    expect(screen.getByLabelText("Database hostname")).toHaveValue("db-host:5433");
    expect(screen.getByLabelText("Database password:")).toHaveValue("Db-Secret7!");
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
