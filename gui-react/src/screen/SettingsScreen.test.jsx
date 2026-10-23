import React from "react";
import {act, fireEvent, render, screen, waitFor} from "@testing-library/react";
import SettingsScreen from "./SettingsScreen";
import ControlAPI from "../ControlAPI";
import {confirmationWindow} from "../components/MessageWindow";
import MainMenu from "../components/MainMenu";
import {MemoryRouter} from "react-router-dom";

jest.mock("../components/MessageWindow", () => ({confirmationWindow: jest.fn()}));
const settings = {node_name: "thinker10", node_host: "thinker10", mqtt_port: 1883, mqtt_ssl_port: 8883,
    web_service_port: 18883, redis_uri: "redis://cache:*****@theater:6379", database_uri: "postgresql://xmq:*****@theater:5432/xmq",
    cluster_enabled: false, clean_start: false};
beforeEach(() => {
    jest.spyOn(ControlAPI, "asyncMakeAPICall").mockImplementation((method, request) => Promise.resolve(
        request.action === "get" ? {result: {success: true}, settings, pending: false, message: ""}
            : {result: {success: true}, settings, pending: request.action === "set", message: "Connections verified."}));
    confirmationWindow.mockResolvedValue(true);
});
afterEach(() => { jest.restoreAllMocks(); jest.clearAllMocks(); });

test("loads existing values, retains password masks, and tests without saving", async () => {
    const view = render(<SettingsScreen/>);
    await waitFor(() => expect(screen.getByLabelText("Node name:")).toHaveValue("thinker10"));
    expect(screen.getByLabelText("Redis password:")).toHaveValue("*****");
    expect(screen.getByLabelText("Database password:")).toHaveValue("*****");
    fireEvent.click(screen.getByRole("button", {name: "Test connections"}));
    await screen.findByText("Connections verified.");
    expect(ControlAPI.asyncMakeAPICall).toHaveBeenCalledWith("SettingsControl", {action: "test", settings: expect.objectContaining({
        redis_uri: settings.redis_uri, database_uri: settings.database_uri
    })});
    expect(confirmationWindow).not.toHaveBeenCalled();
    expect(ControlAPI.asyncMakeAPICall.mock.calls.some(([, body]) => body.action === "set")).toBe(false);
    view.unmount();
});

test("Settings follows Initial Setup in the configuration menu", () => {
    jest.spyOn(ControlAPI, "connected").mockReturnValue(true);
    render(<MemoryRouter><MainMenu/></MemoryRouter>);
    const links = screen.getAllByRole("link");
    const at = links.findIndex(link => link.textContent === "Initial Setup");
    expect(at).toBeGreaterThanOrEqual(0);
    expect(links[at + 1]).toHaveTextContent("Settings");
    expect(links[at + 1]).toHaveAttribute("href", "/settings");
});

test("a database password change is encoded and submitted without resetting users", async () => {
    const view = render(<SettingsScreen/>);
    await waitFor(() => expect(screen.getByLabelText("Node name:")).toHaveValue("thinker10"));
    fireEvent.change(screen.getByLabelText("Database password:"), {target: {value: "New:s@cret7!"}});
    fireEvent.click(screen.getByRole("button", {name: "Save and restart MQTT"}));
    await waitFor(() => expect(ControlAPI.asyncMakeAPICall).toHaveBeenCalledWith("SettingsControl", {
        action: "set", settings: expect.objectContaining({database_uri: "postgresql://xmq:New%3As%40cret7!@theater:5432/xmq"})
    }));
    expect(ControlAPI.asyncMakeAPICall.mock.calls.some(([method]) => method === "InitialSetupControl")).toBe(false);
    view.unmount();
});

test("failed connection checks leave the edited form available", async () => {
    render(<SettingsScreen/>);
    await waitFor(() => expect(screen.getByLabelText("Node name:")).toHaveValue("thinker10"));
    ControlAPI.asyncMakeAPICall.mockResolvedValue({result: {success: false, description: "Database connection refused"}});
    fireEvent.click(screen.getByRole("button", {name: "Save and restart MQTT"}));
    await waitFor(() => expect(screen.getByRole("alert")).toHaveTextContent("Database connection refused"));
    expect(screen.getByLabelText("Database host:")).toHaveValue("theater");
    expect(screen.getByRole("button", {name: "Save and restart MQTT"})).toBeEnabled();
});

test("cancelling a save sends no update and Clean start is named in confirmation", async () => {
    ControlAPI.asyncMakeAPICall.mockResolvedValue({result: {success: true}, settings: {...settings, clean_start: true}});
    render(<SettingsScreen/>);
    await waitFor(() => expect(screen.getByLabelText("Node name:")).toHaveValue("thinker10"));
    confirmationWindow.mockResolvedValue(false);
    fireEvent.click(screen.getByRole("button", {name: "Save and restart MQTT"}));
    await act(async () => {});
    expect(confirmationWindow).toHaveBeenCalledWith(expect.stringContaining("Clean start is enabled"));
    expect(ControlAPI.asyncMakeAPICall.mock.calls.some(([, body]) => body.action === "set")).toBe(false);
});
