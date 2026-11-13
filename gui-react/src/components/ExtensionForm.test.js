import {extensionForm, conditionMatches} from "./ExtensionForm";

const extension = {
    settings: [
        {name: "kind", type: "choice", choices: "ldap,ad"},
        {name: "host", type: "string"},
        {name: "password", type: "secret"}
    ],
    presets: [{name: "fallback"}]
};
const definition = {
    version: 1,
    sections: [{title: "Connection", fields: [
        {name: "kind"},
        {name: "host", visible_when: {setting: "kind", values: ["ad"]}}
    ]}],
    presets: [{name: "ad", choice_setting: "kind", choice_value: "ad",
               values: {kind: "ad", host: "dc.example.org"}}]
};
const parse = value => extensionForm({...extension, ui_definition: JSON.stringify(value)});

test("a data-only form and name-to-value presets are normalized", () => {
    const {form, error} = parse(definition);
    expect(error).toBeUndefined();
    expect(form.presets[0].settings).toEqual([
        {name: "kind", value: "ad"}, {name: "host", value: "dc.example.org"}
    ]);
    expect(conditionMatches(form.sections[0].fields[1].visible_when, () => "ldap")).toBe(false);
    expect(conditionMatches(form.sections[0].fields[1].visible_when, () => "ad")).toBe(true);
    expect(conditionMatches(undefined, () => "ldap")).toBe(true);
});

test.each([
    {...definition, version: 2},
    {...definition, script: "alert(1)"},
    {...definition, sections: [{title: "Bad", fields: [{name: "unknown"}]}]},
    {...definition, sections: [{title: "Bad", fields: [{name: "host"}, {name: "host"}]}]},
    {...definition, sections: [{title: "Bad", fields: [{name: "host", widget: "html"}]}]},
    {...definition, sections: [{title: "Bad", fields: [{name: "host",
        visible_when: {setting: "password", values: ["secret"]}}]}]},
    {...definition, presets: [{name: "bad", values: {password: "secret"}}]},
    {...definition, presets: [{name: "bad", values: {unknown: "value"}}]},
    {...definition, presets: [{name: "bad", choice_setting: "kind", choice_value: "other", values: {}}]}
])("invalid or incompatible descriptions fall back without executable code", value => {
    expect(parse(value).form).toBeUndefined();
    expect(parse(value).error).toMatch(/standard settings form/);
});

test("missing, unreadable, oversized and malformed descriptions are safe", () => {
    expect(extensionForm(extension)).toEqual({});
    expect(extensionForm({...extension, ui_error: "Cannot read"}).error).toBe("Cannot read");
    expect(extensionForm({...extension, ui_definition: "{"}).error).toBeDefined();
    expect(extensionForm({...extension, ui_definition: " ".repeat(65537)}).error).toBeDefined();
});
