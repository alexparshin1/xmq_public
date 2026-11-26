import {errorWindow} from "../components/MessageWindow";
import InstalledExtensionsScreen from "./InstalledExtensionsScreen";

jest.mock("../components/MessageWindow", () => ({errorWindow: jest.fn()}));

/**
 * This page has Enable and Disable, not Start and Stop. What a configuration happens to be doing
 * belongs on its own page. A sink has no settings set here at all - it is chosen inside an export -
 * so it must not appear in a list whose only actions are switches.
 */
test("state says enabled or disabled, and sinks are not listed", () => {
    const page = new InstalledExtensionsScreen({});
    page.state = {
        extensions: [
            {name: "http_sink", sink: true, running: false, extension_enabled: false},
            {name: "ldap", running: true, extension_enabled: true},
            {name: "http-auth", running: false, extension_enabled: true, page: "authentication"}
        ],
        selected: "ldap",
        busy: false
    };

    expect(page.rows().map(row => row.name)).toEqual(["ldap", "http-auth"]);
    expect(page.rows()[0].state).toBe("enabled");
    // No group in the config: the library's declared page supplies it, as it does in the menu.
    expect(page.rows()[1].group).toBe("Authentication");
});

test("disabling a required extension says why it is refused", () => {
    const page = new InstalledExtensionsScreen({});
    page.state = {
        extensions: [{name: "user-database", running: true, extension_enabled: true, required: true}],
        selected: "user-database",
        busy: false
    };
    page.toggleExtension = jest.fn();

    page.disableSelected();

    expect(errorWindow).toHaveBeenCalledWith("This extension is required and can't be disabled.");
    expect(page.toggleExtension).not.toHaveBeenCalled();
});
