import React from "react";

/**
 * Formats a byte count for reading at a glance.
 * @param {?number} bytes  Byte count, or null when unknown.
 */
export function formatBytes(bytes) {
    if (bytes === null || bytes === undefined) {
        return "—";
    }
    const units = ["B", "KB", "MB", "GB", "TB"];
    let value = bytes;
    let unit = 0;
    while (value >= 1024 && unit < units.length - 1) {
        value /= 1024;
        unit++;
    }
    return `${value.toFixed(value >= 100 || unit === 0 ? 0 : 1)} ${units[unit]}`;
}

/**
 * Formats a duration in seconds as days, hours and minutes.
 * @param {?number} seconds  Duration, or null when unknown.
 */
export function formatDuration(seconds) {
    if (seconds === null || seconds === undefined) {
        return "—";
    }
    const days = Math.floor(seconds / 86400);
    const hours = Math.floor((seconds % 86400) / 3600);
    const minutes = Math.floor((seconds % 3600) / 60);
    if (days > 0) {
        return `${days}d ${hours}h`;
    }
    if (hours > 0) {
        return `${hours}h ${minutes}m`;
    }
    return `${minutes}m ${Math.floor(seconds % 60)}s`;
}

/**
 * Formats a count with thousands separators, or a dash when unknown.
 */
export function formatCount(value) {
    if (value === null || value === undefined) {
        return "—";
    }
    return Number(value).toLocaleString();
}

/**
 * A horizontal bar showing a proportion.
 *
 * Drawn rather than written because a proportion is what the eye reads fastest: "how full" is the
 * question being asked of memory and disk, and a number answers it only after arithmetic. The
 * figures stay alongside, since the bar cannot say how much is left in gigabytes.
 *
 * @param {number} fraction  Filled part, 0..1.
 * @param {string} caption   Text shown above the bar.
 * @param {string} detail    Text shown at the right of the caption.
 */
export function Gauge({fraction, caption, detail}) {
    const clamped = Math.max(0, Math.min(1, Number.isFinite(fraction) ? fraction : 0));
    // Green until it matters, amber when it starts to, red when it is nearly gone. The thresholds
    // are the points at which someone should look, not arbitrary quarters.
    const colour = clamped >= 0.9 ? "#c62828" : (clamped >= 0.75 ? "#ef6c00" : "#2e7d32");

    return <div style={{margin: "6px 0"}}>
        <div style={{display: "flex", justifyContent: "space-between", fontSize: "0.85em", color: "#555"}}>
            <span>{caption}</span>
            <span>{detail}</span>
        </div>
        <div style={{background: "#e8e8e8", borderRadius: "3px", height: "10px", overflow: "hidden"}}>
            <div style={{
                width: `${(clamped * 100).toFixed(1)}%`,
                background: colour,
                height: "100%",
                transition: "width 0.4s ease"
            }}/>
        </div>
    </div>;
}

/**
 * A labelled figure.
 */
export function Stat({label, value, hint}) {
    return <div style={{display: "flex", justifyContent: "space-between", padding: "3px 0", gap: "12px"}}>
        <span style={{color: "#555"}}>{label}</span>
        <span style={{fontWeight: "bold", textAlign: "right"}} title={hint || ""}>{value}</span>
    </div>;
}

/**
 * A titled box grouping related figures.
 *
 * Panels sit side by side and wrap, so the dashboard reads on a laptop as well as on a wide
 * monitor without any fixed layout to maintain.
 */
export default function StatPanel({title, children, note}) {
    return <div style={{
        border: "1px solid #d8d8d8",
        borderRadius: "6px",
        padding: "10px 14px",
        minWidth: "260px",
        flex: "1 1 260px",
        background: "#fcfcfc"
    }}>
        <div style={{fontWeight: "bold", marginBottom: "6px"}}>{title}</div>
        {children}
        {note ? <div style={{color: "#888", fontSize: "0.82em", marginTop: "6px"}}>{note}</div> : null}
    </div>;
}
