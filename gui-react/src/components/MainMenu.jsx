import React, {useEffect, useMemo, useState} from "react";
import {useLocation, useNavigate} from "react-router-dom";
import ControlAPI from "../ControlAPI";
import {groupLabel, groupOf, groupPath} from "../extensionGroups";
import AccordionGroup from "./AccordionGroup";
import {documentationPagesIn} from "../documentation/pages";

/**
 * @brief The main menu, an accordion in the manner of the XMQ web site.
 *
 * One group is expanded at a time. Clicking the header of a collapsed group expands it and opens
 * the page last used in that group, or its first page; clicking the header of the expanded group
 * opens that same page again. Every group holds its own remembered page, so moving between groups
 * returns to where each was left rather than always to its first entry.
 *
 * The group holding the current page is the expanded one, whether the page was reached from this
 * menu or from a URL typed into the browser. That is what makes the menu a position indicator and
 * not just a list: a long Configuration group is only open while the user is in it.
 *
 * Groups appear and disappear with the connection, since pages behind it cannot be served without
 * a session, and documentation is available before signing in: installing and tuning the host are
 * done before there is anything to sign in to.
 */
export default function MainMenu() {
    const [connected, setConnected] = useState(() => ControlAPI.connected());

    /**
     * Follows the connection state, so the menu shows what can actually be reached.
     *
     * Subscribed on mount rather than while rendering, and unsubscribed on unmount: listeners are
     * kept in a list, so one registered per render would accumulate.
     */
    useEffect(() => {
        const unsubscribe = ControlAPI.setOnConnectedChange((isConnected) => {
            setConnected(isConnected);
        });
        setConnected(ControlAPI.connected());
        return unsubscribe;
    }, []);

    /**
     * Menu sections named by the extension configuration, not by this file. Asked of the broker
     * rather than read from a config file in the browser: the browser cannot see the broker's
     * files, and an extension that is installed but not enabled still has a section. While the
     * answer is in flight, and when the broker cannot answer at all, the familiar sections stay
     * in place - a menu that emptied out whenever the server was stopped would be unusable.
     */
    const [sections, setSections] = useState([]);
    useEffect(() => {
        if (!connected) {
            setSections([]);
            return undefined;
        }
        let live = true;
        ControlAPI.asyncMakeAPICall("ExtensionControl", {action: "list"}).then(data => {
            if (!live || !data || ControlAPI.apiError(data) || !Array.isArray(data.list)) {
                return;
            }
            const found = new Map();
            for (const extension of data.list) {
                const key = groupOf(extension);
                if (key && !found.has(key)) {
                    found.set(key, groupLabel(extension));
                }
            }
            setSections([...found].map(([key, label]) => ({key, label})));
        });
        return () => { live = false; };
    }, [connected]);

    const groups = useMemo(() => buildGroups(connected, sections), [connected, sections]);

    const location = useLocation();
    const navigate = useNavigate();

    // The first render already shows the right group open: a menu that appears empty for a frame
    // and then fills in is worse than one that paints once.
    const [expanded, setExpanded] = useState(
        () => (groups.find(group => location.pathname in group.items) ?? groups[0]).title);
    const [selectedLinks, setSelectedLinks] = useState({});

    // The group holding the current page opens, and remembers that page. Runs on the first render
    // and on every navigation, including ones that did not start in this menu.
    useEffect(() => {
        const group = groups.find(candidate => location.pathname in candidate.items);
        if (!group) {
            return;
        }
        setSelectedLinks(previous => previous[group.title] === location.pathname
            ? previous
            : {...previous, [group.title]: location.pathname});
        setExpanded(group.title);
    }, [location.pathname, groups]);

    const openLinkOf = (group) => selectedLinks[group.title] ?? Object.keys(group.items)[0];

    const onHeaderClick = (group) => {
        setExpanded(group.title);
        navigate(openLinkOf(group));
    };

    // A group can disappear with the connection (Configuration after signing out) while its title
    // is still the expanded one; fall back to the first group rather than render nothing open.
    const activeGroup = groups.some(group => group.title === expanded) ? expanded : groups[0].title;

    return <div className="MainMenu Accordion">
        {groups.map(group => {
            const open = group.title === activeGroup;
            const selectedLink = open && location.pathname in group.items
                ? location.pathname
                : selectedLinks[group.title];
            return <AccordionGroup key={group.title} title={group.title} items={group.items}
                                   expanded={open} selectedLink={selectedLink}
                                   onHeaderClick={() => onHeaderClick(group)}/>;
        })}
    </div>;
}

