import React from "react";

/**
 * The heading naming the page.
 *
 * A component rather than a heading written out on each screen, so that the pages cannot drift
 * apart again: they had accumulated an h2 on one and h3 on the rest, some with a trailing colon
 * and some without, each carrying its own inline alignment.
 *
 * It earns its line because the menu is an accordion and is usually collapsed, so nothing else on
 * screen says which page is open - which matters most on a tab left open for an hour, or in a
 * screenshot someone has been sent.
 */
export default function PageTitle({children}) {
    return <h3 style={{
        textAlign: "left",
        margin: "0 0 12px 0",
        fontWeight: "bold"
    }}>{children}</h3>;
}
