import ControlAPI from "./ControlAPI";

const KEY = "xmq.control.session";

beforeEach(() => {
    window.sessionStorage.clear();
    ControlAPI.token = "";
    ControlAPI.setupRequired = false;
    jest.restoreAllMocks();
});

afterEach(() => {
    window.sessionStorage.clear();
});

test("signing in keeps the token, and signing out forgets it", async () => {
    jest.spyOn(ControlAPI, "asyncMakeAPICall")
        .mockResolvedValue({token: "abc123", result: {success: true}});

    expect(await ControlAPI.login("admin", "secret")).toBe(true);
    expect(ControlAPI.connected()).toBe(true);
    expect(JSON.parse(window.sessionStorage.getItem(KEY)).token).toBe("abc123");

    ControlAPI.logout();
    expect(ControlAPI.connected()).toBe(false);
    expect(window.sessionStorage.getItem(KEY)).toBeNull();
});

test("a stored session is restored when the interface loads", () => {
    window.sessionStorage.setItem(KEY, JSON.stringify({token: "saved", setupRequired: false}));

    let restored;
    jest.isolateModules(() => {
        restored = require("./ControlAPI").default;
    });

    expect(restored.token).toBe("saved");
    expect(restored.connected()).toBe(true);
});