/**
 * @brief The menu groups for the current connection state, in display order.
 *
 * `sections` are the extension groups the broker reported. The pages that existed before groups
 * came from configuration are always present; a reported group with the same key does not replace
 * its label, and a new one is simply added.
 */
const KNOWN_SECTIONS = [
    {key: "data-integrations", label: "Data Integrations"},
    {key: "authentication", label: "Authentication"}
];

function buildGroups(connected, sections) {
    const loginGroup = {
        title: "System",
        // Signed out there is one thing to do; signed in, the same group signs out.
        items: {"/login": "Login"}
    };
    if (!connected) {
        return [loginGroup, documentationGroup(), extensionDocsGroup()];
    }

    const configurationItems = {
        // First, and named for what it is: it is where a new installation starts, and the
        // one entry here that replaces the configuration rather than editing part of it.
        "/setup": "Initial Setup",
        "/settings": "Settings",
        "/listeners": "Listeners",
        "/sslkeys": "SSL Keys",
        "/persistence": "Persistence",
        "/serverlimits": "Server Limits",
        "/logging": "Logging",
        "/prometheus": "Prometheus",
        "/service": "Service"
    };

    // Every page that edits an extension has its own group. The sections the broker reports are
    // added here, including ones this interface has never hard-coded, so a new settings family
    // needs no new route or menu entry in the code.
    const extensionItems = {"/extensions": "Installed Extensions"};
    // The known pages are always there. Permissions is a broker page under User Management, not an
    // extension group, so an extension that names it must not put a second link here.
    const allSections = [...KNOWN_SECTIONS];
    for (const section of sections) {
        if (section.key === "permissions") continue;
        if (!allSections.some(known => known.key === section.key)) allSections.push(section);
    }
    for (const section of allSections) {
        const path = groupPath(section.key);
        if (!(path in extensionItems)) extensionItems[path] = section.label;
    }

    // Its own group: the dashboard is where the server is watched and operated, which is a
    // different activity from editing what it will do when it next starts.
    return [
        {title: "System", items: {"/": "Logout"}},
        {title: "Operation", items: {"/dashboard": "Dashboard", "/sessions": "Sessions"}},
        {title: "Configuration", items: configurationItems},
        {title: "Extensions", items: extensionItems},
        {
            title: "User Management",
            items: {
                "/users": "Users",
                "/usergroups": "Groups",
                "/permissions": "Permissions",
                "/import": "Imports"
            }
        },
        {
            title: "Cluster",
            items: {
                "/bridges": "Bridges",
                "/cluster": "Cluster"
            }
        },
        documentationGroup(),
        extensionDocsGroup()
    ];
}

function documentationGroup() {
    return {
        title: "Documentation",
        items: Object.fromEntries(documentationPagesIn("Documentation").map(page => [page.path, page.label]))
    };
}

/**
 * @brief One documentation page per shipped extension.
 *
 * Named Extension Docs so it cannot be confused with the Extensions group above, where the pages
 * that *edit* extensions live. Signed out, these pages are as reachable as the manual: the choice
 * of a directory or an HTTP service is made before there is anything to sign in to.
 */
function extensionDocsGroup() {
    return {
        title: "Extension Docs",
        items: Object.fromEntries(documentationPagesIn("Extensions").map(page => [page.path, page.label]))
    };
}
