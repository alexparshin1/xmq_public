/** Versioned, data-only presentation. No expressions, HTML, URLs or executable code. */
export function extensionForm(extension) {
    if (extension.ui_error) return {error: extension.ui_error};
    if (!extension.ui_definition) return {};
    try {
        const object = value => value && typeof value === "object" && !Array.isArray(value);
        const check = (ok, message) => { if (!ok) throw new Error(message); };
        const keys = (value, allowed) => {
            check(object(value), "Expected an object");
            check(Object.keys(value).every(key => allowed.includes(key)), "Unknown UI property");
        };
        const text = value => typeof value === "string" && value.length <= 4096;
        const settings = new Map((extension.settings || [])
            .filter(setting => setting.declared !== false).map(setting => [setting.name, setting]));
        const known = name => text(name) && settings.has(name);
        const condition = value => {
            if (value === undefined) return;
            keys(value, ["setting", "values"]);
            check(known(value.setting) && settings.get(value.setting).type !== "secret",
                  "A condition must reference a declared, non-secret setting");
            check(Array.isArray(value.values) && value.values.length > 0 && value.values.every(text),
                  "A condition needs string values");
        };
        check(extension.ui_definition.length <= 65536, "UI description exceeds 64 KiB");
        const form = JSON.parse(extension.ui_definition);
        keys(form, ["version", "sections", "presets"]);
        check(form.version === 1, "Unsupported UI description version");
        check(Array.isArray(form.sections), "Sections must be an array");
        const seen = new Set();
        form.sections.forEach(section => {
            keys(section, ["title", "description", "fields"]);
            check(text(section.title) && section.title.length > 0, "A section needs a title");
            check(section.description === undefined || text(section.description), "Invalid section description");
            check(Array.isArray(section.fields), "Section fields must be an array");
            section.fields.forEach(field => {
                keys(field, ["name", "visible_when", "enabled_when", "widget"]);
                check(known(field.name) && !seen.has(field.name), "Unknown or duplicate field");
                seen.add(field.name);
                condition(field.visible_when);
                condition(field.enabled_when);
                check(field.widget === undefined ||
                      (field.widget === "multiline" && settings.get(field.name).type === "string"),
                      "Unsupported field widget");
            });
        });
        const names = new Set();
        const presets = form.presets === undefined ? extension.presets || [] : form.presets;
        check(Array.isArray(presets), "Presets must be an array");
        if (form.presets !== undefined) form.presets = presets.map(preset => {
            keys(preset, ["name", "label", "description", "values", "choice_setting", "choice_value"]);
            check(text(preset.name) && preset.name.length > 0 && !names.has(preset.name), "Invalid preset name");
            names.add(preset.name);
            check(preset.label === undefined || text(preset.label), "Invalid preset label");
            check(preset.description === undefined || text(preset.description), "Invalid preset description");
            check(object(preset.values), "Preset values must be an object");
            const values = Object.entries(preset.values);
            check(values.every(([name, value]) => known(name) && text(value) &&
                  !["__proto__", "constructor", "prototype"].includes(name) &&
                  settings.get(name).type !== "secret"), "Preset contains an unknown setting or a secret");
            if (preset.choice_setting !== undefined || preset.choice_value !== undefined) {
                check(known(preset.choice_setting) && settings.get(preset.choice_setting).type === "choice" &&
                      text(preset.choice_value) &&
                      (settings.get(preset.choice_setting).choices || "").split(",")
                          .map(value => value.trim()).includes(preset.choice_value) &&
                      preset.values[preset.choice_setting] === preset.choice_value,
                      "Invalid preset choice binding");
            }
            return {...preset, settings: values.map(([name, value]) => ({name, value}))};
        });
        return {form};
    } catch (error) {
        return {error: error.message + ". Using the standard settings form."};
    }
}

export function conditionMatches(condition, valueOf) {
    return !condition || condition.values.includes(String(valueOf(condition.setting)));
}
