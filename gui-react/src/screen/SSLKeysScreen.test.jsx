import React from "react";
import {fireEvent, render, screen, waitFor} from "@testing-library/react";
import SSLKeysScreen from "./SSLKeysScreen";
import ControlAPI from "../ControlAPI";
import {errorWindow} from "../components/MessageWindow";

jest.mock("../components/MessageWindow", () => ({
    confirmationWindow: jest.fn(),
    errorWindow: jest.fn(),
    informationWindow: jest.fn()
}));

const keys = {
    cafile: "/certs/ca.pem", keyfile: "/certs/server.key", certfile: "/certs/server.pem",
    verify_depth: "3", identity_field: "san_uri", crlfile: "/certs/clients.crl.pem"
};

beforeEach(() => {
    jest.spyOn(ControlAPI, "asyncMakeAPICall").mockImplementation((method, request) =>
        Promise.resolve(method === "SSLKeysControl"
            ? {result: {success: true}, keys, peers: []}
            : {result: {success: true}}));
});
afterEach(() => { jest.restoreAllMocks(); jest.clearAllMocks(); });

test("loads the certificate identity and installed CRL path", async () => {
    render(<SSLKeysScreen/>);
    await waitFor(() => expect(screen.getByLabelText("Client identity:")).toHaveValue("san_uri"));
    expect(screen.getByText("installed: /certs/clients.crl.pem")).toBeInTheDocument();
});

test("can change identity policy without uploading server keys again", async () => {
    render(<SSLKeysScreen/>);
    await waitFor(() => expect(screen.getByLabelText("Client identity:")).toHaveValue("san_uri"));
    fireEvent.change(screen.getByLabelText("Client identity:"), {target: {value: "cn"}});
    fireEvent.click(screen.getByRole("button", {name: "Apply", exact: true}));
    await waitFor(() => expect(ControlAPI.asyncMakeAPICall).toHaveBeenCalledWith("SSLKeysControl", {
        action: "set",
        keys: {cafile: "", keyfile: "", certfile: "", crlfile: "", verify_depth: "3", identity_field: "cn"}
    }));
    expect(errorWindow).not.toHaveBeenCalled();
});
