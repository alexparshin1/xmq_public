/**
 * The shape and meaning of an IP allow list, mirroring the server's IpWhitelist.
 *
 * The text is comma-separated IPv4 addresses, each optionally followed by a prefix length:
 * `10.1.1.1/24,10.1.2.123/32,10.1.1.242`. Empty means "no restriction". The server is the final
 * arbiter; these checks stop an obviously wrong value from reaching it.
 */

export const whitelistPattern = /^\s*(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}(\/\d{1,2})?)(\s*,\s*(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}(\/\d{1,2})?))*\s*$/;

export function isValidWhitelist(text) {
    if (text === null || text === undefined) return true;
    if (text.trim() === "") return true;
    if (!whitelistPattern.test(text)) return false;
    for (const entry of text.split(",")) {
        const trimmed = entry.trim();
        if (!trimmed) return false;
        const [ip, prefix] = trimmed.split("/");
        const octets = ip.split(".").map(Number);
        if (octets.length !== 4 || octets.some((o) => !Number.isInteger(o) || o < 0 || o > 255)) {
            return false;
        }
        if (prefix !== undefined) {
            const length = Number(prefix);
            if (!Number.isInteger(length) || length < 0 || length > 32) return false;
        }
    }
    return true;
}
