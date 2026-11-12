import React, {useEffect, useMemo, useState} from "react";
import {useLocation, useNavigate} from "react-router-dom";
import ControlAPI from "../ControlAPI";
import AccordionGroup from "./AccordionGroup";
import {documentationPages} from "../documentation/pages";

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

    const groups = useMemo(() => buildGroups(connected), [connected]);

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
 */
function buildGroups(connected) {
    const loginGroup = {
        title: "System",
        // Signed out there is one thing to do; signed in, the same group signs out.
        items: {"/login": "Login"}
    };
    if (!connected) {
        return [loginGroup, documentationGroup()];
    }

    // Its own group: the dashboard is where the server is watched and operated, which is a
    // different activity from editing what it will do when it next starts.
    return [
        {title: "System", items: {"/": "Logout"}},
        {title: "Operation", items: {"/dashboard": "Dashboard", "/sessions": "Sessions"}},
        {
            title: "Configuration",
            items: {
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
                "/service": "Service",
                "/extensions": "Extensions"
            }
        },
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
        documentationGroup()
    ];
}

function documentationGroup() {
    return {
        title: "Documentation",
        items: Object.fromEntries(documentationPages.map(page => [page.path, page.label]))
    };
}
