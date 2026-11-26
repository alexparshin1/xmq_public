/**
 * Where an extension's configurations belong in the menu.
 *
 * The library declares a machine-readable `page` (ABI 1.8). An operator can name a menu section in
 * the extension's config entry with `group`; that wins, so one installation can put two libraries
 * on the same page without rebuilding either. Both forms reduce to the same key: "Data
 * Integrations" and "data-integrations" are one section, not two.
 */

export function groupKey(value) {
    return String(value || "")
        .toLowerCase()
        .replace(/[^a-z0-9]+/g, "-")
        .replace(/^-+|-+$/g, "");
}

export function humanName(value) {
    return String(value || "")
        .replace(/[_-]+/g, " ")
        .replace(/\b\w/g, letter => letter.toUpperCase())
        .trim();
}

export function groupOf(extension) {
    return groupKey(extension && (extension.group || extension.page));
}

export function groupLabel(extension) {
    const group = extension && String(extension.group || "").trim();
    return group || humanName(extension && extension.page);
}

// The three sections that had hard-coded routes before groups came from configuration. Keeping
// their addresses means links written down before this change still open the same page.
const ROUTES = {
    "data-integrations": "/data-integrations",
    "authentication": "/authentication",
    "permissions": "/permissions"
};

export function groupPath(key) {
    return ROUTES[key] || "/configurations/" + key;
}
