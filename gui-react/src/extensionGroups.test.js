import {groupKey, groupLabel, groupOf, groupPath} from "./extensionGroups";

test("a menu-section name and the library's page reduce to one key", () => {
    expect(groupKey("Data Integrations")).toBe("data-integrations");
    expect(groupKey("data-integrations")).toBe("data-integrations");
    expect(groupOf({group: "Data Integrations", page: "ignored"})).toBe("data-integrations");
    expect(groupOf({page: "authentication"})).toBe("authentication");
    expect(groupLabel({group: "Exports", page: "webhook"})).toBe("Exports");
    expect(groupLabel({page: "data-integrations"})).toBe("Data Integrations");
});

test("known sections keep their old addresses; new ones get a route", () => {
    expect(groupPath("data-integrations")).toBe("/data-integrations");
    expect(groupPath("authentication")).toBe("/authentication");
    expect(groupPath("permissions")).toBe("/permissions");
    expect(groupPath("exports")).toBe("/configurations/exports");
});
