import ClusterScreen from "./ClusterScreen";
import {confirmationWindow} from "../components/MessageWindow";

jest.mock("../components/DataTable", () => () => null);
jest.mock("../components/MessageWindow", () => ({
    confirmationWindow: jest.fn(), errorWindow: jest.fn(), informationWindow: jest.fn()
}));

beforeEach(() => jest.clearAllMocks());

test.each([false, true])("remove requires confirmation and sends force only as selected: %s", async (force) => {
    const screen = new ClusterScreen({});
    screen.state.selected = {node_id: "node-guid", name: "theater"};
    screen.send = jest.fn();
    confirmationWindow.mockResolvedValue(true);
    screen.removeNode(null, force);
    await Promise.resolve();
    expect(confirmationWindow).toHaveBeenCalledWith(expect.stringContaining("will not restart"));
    expect(screen.send).toHaveBeenCalledWith("remove", {
        node: {node_id: "node-guid", name: "theater"}, force
    }, expect.any(String));
});

test("cancelling removal leaves the configuration untouched", async () => {
    const screen = new ClusterScreen({});
    screen.state.selected = {node_id: "node-guid", name: "theater"};
    screen.send = jest.fn();
    confirmationWindow.mockResolvedValue(false);
    screen.removeNode();
    await Promise.resolve();
    expect(screen.send).not.toHaveBeenCalled();
});

test.each([["leaveCluster", "disable"], ["retireConfiguration", "retire"]])(
    "%s is a separate confirmed action", async (method, action) => {
        const screen = new ClusterScreen({});
        screen.send = jest.fn();
        confirmationWindow.mockResolvedValue(true);
        screen[method]();
        await Promise.resolve();
        expect(screen.send).toHaveBeenCalledWith(action, {}, expect.any(String));
    });
